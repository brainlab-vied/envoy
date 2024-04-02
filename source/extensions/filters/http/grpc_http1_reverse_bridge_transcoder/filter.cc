// TODO: cleanup
// TODO: Respect configured buffer limits: See grpc_json_transcoder/filter.cc line 438
// TODO: Take envoy route configuration into account

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
const auto HeaderOnly = "HTTP message is header only.";
const auto UnexpectedMethodType = "HTTP method type unexpected method type.";
const auto UnexpectedRequestPath = "HTTP request path is unexpected.";
const auto UnexpectedContentType = "HTTP header contains unexpected content type.";
const auto GrpcUnexpectedRequestPath = "gRPC request path is unexpected.";
const auto GrpcFrameTooSmall = "Received gRPC Frame content if too small.";
const auto GrpcToJsonFailed = "Failed to transcode gRPC to JSON.";
const auto JsonToGrpcFailed = "Failed to transcode JSON to gRPC.";
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
    : transcoder_{}, enabled_{false}, decoder_headers_{nullptr}, decoder_body_{},
      encoder_headers_{nullptr}, encoder_body_{} {
  auto const status = transcoder_.init(api, proto_descriptor_path, service_name);
  assert(status.ok());
}

/////////////////////////////////////////////////////////////////
// Implementation Http::StreamDecoderFilter: gRPC -> http/JSON //
/////////////////////////////////////////////////////////////////
Http::FilterHeadersStatus Filter::decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) {
  enabled_ = false;
  decoder_headers_ = nullptr;

  // Short circuit if header only.
  if (end_stream) {
    ENVOY_STREAM_LOG(debug, "Request is Header only. Request is forwarded.", *decoder_callbacks_);
    return Http::FilterHeadersStatus::Continue;
  }

  // Disable filter per route config if applies
  if (decoder_callbacks_->route() != nullptr) {
    const auto* per_route_config =
        Http::Utility::resolveMostSpecificPerFilterConfig<FilterConfigPerRoute>(decoder_callbacks_);

    if (per_route_config != nullptr && per_route_config->disabled()) {
      ENVOY_STREAM_LOG(debug, "Transcoding is disabled for this route. Request is forwarded.",
                       *decoder_callbacks_);
      return Http::FilterHeadersStatus::Continue;
    }
  }

  // If this isn't a gRPC request: Pass through
  if (!Envoy::Grpc::Common::isGrpcRequestHeaders(headers)) {
    ENVOY_STREAM_LOG(debug, "Content-type is not application/grpc. Request is forwarded.",
                     *decoder_callbacks_);
    return Http::FilterHeadersStatus::Continue;
  }

  // Configure transcoder to process this request.
  auto const status_method = httpMethodFrom(headers.getMethodValue());
  if (!status_method.ok()) {
    ENVOY_STREAM_LOG(error, "Failed to parse HTTP Method. Abort Processing. Error was: {}",
                     *decoder_callbacks_, status_method.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::UnexpectedMethodType);
    return Http::FilterHeadersStatus::StopIteration;
  }

  auto path = static_cast<std::string>(headers.getPathValue());
  auto method_and_path = HttpMethodAndPath{*status_method, std::move(path)};
  auto const status_transcoder = transcoder_.prepareTranscoding(method_and_path);
  if (!status_transcoder.ok()) {
    ENVOY_STREAM_LOG(error, "Failed to prepare transcoding. Abort Processing. Error was: {}",
                     *decoder_callbacks_, status_transcoder.message());

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcUnexpectedRequestPath);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // Rewrite HTTP Headers
  auto status_path = transcoder_.getHttpRequestPath();
  if (!status_path.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to determine new HTTP Request path. Abort Processing. Error was: {}",
                     *decoder_callbacks_, status_path.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::UnexpectedRequestPath);
    return Http::FilterHeadersStatus::StopIteration;
  }

  headers.setPath(std::move(*status_path));
  headers.setEnvoyOriginalPath(headers.getPathValue());
  headers.setContentType(Http::Headers::get().ContentTypeValues.Json);
  headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Json);

  // NOTE: Content length handling. Since we don't know the content length
  // before body transcoding. Memorize a pointer to the header map and use it
  // in decodeData().
  decoder_headers_ = &headers;

  // TODO: Figure out what this does???
  decoder_callbacks_->downstreamCallbacks()->clearRouteCache();
  enabled_ = true;

  // TODO: Handle HttpBody
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus Filter::decodeData(Buffer::Instance& buffer, bool end_stream) {
  // NOTE: Buffering behavior. Envoy usually passes buffers chunk wise to the filter chains and
  // assembles the contents in its own internal buffer. These Fragments are passed down the filter
  // chain. In our use case, we don't want this behavior. Instead we copy the streamed data chunks
  // in our own internal buffer and convert the entire buffer at the end of the stream to pass the
  // result further. To achieve this the return code "StopIterationNoBuffer" disables the internal
  // buffering and "Continue" is used to pass on the contents "buffer".
  if (!enabled_) {
    return Http::FilterDataStatus::Continue;
  }

  if (buffer.length()) {
    decoder_body_.add(buffer);
  }

  if (!end_stream) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // From here on the entire data stream is collected and ready for transcoding.
  // Strip gRPC Header from buffer and try to transcode buffered data.
  if (decoder_body_.length() < Grpc::GRPC_FRAME_HEADER_SIZE) {
    clearBuffer(decoder_body_);

    ENVOY_STREAM_LOG(error,
              "gRPC request data frame too few bytes to be a gRPC request. Respond with error "
              "and drop buffer.", *decoder_callbacks_);

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcFrameTooSmall);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  decoder_body_.drain(Grpc::GRPC_FRAME_HEADER_SIZE);

  auto json_status = transcoder_.grpcRequestToJson(decoder_body_.toString());
  clearBuffer(decoder_body_);

  // If transcoding fails: Respond to initial sender with a http message containing an error.
  if (!json_status.ok()) {
    ENVOY_STREAM_LOG(error,
              "Failed to transcode http request from gRPC to JSON. Respond with Error and "
              "drop buffer. Error was: '{}'", *decoder_callbacks_,
              json_status.status().message());

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcToJsonFailed);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  ENVOY_STREAM_LOG(debug, "Transcodeded http request from gRPC to JSON", *decoder_callbacks_);

  // Replace buffer contents with transcoded JSON string
  clearBuffer(buffer);
  buffer.add(*json_status);

  // Rewrite headers content length with the size of the buffers contents.
  decoder_headers_->setContentLength(buffer.length());
  decoder_headers_ = nullptr;

  return Http::FilterDataStatus::Continue;
}

////////////////////////////////////////////////////////////////
// Implementation Http::StreamEncoderFilter: http/JSON-> gRPC //
////////////////////////////////////////////////////////////////
Http::FilterHeadersStatus Filter::encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) {
  encoder_headers_ = nullptr;

  if (!enabled_) {
    return Http::FilterHeadersStatus::Continue;
  }

  // NOTE: Note: Analyze status code. Currently, it is ignored. Send error downstream.
  // NOTE: This may very well happen in case of errors
  if (end_stream) {
    ENVOY_STREAM_LOG(error, "Received HTTP header only response. This must not happen in our use "
                     "case. Respond with Error.", *encoder_callbacks_);

    respondWithGrpcError(*encoder_callbacks_, Errors::HeaderOnly);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // FIXME: When we add transcoding, resonses should be application/json or HttpBody

  auto content_type = headers.getContentTypeValue();
  if (content_type != Http::Headers::get().ContentTypeValues.Json) {
    ENVOY_STREAM_LOG(error, "Received HTTP response not containing JSON payload. Unable to "
                     "transcode. Respond with Error.", *encoder_callbacks_);

    respondWithGrpcError(*encoder_callbacks_, Errors::UnexpectedContentType);
  }

  // FIXME: When we add transcoding, resonses should be application/grpc or HttpBody
  headers.setContentType(Http::Headers::get().ContentTypeValues.Grpc);
  headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Grpc);

  // NOTE: Content length handling. Since we don't know the content length
  // before body transcoding. Memorize a pointer to the header map and use it
  // in decodeData().
  encoder_headers_ = &headers;
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus Filter::encodeData(Buffer::Instance& buffer, bool end_stream) {
  // NOTE: Buffering behavior. Envoy usually passes buffers chunk wise to the filter chains and
  // assembles the contents in its own internal buffer. These Fragments are passed down the filter
  // chain. In our use case, we don't want this behavior. Instead we copy the streamed data chunks
  // in our own internal buffer and convert the entire buffer at the end of the stream to pass the
  // result further. To achieve this the return code "StopIterationNoBuffer" disables the internal
  // buffering and "Continue" is used to pass on the contents "buffer".
  if (!enabled_) {
    return Http::FilterDataStatus::Continue;
  }

  if (buffer.length()) {
    encoder_body_.add(buffer);
  }

  if (!end_stream) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // From here on the entire data stream is collected and ready for transcoding.
  auto grpc_status = transcoder_.jsonResponseToGrpc(encoder_body_.toString());
  clearBuffer(encoder_body_);

  // If transcoding fails: Respond to initial sender with a http message containing an error.
  if (!grpc_status.ok()) {
    ENVOY_STREAM_LOG(error,
              "Failed to transcode http response from JSON to gRPC. Respond with Error and "
              "drop buffer. Error was: '{}'", *encoder_callbacks_,
              grpc_status.status().message());

    respondWithGrpcError(*encoder_callbacks_, Errors::JsonToGrpcFailed);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  ENVOY_STREAM_LOG(debug, "Transcodeded http response from JSON to gRPC", *encoder_callbacks_);

  // Replace buffer contents with transcoded gRPC message then update header and trailers
  replaceBufferWithGrpcMessage(buffer, *grpc_status);

  // Elide gRPC response status code from HTTP status code
  auto& trailers = encoder_callbacks_->addEncodedTrailers();
  auto const http_status = Http::Utility::getResponseStatus(*encoder_headers_);
  trailers.setGrpcStatus(grpcStatusFromHttpStatus(http_status));

  encoder_headers_->setContentLength(buffer.length());
  encoder_headers_ = nullptr;

  return Http::FilterDataStatus::Continue;
}

template <class CallbackType>
void Filter::respondWithGrpcError(CallbackType& callback_type, const std::string_view description) {
  // Send a gRPC response indicating an error. Despite propagating an error the
  // underlying HTTP Response is still well formed.
  // Since we are transcoding here, the only gRPC status code that somehow fits is "Unknown".
  callback_type.sendLocalReply(Http::Code::OK,
                               "envoy reverse bridge: gRPC <-> http/JSON transcoding failed.",
                               nullptr, Grpc::Status::WellKnownGrpcStatus::Unknown, description);
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
