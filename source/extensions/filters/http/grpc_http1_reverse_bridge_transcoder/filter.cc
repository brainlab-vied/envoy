#include "envoy/http/filter.h"
#include "absl/status/status.h"
#include "envoy/http/header_map.h"

#include "source/common/grpc/codec.h"
#include "source/common/grpc/common.h"
#include "source/common/grpc/status.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"

#include "filter.h"
#include "http_methods.h"

// TODO: Rename values to StatusOr Types to recommended _or notation
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
//
namespace ContentTypes {
const auto Grpc = Http::Headers::get().ContentTypeValues.Grpc;
const auto Json = Http::Headers::get().ContentTypeValues.Json;
} // namespace ContentTypes

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
  if (!status.ok()) {
    ENVOY_LOG(critical, "Failed to intialize transcoder. Error was: {}", status.message());
    assert(false);
  }
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

  // Transform shared HTTP Headers
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
  headers.removeTE();

  // Transform HTTP Headers by type.
  auto const transcoding_type_status = transcoder_.mapRequestTo();
  if (!transcoding_type_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to lookup transcoding type. Destroy session and "
                     "send gRPC error message downstream. Error was: {}",
                     *decoder_callbacks_, transcoding_type_status.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::InternalError);
    return Http::FilterHeadersStatus::StopIteration;
  }
  switch (*transcoding_type_status) {
  case TranscodingType::HttpJson: {
    ENVOY_STREAM_LOG(debug, "Transcode request to HTTP/JSON. Set ContentType Header.",
                     *decoder_callbacks_);
    headers.setContentType(ContentTypes::Json);
  } break;

  case TranscodingType::HttpBody:
    ENVOY_STREAM_LOG(debug, "Transcode request to HTTP/BODY. Remove ContentType header for now.",
                     *decoder_callbacks_);
    headers.removeContentType();
    break;
  };

  // NOTE: Content length handling. Since we don't know the content length
  // before body transcoding. Memorize a pointer to the header map and use it
  // in decodeData().
  session->decoder_headers = &headers;
  decoder_callbacks_->downstreamCallbacks()->clearRouteCache();
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
  // In case transcoding fails for any reason: Send gRPC error message downstream and
  // cleanup session.
  auto const status = transcodeRequest(*session, buffer);
  if (!status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "gRPC transcoding failed. Destroy "
                     "Session and send gRPC error message downstream. Error was: {}",
                     *decoder_callbacks_);
    respondWithGrpcError(*decoder_callbacks_, status.message());
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // Transcoding was successful. Free this sessions buffer and keep session
  // alive for a potential response as it is reused for a response.
  clearBuffer(session->decoder_data);
  session_guard.keepAccessedSessionsAlive();
  return Http::FilterDataStatus::Continue;
}

absl::Status Filter::transcodeRequest(Session& session, Buffer::Instance& outgoing_buffer) {
  // Strip gRPC Header from buffer
  if (session.decoder_data.length() < Grpc::GRPC_FRAME_HEADER_SIZE) {
    ENVOY_STREAM_LOG(
        error,
        "gRPC request data frame contains too few bytes to be a gRPC request. Abort Transcoding.",
        *decoder_callbacks_);
    return absl::OutOfRangeError(Errors::GrpcFrameTooSmall);
  }
  session.decoder_data.drain(Grpc::GRPC_FRAME_HEADER_SIZE);

  // Prepare Transcoding.
  auto const transcoder_status = transcoder_.prepareTranscoding(session.method_and_path);
  if (!transcoder_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to prepare Transcoder from HTTP Method and Path. Abort Transcoding. "
                     "Error was: {}",
                     *decoder_callbacks_, transcoder_status.message());
    return absl::InternalError(Errors::InternalError);
  }

  // Transcode buffer depending on its type
  auto const transcoding_type_status = transcoder_.mapRequestTo();
  if (!transcoding_type_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to lookup transcoding type. Abort Transconding. "
                     "Error was: {}",
                     *decoder_callbacks_, transcoding_type_status.status().message());
    return absl::InternalError(Errors::InternalError);
  }

  // Perform transcoding based on type
  auto transcoding_status = absl::UnknownError("uninitialized");
  switch (*transcoding_type_status) {
  case TranscodingType::HttpJson:
    transcoding_status = transcodeRequestToHttpJson(session, outgoing_buffer);
    break;

  case TranscodingType::HttpBody:
    transcoding_status = transcodeRequestToHttpBody(session, outgoing_buffer);
    break;
  }

  if (!transcoding_status.ok()) {
    ENVOY_STREAM_LOG(error, "Transcoding failed. Forward error.", *decoder_callbacks_);
    return transcoding_status;
  }

  // Rewrite headers common headers.
  session.decoder_headers->setContentLength(outgoing_buffer.length());
  return absl::OkStatus();
}

absl::Status Filter::transcodeRequestToHttpJson(Session& session,
                                                Buffer::Instance& outgoing_buffer) {
  auto json_status = transcoder_.grpcRequestToJson(session.decoder_data.toString());
  if (!json_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to transcode HTTP request from gRPC to JSON. "
                     "Error was: {}",
                     *decoder_callbacks_, json_status.status().message());
    return absl::InvalidArgumentError(Errors::GrpcToJsonFailed);
  }
  ENVOY_STREAM_LOG(debug, "Transcodeded HTTP request from gRPC to JSON", *decoder_callbacks_);

  // Replace buffer contents with transcoded JSON string
  clearBuffer(outgoing_buffer);
  outgoing_buffer.add(*json_status);
  return absl::OkStatus();
}

absl::Status Filter::transcodeRequestToHttpBody(Session&, Buffer::Instance&) {
  // TODO: Implement me
  return absl::UnimplementedError("transcodeRequestToHttpBody");
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

  // Prepare Transcoding.
  auto const transcoder_status = transcoder_.prepareTranscoding(session->method_and_path);
  if (!transcoder_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to prepare Transcoder from HTTP Method and Path. Abort Transcoding. "
                     "Error was: '{}'",
                     *encoder_callbacks_, transcoder_status.message());
    respondWithGrpcError(*encoder_callbacks_, Errors::InternalError);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Verify Headers depending on the methods transcoding type
  auto const transcoding_type_status = transcoder_.mapRequestTo();
  if (!transcoding_type_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to lookup transcoding type. Abort Transconding. "
                     "Error was: {}",
                     *encoder_callbacks_, transcoding_type_status.status().message());
    respondWithGrpcError(*encoder_callbacks_, Errors::InternalError);
    return Http::FilterHeadersStatus::StopIteration;
  }

  switch (*transcoding_type_status) {
  case TranscodingType::HttpJson: {
    if (headers.getContentTypeValue() != ContentTypes::Json) {
      ENVOY_STREAM_LOG(error,
                       "Received HTTP response does not containing JSON payload. Content type is "
                       "unsupported. Destroy session and send gRPC error downstream.",
                       *encoder_callbacks_);

      respondWithGrpcError(*encoder_callbacks_, Errors::UnexpectedContentType);
      return Http::FilterHeadersStatus::StopIteration;
    }
  } break;

  case TranscodingType::HttpBody:
    // Do nothing here. From gRPC definition, we don't know what the
    // content type should be. We just accept what we get.
    break;
  }

  // Modify common headers and proceed.
  headers.setContentType(ContentTypes::Grpc);

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

  // From here on the entire data stream is collected and ready for transcoding.
  // In case transcoding fails for any reason: Send gRPC error message downstream and
  // cleanup session.
  auto const status = transcodeResponse(*session, buffer);
  if (!status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Reponse transcoding failed. Destroy "
                     "Session and send gRPC error message downstream. Error was: {}",
                     *encoder_callbacks_);
    respondWithGrpcError(*encoder_callbacks_, status.message());
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  ENVOY_STREAM_LOG(debug, "Processed Session successfully. Destroy session.", *encoder_callbacks_);
  return Http::FilterDataStatus::Continue;
}

absl::Status Filter::transcodeResponse(Session& session, Buffer::Instance& outgoing_buffer) {
  auto const transcoder_status = transcoder_.prepareTranscoding(session.method_and_path);
  if (!transcoder_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to prepare Transcoder from HTTP Method and Path. Abort Transcoding. "
                     "Error was: {}",
                     *encoder_callbacks_, transcoder_status.message());
    return absl::InternalError(Errors::InternalError);
  }

  // Transcode buffer depending on its type
  auto const transcoding_type_status = transcoder_.mapRequestTo();
  if (!transcoding_type_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to lookup transcoding type. Abort Transconding. "
                     "Error was: {}",
                     *encoder_callbacks_, transcoding_type_status.status().message());
    return absl::InternalError(Errors::InternalError);
  }

  // Perform transcoding based on assumend type
  auto transcoding_status = absl::UnknownError("uninitialized");
  switch (*transcoding_type_status) {
  case TranscodingType::HttpJson:
    transcoding_status = transcodeResponseFromHttpJson(session, outgoing_buffer);
    break;

  case TranscodingType::HttpBody:
    transcoding_status = transcodeResponseFromHttpBody(session, outgoing_buffer);
    break;
  }

  if (!transcoding_status.ok()) {
    ENVOY_STREAM_LOG(error, "Transcoding failed. Forward error.", *encoder_callbacks_);
    return transcoding_status;
  }

  // Rewrite transcoding type independent headers
  session.encoder_headers->setContentLength(outgoing_buffer.length());

  // Elide gRPC response status code from HTTP status code
  auto& trailers = encoder_callbacks_->addEncodedTrailers();
  auto const http_status = Http::Utility::getResponseStatus(*(session.encoder_headers));
  trailers.setGrpcStatus(grpcStatusFromHttpStatus(http_status));
  return absl::OkStatus();
}

absl::Status Filter::transcodeResponseFromHttpJson(Session& session,
                                                   Buffer::Instance& outgoing_buffer) {
  auto grpc_status = transcoder_.jsonResponseToGrpc(session.encoder_data.toString());
  if (!grpc_status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to transcode http response from JSON to gRPC. "
                     "Error was: {}",
                     *encoder_callbacks_, grpc_status.status().message());
    return absl::InternalError(Errors::JsonToGrpcFailed);
  }

  ENVOY_STREAM_LOG(debug, "Transcodeded http response from JSON to gRPC", *encoder_callbacks_);

  // Replace buffer contents with transcoded gRPC message
  replaceBufferWithGrpcMessage(outgoing_buffer, *grpc_status);
  return absl::OkStatus();
}
absl::Status Filter::transcodeResponseFromHttpBody(Session&, Buffer::Instance&) {
  // TODO: Implement me
  return absl::UnimplementedError("Implement me");
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
