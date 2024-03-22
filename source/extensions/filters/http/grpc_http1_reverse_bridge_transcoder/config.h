// TODO: cleanup

#pragma once

#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.h"
#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.validate.h"
#include "envoy/server/filter_config.h"

#include "source/extensions/filters/http/common/factory_base.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

class Config
    : public Common::FactoryBase<
          envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::FilterConfig,
          envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::
              FilterConfigPerRoute> {
public:
  Config() : FactoryBase("envoy.filters.http.grpc_http1_reverse_bridge_transcoder") {}

  Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::
          FilterConfig& config,
      const std::string& stat_prefix,
      Envoy::Server::Configuration::FactoryContext& context) override;

private:
  Router::RouteSpecificFilterConfigConstSharedPtr createRouteSpecificFilterConfigTyped(
      const envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::
          FilterConfigPerRoute& proto_config,
      Server::Configuration::ServerFactoryContext& context,
      ProtobufMessage::ValidationVisitor& validator) override;
};
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
