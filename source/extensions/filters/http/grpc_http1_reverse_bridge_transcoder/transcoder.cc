#include <unordered_map>
#include "google/api/annotations.pb.h"
#include "google/api/http.pb.h"
#include "http_methods.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"
#include "source/common/common/logger.h"

#include "transcoder.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {
namespace {
struct MethodInfo {
  Protobuf::MethodDescriptor const* method_descriptor;
  Protobuf::Descriptor const* request_descriptor;
  Protobuf::Descriptor const* response_descriptor;
  google::api::HttpRule http_rule;
};

std::string typeUrlFrom(Protobuf::Descriptor const* descriptor) {
  assert(descriptor);

  auto full_name = descriptor->full_name();

  if ((!full_name.empty()) && (full_name.front() != '/')) {
    return "/" + full_name;
  }
  return full_name;
}
} // namespace

class Transcoder::Impl : public Logger::Loggable<Logger::Id::filter> {
private:
  using DescriptorPoolPtr = std::unique_ptr<Protobuf::DescriptorPool>;
  using TypeResolverPtr = std::unique_ptr<Protobuf::util::TypeResolver>;
  using MethodName = std::string;
  using MethodInfoMap = std::unordered_map<MethodName, MethodInfo>;

public:
  absl::Status init(Api::Api& api, std::string const& proto_descriptor_path,
                    std::string const& service_name);
  absl::Status prepareTranscoding(HttpMethodAndPath method_and_path);
  absl::StatusOr<std::string> getHttpRequestPath() const;
  absl::StatusOr<std::string> grpcRequestToJson(std::string const& grpc_data) const;
  absl::StatusOr<std::string> jsonResponseToGrpc(std::string const& json_data) const;

private:
  bool isInitialized() const;

private:
  DescriptorPoolPtr descriptors_;
  TypeResolverPtr type_resolver_;
  MethodInfoMap grpc_method_infos_;
  MethodInfo const* selected_grpc_method_;
  HttpMethodAndPath selected_http_method_and_path_;
};

absl::Status Transcoder::Impl::init(Api::Api& api, std::string const& proto_descriptor_path,
                                    std::string const& service_name) {
  if (isInitialized()) {
    ENVOY_LOG(debug, "Transcoder was already initialized. Do nothing.");
    return absl::OkStatus();
  }

  // Try to parse given proto descriptor path and collect all contained descriptors
  auto file_or_error = api.fileSystem().fileReadToEnd(proto_descriptor_path);
  if (!file_or_error.ok()) {
    return absl::NotFoundError(absl::StrCat("Failed to read file: ", proto_descriptor_path));
  }

  auto file_descriptor_set = Protobuf::FileDescriptorSet();
  auto ok = file_descriptor_set.ParseFromString(file_or_error.value());
  if (!ok) {
    return absl::InternalError(
        absl::StrCat("Failed to parse proto descriptors from file: ", proto_descriptor_path));
  }

  auto descriptors = std::make_unique<Protobuf::DescriptorPool>();
  for (const auto& file : file_descriptor_set.file()) {
    descriptors->BuildFile(file);
  }

  // Create type resolver from descriptors and the requested service description.
  auto type_resolver =
      TypeResolverPtr{Protobuf::util::NewTypeResolverForDescriptorPool("", descriptors.get())};
  if (!type_resolver) {
    return absl::InternalError("Failed to Construct Type resolver from givne Descriptor Pool");
  }

  auto service_descriptor = descriptors->FindServiceByName(service_name);
  if (!service_descriptor) {
    return absl::NotFoundError(
        absl::StrCat("Failed to find service descriptor of: ", service_name));
  }

  // Populate method resolver with all methods in given service
  MethodInfoMap grpc_method_infos;
  for (auto i = 0; i < service_descriptor->method_count(); ++i) {
    auto const* method_descriptor = service_descriptor->method(i);
    auto http_rule = google::api::HttpRule{};

    if (method_descriptor->options().HasExtension(google::api::http)) {
      http_rule = method_descriptor->options().GetExtension(google::api::http);
    }

    ENVOY_LOG(debug, "Store method descriptors for: {}", method_descriptor->name());
    grpc_method_infos.emplace(method_descriptor->name(),
                              MethodInfo{method_descriptor, method_descriptor->input_type(),
                                         method_descriptor->output_type(), http_rule});
  }

  // From here on, nothing can fail on initialization anymore.
  // Transfer ownership of owned properties.
  descriptors_ = std::move(descriptors);
  type_resolver_ = std::move(type_resolver);
  grpc_method_infos_ = std::move(grpc_method_infos);
  selected_grpc_method_ = nullptr;
  return absl::OkStatus();
}

bool Transcoder::Impl::isInitialized() const { return descriptors_ && type_resolver_; }

absl::Status Transcoder::Impl::prepareTranscoding(HttpMethodAndPath method_and_path) {
  assert(isInitialized());

  auto& path = method_and_path.path;
  auto stripped_path = path.substr(path.find_last_of('/') + 1);
  auto pos = grpc_method_infos_.find(stripped_path);

  if (pos == grpc_method_infos_.cend()) {
    selected_grpc_method_ = nullptr;
    return absl::NotFoundError(absl::StrCat("Failed to find path: ", path));
  }

  ENVOY_LOG(debug, "Prepared for transcoding method: {}", path);
  selected_grpc_method_ = &(pos->second);
  selected_http_method_and_path_ = std::move(method_and_path);
  return absl::OkStatus();
}

absl::StatusOr<HttpPath> Transcoder::Impl::getHttpRequestPath() const {
  // NOTE: Try to figure out if the protobuf definition contains attributes
  // reroute the path of the HTML request surrounding the gRPC Request.
  // If not, keep using the original request.
  assert(isInitialized());

  if (selected_grpc_method_ == nullptr) {
    return absl::FailedPreconditionError("No method to transcode selected. Abort.");
  }
  auto const& http_rule = selected_grpc_method_->http_rule;
  auto const& http_method = selected_http_method_and_path_.method;
  auto const& http_path = selected_http_method_and_path_.path;

  std::string new_http_path;
  switch (http_method) {
  case HttpMethod::GET:
    new_http_path = http_rule.get();
    break;
  case HttpMethod::POST:
    new_http_path = http_rule.post();
    break;
  case HttpMethod::PUT:
    new_http_path = http_rule.put();
    break;
  case HttpMethod::DELETE:
    new_http_path = http_rule.delete_();
    break;
  };

  if (new_http_path.empty()) {
    ENVOY_LOG(debug, "No path for HTTP method {} found. Use original path {}",
              httpMethodToString(http_method), http_path);
    return http_path;
  }

  ENVOY_LOG(debug, "New path for HTTP method {} found. Use path {}",
            httpMethodToString(http_method), new_http_path);
  return new_http_path;
}

absl::StatusOr<std::string> Transcoder::Impl::grpcRequestToJson(std::string const& grpc) const {
  assert(isInitialized());

  if (selected_grpc_method_ == nullptr) {
    return absl::FailedPreconditionError("No method to transcode selected. Abort.");
  }

  std::string json;
  Protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  options.always_print_primitive_fields = true;

  auto url = typeUrlFrom(selected_grpc_method_->request_descriptor);
  ENVOY_LOG(debug, "Attempt transcoding of type url {} to JSON", url);

  auto status = Protobuf::util::BinaryToJsonString(type_resolver_.get(), url, grpc, &json, options);
  if (status.ok()) {
    return json;
  }
  return status;
}

absl::StatusOr<std::string> Transcoder::Impl::jsonResponseToGrpc(std::string const& json) const {
  assert(isInitialized());

  if (selected_grpc_method_ == nullptr) {
    return absl::FailedPreconditionError("No method to transcode selected. Abort.");
  }

  std::string grpc;
  Protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = false;
  options.case_insensitive_enum_parsing = true;

  auto url = typeUrlFrom(selected_grpc_method_->response_descriptor);
  ENVOY_LOG(debug, "Attempt transcoding of type url {} to GRPC", url);

  auto status = Protobuf::util::JsonToBinaryString(type_resolver_.get(), url, json, &grpc, options);
  if (status.ok()) {
    return grpc;
  }
  return status;
}

// Pimpl call delegations
Transcoder::Transcoder() : pimpl_{std::make_unique<Impl>()} {}

Transcoder::~Transcoder() = default;

absl::Status Transcoder::init(Api::Api& api, const std::string& proto_descriptor,
                              const std::string& service_name) {
  return pimpl_->init(api, proto_descriptor, service_name);
}

absl::Status Transcoder::prepareTranscoding(HttpMethodAndPath method_and_path) {
  return pimpl_->prepareTranscoding(std::move(method_and_path));
}

absl::StatusOr<HttpPath> Transcoder::getHttpRequestPath() const {
  return pimpl_->getHttpRequestPath();
}

absl::StatusOr<std::string> Transcoder::grpcRequestToJson(std::string const& grpc_data) const {
  return pimpl_->grpcRequestToJson(grpc_data);
}

absl::StatusOr<std::string> Transcoder::jsonResponseToGrpc(std::string const& json_data) const {
  return pimpl_->jsonResponseToGrpc(json_data);
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
