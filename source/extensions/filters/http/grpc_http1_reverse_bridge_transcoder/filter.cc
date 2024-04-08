#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "source/common/grpc/codec.h"
#include "source/common/grpc/common.h"
#include "source/common/grpc/status.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"

#include "filter.h"
#include "http_methods.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {
namespace {
// Internal constants
namespace Errors {
// NOTE: Avoid whitespaces in error messages. They trigger an assertion on sending.
const auto UnexpectedMethodType = "HTTP_method_type_is_unexpected";
const auto UnexpectedRequestPath = "HTTP_request_path_is_unexpected";
const auto UnexpectedContentType = "HTTP_header_contains_unexpected_content_type";
const auto GrpcUnexpectedRequestPath = "gRPC_request_path_is_unexpected";
const auto GrpcFrameTooSmall = "gRPC_Frame_content_is_too_small";
const auto GrpcToJsonFailed = "Failed_to_transcode_gRPC_to_JSON";
const auto JsonToGrpcFailed = "Failed_to_transcode_JSON_to_gRPC";
const auto ResponseNotOkay = "HTTP_response_status_code_is_not_okay";
const auto ResponseHeaderOnly = "HTTP_response_is_header_only";
const auto BufferExceedsLimitError = "Buffered_data_exceeds_configured_limit";
const auto InternalError = "Internal_Error_in_Plugin_occurred";
} // namespace Errors

// Internal functions
void clearBuffer(Buffer::Instance& buffer) { buffer.drain(buffer.length()); }

void replaceBufferWithGrpcMessage(Buffer::Instance& buffer, std::string& payload) {
  using GrpcFrameHeader = std::array<uint8_t, Grpc::GRPC_FRAME_HEADER_SIZE>;

  GrpcFrameHeader header;
  Grpc::Encoder().newFrame(Grpc::GRPC_FH_DEFAULT, payload.size(), header);

  clearBuffer(buffer);
  buffer.add(header.data(), header.size());
  buffer.add(payload);
}

Grpc::Status::GrpcStatus grpcStatusFromHttpStatus(uint64_t http_status) {
  // For some odd reason, envoys HTTP to gRPC return code conversion does not
  // support okay results, only the other way around. Add this mapping.
  static auto const http_status_ok =
      Grpc::Utility::grpcToHttpStatus(Grpc::Status::WellKnownGrpcStatus::Ok);

  if (http_status != http_status_ok) {
    return Grpc::Utility::httpToGrpcStatus(http_status);
  }
  return Grpc::Status::WellKnownGrpcStatus::Ok;
}
} // namespace

Filter::Filter(Api::Api& api, std::string proto_descriptor_path, std::string service_name)
    : transcoder_{}, grpc_sessions_{} {
  auto const status = transcoder_.init(api, proto_descriptor_path, service_name);
  assert(status.ok());
}

/////////////////////////////////////////////////////////////////
// Implementation Http::StreamDecoderFilter: gRPC -> http/JSON //
/////////////////////////////////////////////////////////////////
Http::FilterHeadersStatus Filter::decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) {
  // Try to create new session. Respond with an error message is this fails.
  // Short circuit if header only.
  if (end_stream) {
    ENVOY_STREAM_LOG(debug,
                     "Header only request received. This cannot be a gRPC Request. Forward "
                     "request headers unmodified.",
                     *decoder_callbacks_);

    return Http::FilterHeadersStatus::Continue;
  }

  // Disable Transcoding if disable for this Route
  if (decoder_callbacks_->route() != nullptr) {
    const auto* per_route_config =
        Http::Utility::resolveMostSpecificPerFilterConfig<FilterConfigPerRoute>(decoder_callbacks_);

    if (per_route_config != nullptr && per_route_config->disabled()) {
      ENVOY_STREAM_LOG(
          debug, "Transcoding is disabled for this route. Forwarded request headers unmodified.",
          *decoder_callbacks_);
      return Http::FilterHeadersStatus::Continue;
    }
  }

  // If this isn't a gRPC request: Pass through
  if (!Envoy::Grpc::Common::isGrpcRequestHeaders(headers)) {
    ENVOY_STREAM_LOG(debug,
                     "Requests content-type header value is not 'application/grpc'. Forward "
                     "request headers unmodified.",
                     *decoder_callbacks_);
    return Http::FilterHeadersStatus::Continue;
  }

  // From here on, this Request must be transcoded. Create session.
  auto session_guard = SessionGuard(grpc_sessions_);
  auto session_status = session_guard.createSession(decoder_callbacks_->streamId());
  if (!session_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Unable to create session. Send gRPC error message downstream. Error was: {}",
                     *decoder_callbacks_, session_status.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::InternalError);
    return Http::FilterHeadersStatus::StopIteration;
  }
  auto* const session = *session_status;

  auto const method_status = httpMethodFrom(headers.getMethodValue());
  if (!method_status.ok()) {
    ENVOY_STREAM_LOG(
        error,
        "Failed to construct HTTP Method from header method value. Destroy session and "
        "send gRPC error message downstream. Error was: {}",
        *decoder_callbacks_, method_status.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::UnexpectedMethodType);
    return Http::FilterHeadersStatus::StopIteration;
  }
  auto path = static_cast<std::string>(headers.getPathValue());
  session->method_and_path = HttpMethodAndPath{*method_status, std::move(path)};

  auto const transcoder_status = transcoder_.prepareTranscoding(session->method_and_path);
  if (!transcoder_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to prepare Transcoder from HTTP Method and Path. Destroy session and "
                     "send gRPC error message downstream. Error was: {}",
                     *decoder_callbacks_, transcoder_status.message());

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcUnexpectedRequestPath);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Rewrite HTTP Headers
  auto path_status = transcoder_.getHttpRequestPath();
  if (!path_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Transcoder failed to determine new HTTP Request path. Destroy session and "
                     "send gRPC error message downstream. Error was: {}",
                     *decoder_callbacks_, path_status.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::UnexpectedRequestPath);
    return Http::FilterHeadersStatus::StopIteration;
  }

  headers.setEnvoyOriginalPath(headers.getPathValue());
  headers.setPath(std::move(*path_status));
  headers.setContentType(Http::Headers::get().ContentTypeValues.Json);
  headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Json);
  headers.removeTE();

  // NOTE: Content length handling. Since we don't know the content length
  // before body transcoding. Memorize a pointer to the header map and use it
  // in decodeData().
  session->decoder_headers = &headers;
  decoder_callbacks_->downstreamCallbacks()->clearRouteCache();

  // TODO: Handle HttpBody Messages
  session_guard.keepAccessedSessionsAlive();
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus Filter::decodeData(Buffer::Instance& buffer, bool end_stream) {
  // NOTE: Buffering behavior. Envoy usually passes buffers chunk wise to the filter chains and
  // assembles the contents in its own internal buffer. These Fragments are passed down the filter
  // chain. In our use case, we don't want this behavior. Instead we copy the streamed data chunks
  // in our own internal buffer and convert the entire buffer at the end of the stream to pass the
  // result further. To achieve this the return code "StopIterationNoBuffer" disables the internal
  // buffering and "Continue" is used to pass on the contents "buffer".
  auto session_guard = SessionGuard(grpc_sessions_);
  auto session_status = session_guard.lookupSession(decoder_callbacks_->streamId());
  if (!session_status.ok()) {
    ENVOY_STREAM_LOG(debug,
                     "No gRPC Session found for this stream. Forwarded request data unmodified.",
                     *decoder_callbacks_);
    return Http::FilterDataStatus::Continue;
  }
  auto* const session = *session_status;

  if (buffer.length()) {
    ENVOY_STREAM_LOG(debug, "Add {} bytes to decoder buffer.", *decoder_callbacks_,
                     buffer.length());
    session->decoder_data.add(buffer);
  }

  if (decoder_callbacks_->decoderBufferLimit() < session->decoder_data.length()) {
    ENVOY_STREAM_LOG(error,
                     "Buffered data exceed configured buffer limits. Destroy session and "
                     "send gRPC error message downstream.",
                     *decoder_callbacks_);

    respondWithGrpcError(*decoder_callbacks_, Errors::BufferExceedsLimitError);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (!end_stream) {
    ENVOY_STREAM_LOG(debug, "End of stream is not reached. Return and wait for more data.",
                     *decoder_callbacks_);

    session_guard.keepAccessedSessionsAlive();
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // From here on the entire data stream is collected and ready for transcoding.
  // Strip gRPC Header from buffer and try to transcode buffered data.
  if (session->decoder_data.length() < Grpc::GRPC_FRAME_HEADER_SIZE) {
    ENVOY_STREAM_LOG(error,
                     "gRPC request data frame contains too few bytes to be a gRPC request. Destroy "
                     "Session and send gRPC error message downstream.",
                     *decoder_callbacks_);

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcFrameTooSmall);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  session->decoder_data.drain(Grpc::GRPC_FRAME_HEADER_SIZE);

  // From here on the entire data stream is collected and ready for transcoding, prepare it.
  auto const transcoder_status = transcoder_.prepareTranscoding(session->method_and_path);
  if (!transcoder_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to prepare Transcoder from HTTP Method and Path. Destroy session and "
                     "send gRPC error message downstream. Error was: {}",
                     *decoder_callbacks_, transcoder_status.message());

    respondWithGrpcError(*decoder_callbacks_, Errors::InternalError);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  auto json_status = transcoder_.grpcRequestToJson(session->decoder_data.toString());
  clearBuffer(session->decoder_data);

  // If transcoding fails: Respond to initial sender with a http message containing an error.
  if (!json_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to transcode http request from gRPC to JSON. Destroy session and gRPC "
                     "error message downstream. Error was: '{}'",
                     *decoder_callbacks_, json_status.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcToJsonFailed);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  ENVOY_STREAM_LOG(debug, "Transcodeded http request from gRPC to JSON", *decoder_callbacks_);

  // Replace buffer contents with transcoded JSON string
  clearBuffer(buffer);
  buffer.add(*json_status);

  // Rewrite headers content length with the size of the buffers contents.
  session->decoder_headers->setContentLength(buffer.length());

  session_guard.keepAccessedSessionsAlive();
  return Http::FilterDataStatus::Continue;
}

////////////////////////////////////////////////////////////////
// Implementation Http::StreamEncoderFilter: http/JSON-> gRPC //
////////////////////////////////////////////////////////////////
Http::FilterHeadersStatus Filter::encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) {
  auto session_guard = SessionGuard(grpc_sessions_);
  auto session_status = session_guard.lookupSession(encoder_callbacks_->streamId());

  if (!session_status.ok()) {
    ENVOY_STREAM_LOG(
        debug, "No gRPC Session found for this stream. Forwarded response headers unmodified.",
        *decoder_callbacks_);
    return Http::FilterHeadersStatus::Continue;
  }
  auto* const session = *session_status;

  // Map HTTP Status to gRPC status. In case of an error, send a reply to the downstream host.
  const auto http_status = Http::Utility::getResponseStatus(headers);
  const auto grpc_status = grpcStatusFromHttpStatus(http_status);
  if (grpc_status != Grpc::Status::WellKnownGrpcStatus::Ok) {
    ENVOY_STREAM_LOG(error,
                     "Response contained HTTP status code {}. Destroy session and send gRPC error "
                     "message with converted status code downstream.",
                     *encoder_callbacks_, http_status);

    respondWithGrpcError(*encoder_callbacks_, Errors::ResponseNotOkay, grpc_status);
    return Http::FilterHeadersStatus::StopIteration;
  };

  if (end_stream) {
    ENVOY_STREAM_LOG(error,
                     "Received HTTP header only response. This is unexpected for HTTP/JSON "
                     "responses. Destroy session and send gRPC error downstream.",
                     *encoder_callbacks_);

    respondWithGrpcError(*encoder_callbacks_, Errors::ResponseHeaderOnly);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // TODO: Add support HttpBody Messages
  auto content_type = headers.getContentTypeValue();
  if (content_type != Http::Headers::get().ContentTypeValues.Json) {
    ENVOY_STREAM_LOG(error,
                     "Received HTTP response does not containing JSON payload. Content type is "
                     "unsupported. Destroy session and send gRPC error downstream.",
                     *encoder_callbacks_);

    respondWithGrpcError(*encoder_callbacks_, Errors::UnexpectedContentType);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Modify Headers and proceed.
  headers.setContentType(Http::Headers::get().ContentTypeValues.Grpc);
  headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Grpc);

  // NOTE: Content length handling. Since we don't know the content length
  // before body transcoding. Memorize a pointer to the header map and use it
  // in decodeData().
  session->encoder_headers = &headers;
  session_guard.keepAccessedSessionsAlive();
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus Filter::encodeData(Buffer::Instance& buffer, bool end_stream) {
  // NOTE: Buffering behavior. Envoy usually passes buffers chunk wise to the filter chains and
  // assembles the contents in its own internal buffer. These Fragments are passed down the filter
  // chain. In our use case, we don't want this behavior. Instead we copy the streamed data chunks
  // in our own internal buffer and convert the entire buffer at the end of the stream to pass the
  // result further. To achieve this the return code "StopIterationNoBuffer" disables the internal
  // buffering and "Continue" is used to pass on the contents "buffer".
  auto session_guard = SessionGuard(grpc_sessions_);
  auto session_status = session_guard.lookupSession(encoder_callbacks_->streamId());
  if (!session_status.ok()) {
    ENVOY_STREAM_LOG(debug,
                     "No gRPC Session found for this stream. Forwarded response data unmodified.",
                     *decoder_callbacks_);
    return Http::FilterDataStatus::Continue;
  }
  auto* const session = *session_status;

  if (buffer.length()) {
    ENVOY_STREAM_LOG(debug, "Add {} bytes to encoder buffer.", *encoder_callbacks_,
                     buffer.length());
    session->encoder_data.add(buffer);
  }

  if (encoder_callbacks_->encoderBufferLimit() < session->encoder_data.length()) {
    ENVOY_STREAM_LOG(error,
                     "Buffered data exceed configured buffer limits. Destroy session and "
                     "send gRPC error message downstream.",
                     *encoder_callbacks_);

    respondWithGrpcError(*encoder_callbacks_, Errors::BufferExceedsLimitError);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (!end_stream) {
    ENVOY_STREAM_LOG(debug, "End of stream is not reached. Return and wait for more data.",
                     *encoder_callbacks_);

    session_guard.keepAccessedSessionsAlive();
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // From here on the entire data stream is collected and ready for transcoding, prepare it.
  auto const transcoder_status = transcoder_.prepareTranscoding(session->method_and_path);
  if (!transcoder_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to prepare Transcoder from HTTP Method and Path. Destroy session and "
                     "send gRPC error message downstream. Error was: {}",
                     *encoder_callbacks_, transcoder_status.message());

    respondWithGrpcError(*encoder_callbacks_, Errors::InternalError);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  auto grpc_status = transcoder_.jsonResponseToGrpc(session->encoder_data.toString());
  if (!grpc_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to transcode http response from JSON to gRPC. Destroy session and "
                     "send gRPC error message downstream. Error was: '{}'",
                     *encoder_callbacks_, grpc_status.status().message());

    respondWithGrpcError(*encoder_callbacks_, Errors::JsonToGrpcFailed);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  ENVOY_STREAM_LOG(debug, "Transcodeded http response from JSON to gRPC", *encoder_callbacks_);

  // Replace buffer contents with transcoded gRPC message then update header and trailers
  replaceBufferWithGrpcMessage(buffer, *grpc_status);

  // Elide gRPC response status code from HTTP status code
  auto& trailers = encoder_callbacks_->addEncodedTrailers();
  auto const http_status = Http::Utility::getResponseStatus(*(session->encoder_headers));
  trailers.setGrpcStatus(grpcStatusFromHttpStatus(http_status));
  session->encoder_headers->setContentLength(buffer.length());

  ENVOY_STREAM_LOG(debug, "Processed Session successfully. Destroy session.", *encoder_callbacks_);
  return Http::FilterDataStatus::Continue;
}

template <class CallbackType>
void Filter::respondWithGrpcError(CallbackType& callback_type, const std::string_view description,
                                  Grpc::Status::GrpcStatus grpcStatus) {
  // Send a gRPC response indicating an error. Despite propagating an error the
  // underlying HTTP Response is still well formed.
  // Since we are transcoding here, the only gRPC status code that somehow fits is "Unknown".
  callback_type.sendLocalReply(Http::Code::OK, description, nullptr, grpcStatus, description);
}

template <class CallbackType>
void Filter::respondWithGrpcError(CallbackType& callback_type, const std::string_view description) {
  respondWithGrpcError(callback_type, description, Grpc::Status::WellKnownGrpcStatus::Unknown);
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
