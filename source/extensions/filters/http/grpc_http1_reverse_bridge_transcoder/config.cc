// TODO: cleanup

#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/config.h"

#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.h"
#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.validate.h"
#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/filter.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

Http::FilterFactoryCb Config::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::FilterConfig&
        config,
    const std::string&, Server::Configuration::FactoryContext& context) {

  auto filter_config = std::make_shared<Filter>(context.getServerFactoryContext().api(),
                                                config.proto_descriptor(), config.service());

  return [filter_config](Envoy::Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(filter_config);
  };
}

Router::RouteSpecificFilterConfigConstSharedPtr Config::createRouteSpecificFilterConfigTyped(
    const envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::
        FilterConfigPerRoute& proto_config,
    Server::Configuration::ServerFactoryContext&, ProtobufMessage::ValidationVisitor&) {
  return std::make_shared<FilterConfigPerRoute>(proto_config);
}

/// Static registration for the grpc http1 reverse bridge filter. @see RegisterFactory.
REGISTER_FACTORY(Config, Server::Configuration::NamedHttpFilterConfigFactory);
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
