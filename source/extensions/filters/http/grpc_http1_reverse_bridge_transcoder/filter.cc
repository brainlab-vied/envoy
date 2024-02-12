#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/filter.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "source/common/common/enum_to_int.h"
#include "source/common/grpc/codec.h"
#include "source/common/grpc/common.h"
#include "source/common/grpc/status.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

Http::RegisterCustomInlineHeader<Http::CustomInlineHeaderRegistry::Type::RequestHeaders>
    accept_handle(Http::CustomHeaders::get().Accept);

struct RcDetailsValues {
  // The gRPC HTTP/1 reverse bridge failed because the body payload was too
  // small to be a gRPC frame.
  const std::string GrpcBridgeFailedTooSmall = "grpc_bridge_data_too_small";
  // The gRPC HTTP/1 bridge encountered an unsupported content type.
  const std::string GrpcBridgeFailedContentType = "grpc_bridge_content_type_wrong";
  // The gRPC HTTP/1 bridge expected the upstream to set a header indicating
  // the content length, but it did not.
  const std::string GrpcBridgeFailedMissingContentLength = "grpc_bridge_content_length_missing";
  // The gRPC HTTP/1 bridge expected the upstream to set a header indicating
  // the content length, but it sent a value different than the actual response
  // payload size.
  const std::string GrpcBridgeFailedWrongContentLength = "grpc_bridge_content_length_wrong";
};
using RcDetails = ConstSingleton<RcDetailsValues>;

namespace {
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

std::string badContentTypeMessage(const Http::ResponseHeaderMap& headers) {
  if (headers.ContentType() != nullptr) {
    return fmt::format(
        "envoy reverse bridge: upstream responded with unsupported content-type {}, status code {}",
        headers.getContentTypeValue(), headers.getStatusValue());
  } else {
    return fmt::format(
        "envoy reverse bridge: upstream responded with no content-type header, status code {}",
        headers.getStatusValue());
  }
}

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
} // namespace

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

    adjustContentLength(headers, [](auto size) { return size - Grpc::GRPC_FRAME_HEADER_SIZE; });

    decoder_callbacks_->downstreamCallbacks()->clearRouteCache();
  } else {
    ENVOY_STREAM_LOG(debug,
                     "Content-type is not application/grpc. Request is passed through "
                     "without transcoding.",
                     *decoder_callbacks_);
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus Filter::decodeData(Buffer::Instance& buffer, bool) {
    if (enabled_) {
    if (!prefix_stripped_) {
      if (buffer.length() < Grpc::GRPC_FRAME_HEADER_SIZE) {
        decoder_callbacks_->sendLocalReply(Http::Code::OK, "invalid request body", nullptr,
                                           Grpc::Status::WellKnownGrpcStatus::Unknown,
                                           RcDetails::get().GrpcBridgeFailedTooSmall);

        return Http::FilterDataStatus::StopIterationNoBuffer;
      }

      buffer.drain(Grpc::GRPC_FRAME_HEADER_SIZE);
      prefix_stripped_ = true;

      Buffer::OwnedImpl workBuffer;
      workBuffer.move(buffer);

      auto [status, outputData] = transcoder_.fromGrpcBufferToJson(workBuffer);

      if (!status.ok()) {
        ENVOY_STREAM_LOG(error, "Failed to transcode gRPC request to JSON: {}", *decoder_callbacks_,
                         status.message());

        decoder_callbacks_->sendLocalReply(
            Http::Code::OK, "Failed to transcode gRPC request to JSON", nullptr,
            Grpc::Status::WellKnownGrpcStatus::Unknown, RcDetails::get().GrpcBridgeFailedTooSmall);
      } else {
        ENVOY_STREAM_LOG(debug, "Succesfully transcoded gRPC request to JSON", *decoder_callbacks_);
      }

      buffer.add(outputData);
    }
  }

  return Http::FilterDataStatus::Continue;
}

Http::FilterHeadersStatus Filter::encodeHeaders(Http::ResponseHeaderMap& headers, bool) {
  if (enabled_) {
    absl::string_view content_type = headers.getContentTypeValue();

    // FIXME: When we add transcoding, resonses should be application/json or HttpBody
    if (content_type != Http::Headers::get().ContentTypeValues.Json) {

      decoder_callbacks_->sendLocalReply(Http::Code::OK, badContentTypeMessage(headers), nullptr,
                                         Grpc::Status::WellKnownGrpcStatus::Unknown,
                                         RcDetails::get().GrpcBridgeFailedContentType);

      return Http::FilterHeadersStatus::StopIteration;
    }

    // FIXME: When we add transcoding, resonses should be application/grpc or HttpBody
    headers.setContentType(Http::Headers::get().ContentTypeValues.Grpc);
    headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Grpc);

    //adjustContentLength(headers, [](auto length) { return length + Grpc::GRPC_FRAME_HEADER_SIZE; });
    headers.setContentLength(16);
    grpc_status_ = grpcStatusFromHeaders(headers);
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus Filter::encodeData(Buffer::Instance& buffer, bool end_stream) {
  if (!enabled_) {
    return Http::FilterDataStatus::Continue;
  }

  if (end_stream) {

    auto [status, outputData] = transcoder_.fromJsonBufferToGrpc(buffer_);

    if (!status.ok()) {
      ENVOY_STREAM_LOG(error, "Failed to transcode gRPC request to JSON: {}", *encoder_callbacks_,
                       status.message());

      encoder_callbacks_->sendLocalReply(Http::Code::OK, "Failed to transcode gRPC request to JSON",
                                         nullptr, Grpc::Status::WellKnownGrpcStatus::Unknown,
                                         RcDetails::get().GrpcBridgeFailedTooSmall);
    } else {
      ENVOY_STREAM_LOG(debug, "Succesfully transcoded JSON response to gRPC", *encoder_callbacks_);
    }

    // Insert grpc-status trailers to communicate the error code.
    auto& trailers = encoder_callbacks_->addEncodedTrailers();
    trailers.setGrpcStatus(grpc_status_);

    buffer.prepend(outputData);
    buildGrpcFrameHeader(buffer, buffer.length());

    return Http::FilterDataStatus::Continue;
  }

  // Buffer the response in a mutable buffer: we need to determine the size of the response
  // and modify it later on.

  buffer_.move(buffer);
  return Http::FilterDataStatus::StopIterationAndBuffer;
}

Http::FilterTrailersStatus Filter::encodeTrailers(Http::ResponseTrailerMap& trailers) {
  if (!enabled_) {
    return Http::FilterTrailersStatus::Continue;
  }

  trailers.setGrpcStatus(grpc_status_);

  return Http::FilterTrailersStatus::Continue;
}

void Filter::buildGrpcFrameHeader(Buffer::Instance& buffer, uint32_t message_length) {
  // We do this even if the upstream failed: If the response returned non-200,
  // we'll respond with a grpc-status with an error, so clients will know that the request
  // was unsuccessful. Since we're guaranteed at this point to have a valid response
  // (unless upstream lied in content-type) we attempt to return a well-formed gRPC
  // response body.
  Grpc::Encoder().prependFrameHeader(Grpc::GRPC_FH_DEFAULT, buffer, message_length);
}

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy