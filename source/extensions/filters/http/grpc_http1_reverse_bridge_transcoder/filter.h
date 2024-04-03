// TODO: cleanup

#pragma once

#include <string>
#include <string_view>

#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.h"
#include "envoy/http/filter.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/extensions/filters/http/common/pass_through_filter.h"
#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/transcoder.h"
#include "source/common/common/logger.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

class Filter : public Envoy::Http::PassThroughFilter, public Logger::Loggable<Logger::Id::filter> {
public:
  // ctor
  Filter(Api::Api& api, std::string proto_descriptor_path, std::string service_name);

  // Implementation Http::StreamDecoderFilter: gRPC -> http/Rest
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& buffer, bool end_stream) override;

  // Implementation Http::StreamEncoderFilter: http/Rest -> gRPC
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& buffer, bool end_stream) override;

private:
  template <class CallbackType>
  void respondWithGrpcError(CallbackType& callback_type, const std::string_view description);

  template <class CallbackType>
  void respondWithGrpcError(CallbackType& callback_type, const std::string_view description,
                            Grpc::Status::GrpcStatus grpc_status);

private:
  Transcoder transcoder_;
  bool enabled_;
  Http::RequestHeaderMap* decoder_headers_;
  Buffer::OwnedImpl decoder_body_;
  Http::ResponseHeaderMap* encoder_headers_;
  Buffer::OwnedImpl encoder_body_;
};

class FilterConfigPerRoute : public Router::RouteSpecificFilterConfig {
public:
  FilterConfigPerRoute(const envoy::extensions::filters::http::
                           grpc_http1_reverse_bridge_transcoder::v3::FilterConfigPerRoute&) {}

  bool disabled() const { return disabled_; }

private:
  bool disabled_ = false;
};
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
