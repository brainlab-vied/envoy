#pragma once

#include "envoy/server/filter_config.h"
#include "source/common/buffer/buffer_impl.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"
#include "source/common/common/logger.h"

#include "google/api/annotations.pb.h"
#include "google/api/http.pb.h"
#include "google/api/httpbody.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

struct MethodInfo
{
    const Protobuf::MethodDescriptor* method_desc;
    const Protobuf::Descriptor* request_desc;
    const Protobuf::Descriptor* response_desc;
    google::api::HttpRule http_rule;
};

struct MethodInfoResolver {
    MethodInfoResolver() = default;

    void emplace(std::string_view name, const MethodInfo& method) {
        method_info_map_.emplace(name, method);
    }

    bool contain(const std::string& name) { 
        return method_info_map_.find(name) != method_info_map_.end(); 
    }

    const Protobuf::Descriptor* findRequestDescByMethod(const std::string& name) {
        return method_info_map_.find(name)->second.request_desc;
    }

    const Protobuf::Descriptor* findResponseDescByMethod(const std::string& name) {
        return method_info_map_.find(name)->second.response_desc;
    }
private:
    std::unordered_map<std::string, MethodInfo> method_info_map_;
};

class Transcoder : public Logger::Loggable<Logger::Id::filter> {
public:
  Transcoder(Api::Api& api, const std::string& proto_descriptor, const std::string& service_name);

  std::pair<absl::Status, std::string> fromGrpcBufferToJson(Buffer::OwnedImpl& buffer);
  std::pair<absl::Status, std::string> fromJsonBufferToGrpc(Buffer::OwnedImpl& buffer);

  void setCurrentMethod(std::string_view name) { method_name_ = name.substr(name.find_last_of('/') + 1); }

private:
  Envoy::Protobuf::FileDescriptorSet desc_set_;
  Protobuf::DescriptorPool desc_pool_;
  std::unique_ptr<Protobuf::util::TypeResolver> resolver_;
  const Protobuf::ServiceDescriptor* service_desc_;
  MethodInfoResolver method_resolver_;
  std::string method_name_;
};

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy