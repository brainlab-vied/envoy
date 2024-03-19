// TODO: Add documentation
// TODO: cleanup

#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/filter.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "source/common/grpc/codec.h"
#include "source/common/grpc/common.h"
#include "source/common/grpc/status.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {
namespace {

// Internal constants
namespace Errors {
const std::string HeaderOnly = "HTTP message is header only.";
const std::string UnexpectedContentType = "HTTP header contains unexpected content type.";
const std::string GrpcFrameTooSmall = "Received gRPC Frame content if too small.";
const std::string GrpcToJsonFailed = "Failed to transcode gRPC to JSON.";
const std::string JsonToGrpcFailed = "Failed to transcode JSON to gRPC.";
} // namespace Errors

Http::RegisterCustomInlineHeader<Http::CustomInlineHeaderRegistry::Type::RequestHeaders>
    accept_handle(Http::CustomHeaders::get().Accept);

// Refactor me?
Grpc::Status::GrpcStatus grpcStatusFromHeaders(Http::ResponseHeaderMap& headers) {
  const auto http_response_status = Http::Utility::getResponseStatus(headers);

  // Notably, we treat an upstream 200 as a successful response. This differs
  // from the standard but is key in being able to transform a successful
  // upstream HTTP response into a gRPC response.
  if (http_response_status == 200) {
    return Grpc::Status::WellKnownGrpcStatus::Ok;
  } else {
    return Grpc::Utility::httpToGrpcStatus(http_response_status);
  }
}

/*
// TODO: Reenable me after adding contentLength header support
void adjustContentLength(Http::RequestOrResponseHeaderMap& headers,
                         const std::function<uint64_t(uint64_t value)>& adjustment) {
  auto length_header = headers.getContentLengthValue();
  if (!length_header.empty()) {
    uint64_t length;
    if (absl::SimpleAtoi(length_header, &length)) {
      if (length != 0) {
        headers.setContentLength(adjustment(length));
      }
    }
  }
}
*/

void clearBuffer(Buffer::Instance& buffer) { buffer.drain(buffer.length()); }

void replaceBufferWithGrpcMessage(Buffer::Instance& buffer, std::string& payload) {
  using GrpcFrameHeader = std::array<uint8_t, Grpc::GRPC_FRAME_HEADER_SIZE>;

  GrpcFrameHeader header;
  Grpc::Encoder().newFrame(Grpc::GRPC_FH_DEFAULT, payload.size(), header);

  clearBuffer(buffer);
  buffer.add(header.data(), header.size());
  buffer.add(payload);
}
} // namespace

Filter::Filter(Api::Api& api, std::string proto_descriptor, std::string service_name)
    : transcoder_{api, std::move(proto_descriptor), std::move(service_name)}, enabled_{false},
      grpc_status_{}, decoder_buffer_{}, encoder_buffer_{} {}

/////////////////////////////////////////////////////////////////
// Implementation Http::StreamDecoderFilter: gRPC -> http/JSON //
/////////////////////////////////////////////////////////////////
Http::FilterHeadersStatus Filter::decodeHeaders(Http::RequestHeaderMap& headers, bool end_stream) {
  // Short circuit if header only.
  if (end_stream) {
    ENVOY_STREAM_LOG(debug, "Header only request", *decoder_callbacks_);
    return Http::FilterHeadersStatus::Continue;
  }

  // Disable filter per route config if applies
  if (decoder_callbacks_->route() != nullptr) {

    const auto* per_route_config =
        Http::Utility::resolveMostSpecificPerFilterConfig<FilterConfigPerRoute>(decoder_callbacks_);

    if (per_route_config != nullptr && per_route_config->disabled()) {
      enabled_ = false;
      ENVOY_STREAM_LOG(debug,
                       "Transcoding is disabled for the route. Request headers is passed through.",
                       *decoder_callbacks_);
      return Http::FilterHeadersStatus::Continue;
    }
  }

  if (Envoy::Grpc::Common::isGrpcRequestHeaders(headers)) {
    enabled_ = true;
    transcoder_.setCurrentMethod(headers.getPathValue());

    // FIXME: Handle HttpBody
    headers.setContentType(Http::Headers::get().ContentTypeValues.Json);
    headers.setInline(accept_handle.handle(), Http::Headers::get().ContentTypeValues.Json);

    // TODO: As of yet the content length is okay. It needs to be recalculated after json
    // transcoding. adjustContentLength(headers, [](auto size) { return size -
    // Grpc::GRPC_FRAME_HEADER_SIZE; });
    headers.removeContentLength();

    decoder_callbacks_->downstreamCallbacks()->clearRouteCache();
  } else {
    ENVOY_STREAM_LOG(debug,
                     "Content-type is not application/grpc. Request is passed through "
                     "without transcoding.",
                     *decoder_callbacks_);
  }

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
    decoder_buffer_.add(buffer);
  }

  if (!end_stream) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // From here on the entire data stream is collected and ready for transcoding.
  // Strip gRPC Header from buffer and try to transcode buffered data.
  if (decoder_buffer_.length() < Grpc::GRPC_FRAME_HEADER_SIZE) {
    clearBuffer(decoder_buffer_);

    ENVOY_STREAM_LOG(
        error,
        "gRPC request data frame to0 few bytes to be a gRPC request. Respond with error "
        "and drop buffer.",
        *decoder_callbacks_);

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcFrameTooSmall);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  decoder_buffer_.drain(Grpc::GRPC_FRAME_HEADER_SIZE);

  auto [status, json_payload] = transcoder_.fromGrpcBufferToJson(decoder_buffer_);
  clearBuffer(decoder_buffer_);

  // If transcoding fails: Respond to initial sender with a http message containing an error.
  if (!status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to transcode http request from gRPC to JSON. Respond with Error and "
                     "drop buffer. Error was: '{}'",
                     *decoder_callbacks_, status.message());

    respondWithGrpcError(*decoder_callbacks_, Errors::GrpcToJsonFailed);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  ENVOY_STREAM_LOG(debug, "Transcodeded http request from gRPC to JSON", *decoder_callbacks_);

  // Replace buffer contents with transcoded JSON string
  clearBuffer(buffer);
  buffer.add(json_payload);
  return Http::FilterDataStatus::Continue;
}

////////////////////////////////////////////////////////////////
// Implementation Http::StreamEncoderFilter: http/JSON-> gRPC //
////////////////////////////////////////////////////////////////
Http::FilterHeadersStatus Filter::encodeHeaders(Http::ResponseHeaderMap& headers, bool end_stream) {
  if (!enabled_) {
    return Http::FilterHeadersStatus::Continue;
  }

  if (end_stream) {
    ENVOY_STREAM_LOG(error,
                     "Received HTTP header only response. This must not happen in our use "
                     "case. Respond with Error.",
                     *encoder_callbacks_);

    respondWithGrpcError(*encoder_callbacks_, Errors::HeaderOnly);
    return Http::FilterHeadersStatus::StopIteration;
  }

  // FIXME: When we add transcoding, resonses should be application/json or HttpBody
  auto content_type = headers.getContentTypeValue();
  if (content_type != Http::Headers::get().ContentTypeValues.Json) {
    ENVOY_STREAM_LOG(error,
                     "Received HTTP response not containing JSON payload. Unable to "
                     "transcode. Respond with Error.",
                     *encoder_callbacks_);

    respondWithGrpcError(*encoder_callbacks_, Errors::UnexpectedContentType);
  }

  // FIXME: When we add transcoding, resonses should be application/grpc or HttpBody
  headers.setContentType(Http::Headers::get().ContentTypeValues.Grpc);
  headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Grpc);

  // TODO: Adjust Content Length at a later point (maybe use Trailers for this?)
  // adjustContentLength(headers, [](auto length) { return length + Grpc::GRPC_FRAME_HEADER_SIZE;
  // }); headers.setContentLength(16);
  headers.removeContentLength();
  grpc_status_ = grpcStatusFromHeaders(headers);
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
    encoder_buffer_.add(buffer);
  }

  if (!end_stream) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  // From here on the entire data stream is collected and ready for transcoding.
  auto [status, grpc_payload] = transcoder_.fromJsonBufferToGrpc(encoder_buffer_);
  clearBuffer(encoder_buffer_);

  // If transcoding fails: Respond to initial sender with a http message containing an error.
  if (!status.ok()) {
    ENVOY_STREAM_LOG(error,
                     "Failed to transcode http response from JSON to gRPC. Respond with Error and "
                     "drop buffer. Error was: '{}'",
                     *encoder_callbacks_, status.message());

    respondWithGrpcError(*encoder_callbacks_, Errors::JsonToGrpcFailed);
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  ENVOY_STREAM_LOG(debug, "Transcodeded http response from JSON to gRPC", *encoder_callbacks_);

  // Replace buffer contents with transcoded gRPC message and attach http
  // trailer with memorized status code.
  replaceBufferWithGrpcMessage(buffer, grpc_payload);
  auto& trailers = encoder_callbacks_->addEncodedTrailers();
  trailers.setGrpcStatus(grpc_status_);

  return Http::FilterDataStatus::Continue;
}

template <class CallbackType>
void Filter::respondWithGrpcError(CallbackType& callback_type, const std::string& description) {
  // Send a gRPC response indicating an error. Despite propagating an error the
  // underlying HTTP Response is still well formed.
  // Since we are transcoding here, the only gRPC status code that somehow fits is "Unknown".
  callback_type.sendLocalReply(Http::Code::OK,
                               "envoy reverse bridge: gRPC <-> http/JSON transcoding failed.",
                               nullptr, Grpc::Status::WellKnownGrpcStatus::Unknown, description);
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
