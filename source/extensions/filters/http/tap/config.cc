#include "source/extensions/filters/http/tap/config.h"

#include "envoy/config/tap/v3/common.pb.h"
#include "envoy/extensions/filters/http/tap/v3/tap.pb.h"
#include "envoy/extensions/filters/http/tap/v3/tap.pb.validate.h"
#include "envoy/registry/registry.h"

#include "source/extensions/filters/http/tap/tap_config_impl.h"
#include "source/extensions/filters/http/tap/tap_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace TapFilter {

class HttpTapConfigFactoryImpl : public Extensions::Common::Tap::TapConfigFactory {
public:
  HttpTapConfigFactoryImpl(Server::Configuration::FactoryContext& context)
      : factory_context_(context) {}
  // TapConfigFactory
  Extensions::Common::Tap::TapConfigSharedPtr
  createConfigFromProto(const envoy::config::tap::v3::TapConfig& proto_config,
                        Extensions::Common::Tap::Sink* admin_streamer) override {
    return std::make_shared<HttpTapConfigImpl>(std::move(proto_config), admin_streamer,
                                               factory_context_);
  }

private:
  Server::Configuration::FactoryContext& factory_context_;
};

Http::FilterFactoryCb TapFilterFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::tap::v3::Tap& proto_config,
    const std::string& stats_prefix, Server::Configuration::FactoryContext& context) {
  auto& server_context = context.getServerFactoryContext();

  FilterConfigSharedPtr filter_config(new FilterConfigImpl(
      proto_config, stats_prefix, std::make_unique<HttpTapConfigFactoryImpl>(context),
      context.scope(), server_context.admin(), server_context.singletonManager(),
      server_context.threadLocal(), server_context.mainThreadDispatcher()));
  return [filter_config](Http::FilterChainFactoryCallbacks& callbacks) -> void {
    auto filter = std::make_shared<Filter>(filter_config);
    callbacks.addStreamFilter(filter);
    callbacks.addAccessLogHandler(filter);
  };
}

/**
 * Static registration for the tap filter. @see RegisterFactory.
 */
REGISTER_FACTORY(TapFilterFactory, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace TapFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
