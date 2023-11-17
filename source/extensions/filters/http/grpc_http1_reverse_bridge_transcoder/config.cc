#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/config.h"

#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.h"
#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/v3/config.pb.validate.h"
#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

Http::FilterFactoryCb Config::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::FilterConfig& config,
    const std::string&, Server::Configuration::FactoryContext&) {
  return [config](Envoy::Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<Filter>(config.temp_param()));
  };
}

Router::RouteSpecificFilterConfigConstSharedPtr Config::createRouteSpecificFilterConfigTyped(
    const envoy::extensions::filters::http::grpc_http1_reverse_bridge_transcoder::v3::FilterConfigPerRoute&
        proto_config,
    Server::Configuration::ServerFactoryContext&, ProtobufMessage::ValidationVisitor&) {
  return std::make_shared<FilterConfigPerRoute>(proto_config);
}

/**
 * Static registration for the grpc http1 reverse bridge filter. @see RegisterFactory.
 */
REGISTER_FACTORY(Config, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace GrpcHttp1ReverseBridge
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
