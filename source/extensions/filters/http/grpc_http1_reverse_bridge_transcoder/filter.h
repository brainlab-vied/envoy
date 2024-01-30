#pragma once

#include <memory>
#include <string>

#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.h"
#include "envoy/http/filter.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/grpc/status.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"

#include "source/common/common/logger.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

// When enabled, will downgrade an incoming gRPC http request into a h/1.1 request.
class Filter : public Envoy::Http::PassThroughFilter, public Logger::Loggable<Logger::Id::filter> {
public:
  Filter( Api::Api& api, std::string temp_param)
      : api_{api}, _temp_param(std::move(temp_param)) {}
  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& buffer, bool end_stream) override;

  // Http::StreamEncoderFilter
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& buffer, bool end_stream) override;
  Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap& trailers) override;

private:
  // Prepend the grpc frame into the buffer
  void buildGrpcFrameHeader(Buffer::Instance& buffer, uint32_t message_length);

  Api::Api& api_;
    // FIXME: Add real paramters
  const std::string _temp_param;

  bool enabled_{};
  bool prefix_stripped_{};

  // The actual size of the response returned by the upstream so far.
  uint32_t upstream_response_bytes_{};

  std::string content_type_{};
  Grpc::Status::GrpcStatus grpc_status_{};
  // Normally we'd use the encoding buffer, but since we need to mutate the
  // buffer we instead maintain our own.
  Buffer::OwnedImpl buffer_{};
};

using FilterPtr = std::unique_ptr<Filter>;

class FilterConfigPerRoute : public Router::RouteSpecificFilterConfig {
public:
  FilterConfigPerRoute(
      const envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::FilterConfigPerRoute&
          config)
      : disabled_(false) { (void) config; }
  bool disabled() const { return disabled_; }

private:
  bool disabled_;
};

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
