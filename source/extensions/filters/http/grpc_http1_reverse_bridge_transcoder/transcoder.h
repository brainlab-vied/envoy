#pragma once

#include "envoy/server/filter_config.h"
#include "source/common/buffer/buffer_impl.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

class Transcoder {
public:
    Transcoder(Api::Api& api);

    std::pair<absl::Status, std::string> fromGrpcBufferToJson(Buffer::OwnedImpl& buffer);
    std::pair<absl::Status, std::string> fromJsonBufferToGrpc(Buffer::OwnedImpl& buffer);
private:
    Envoy::Protobuf::FileDescriptorSet descSet_;
    Protobuf::DescriptorPool descPool_;
    std::unique_ptr<Protobuf::util::TypeResolver> resolver_;
};

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
