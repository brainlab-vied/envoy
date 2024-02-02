#pragma once

#include "envoy/server/filter_config.h"
#include "source/common/buffer/buffer_impl.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"
#include "source/common/common/logger.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

class Transcoder : public Logger::Loggable<Logger::Id::filter> {
public:
  Transcoder(Api::Api& api, std::string proto_descriptor, std::string service_name);

  std::pair<absl::Status, std::string> fromGrpcBufferToJson(Buffer::OwnedImpl& buffer);
  std::pair<absl::Status, std::string> fromJsonBufferToGrpc(Buffer::OwnedImpl& buffer);

  void setCurrentMethod(std::string_view methodName) { methodName_ = methodName; }

private:
  Envoy::Protobuf::FileDescriptorSet descSet_;
  Protobuf::DescriptorPool descPool_;
  std::unique_ptr<Protobuf::util::TypeResolver> resolver_;
  const Protobuf::ServiceDescriptor* serviceDesc_;
  std::string methodName_;
};

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy