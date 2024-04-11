#include <unordered_map>
#include "google/api/annotations.pb.h"
#include "google/api/http.pb.h"
#include "google/api/httpbody.pb.h"
#include "http_methods.h"
#include "grpc_transcoding/type_helper.h"
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
  bool request_type_is_http_body;
  bool response_type_is_http_body;
};

std::string typeUrlFrom(Protobuf::Descriptor const* descriptor) {
  assert(descriptor);

  auto full_name = descriptor->full_name();

  if ((!full_name.empty()) && (full_name.front() != '/')) {
    return "/" + full_name;
  }
  return full_name;
}

absl::StatusOr<bool> isHttpBodyType(Protobuf::Descriptor const* descriptor,
                                    google::grpc::transcoding::TypeHelper const& type_helper,
                                    google::api::HttpRule const& http_rule) {
  // Constants
  static const auto http_body_type_name = google::api::HttpBody::descriptor()->full_name();

  // Try to lookup message type of given descriptor.
  const auto type_url = typeUrlFrom(descriptor);
  const auto* message_type = type_helper.Info()->GetTypeByTypeUrl(type_url);
  if (message_type == nullptr) {
    return absl::NotFoundError(
        absl::StrCat("Unable to find message type of type ", type_url, ". Abort."));
  }

  // Normalize and resolve field path from http rule attribute. Setting "*" maps to ""
  // The body field determines the top-level gRPC field that forms the sent HTTP messages body.
  // This is the gRPC field were we want to check if contains a http body message.
  std::string message_body_field_path{http_rule.body()};
  if (message_body_field_path == "*") {
    message_body_field_path.clear();
  }

  std::vector<ProtobufWkt::Field const*> message_body_fields;
  const auto status =
      type_helper.ResolveFieldPath(*message_type, message_body_field_path, &message_body_fields);
  if (!status.ok()) {
    return status;
  }

  // Examine protobuf fields of given type descriptor. If there are none
  // The given descriptor itself might be of type http body message, if there are fields
  // A http body messages can only have a single and it must match the type name.
  // If all of this is not fulfilled, it is no body message.
  if (message_body_fields.empty()) {
    return descriptor->full_name() == http_body_type_name;
  } else if (message_body_fields.size() == 1) {
    auto const* field_descriptor =
        type_helper.Info()->GetTypeByTypeUrl(message_body_fields.back()->type_url());
    return (field_descriptor && (field_descriptor->name() == http_body_type_name));
  }
  return false;
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
  absl::StatusOr<TranscodingType> mapRequestTo() const;
  absl::StatusOr<TranscodingType> mapResponseFrom() const;
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

  // Create temporary type helper to analyze given proto message
  auto type_helper = google::grpc::transcoding::TypeHelper{
      Protobuf::util::NewTypeResolverForDescriptorPool("", descriptors.get())};

  // Populate method resolver with all methods in given service
  MethodInfoMap grpc_method_infos;
  for (auto i = 0; i < service_descriptor->method_count(); ++i) {
    auto const* method_descriptor = service_descriptor->method(i);
    auto http_rule = google::api::HttpRule{};

    if (method_descriptor->options().HasExtension(google::api::http)) {
      http_rule = method_descriptor->options().GetExtension(google::api::http);
    }

    // NOTE: We examine the input and output type to figure out if it is a http_body message.
    // Although http body messages are normal fields that can occur in any message
    // We care only about to level definitions. No recursive message field tree resolution happens
    // here.
    auto const* request_descriptor = method_descriptor->input_type();
    auto const is_request_http_body_or = isHttpBodyType(request_descriptor, type_helper, http_rule);
    if (!is_request_http_body_or.ok()) {
      return absl::InternalError(
          absl::StrCat("Failed to determine if request type ", request_descriptor->full_name(),
                       " is http body. Error was: ", is_request_http_body_or.status().message()));
    }
    ENVOY_LOG(debug, "Is Request Type a HTTP Body Message: {}", *is_request_http_body_or);

    auto const* response_descriptor = method_descriptor->output_type();
    auto const is_response_http_body_or =
        isHttpBodyType(response_descriptor, type_helper, http_rule);
    if (!is_response_http_body_or.ok()) {
      return absl::InternalError(
          absl::StrCat("Failed to determine if response type ", response_descriptor->full_name(),
                       " is http body. Error was: ", is_response_http_body_or.status().message()));
    }
    ENVOY_LOG(debug, "Is Response Type a HTTP Body Message: {}", *is_response_http_body_or);

    // Create method info and store it.
    auto method_info =
        MethodInfo{method_descriptor, request_descriptor,       response_descriptor,
                   http_rule,         *is_request_http_body_or, *is_response_http_body_or};

    ENVOY_LOG(debug, "Store method descriptors for: {}", method_descriptor->name());
    grpc_method_infos.emplace(method_descriptor->name(), std::move(method_info));
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

  auto const& path = method_and_path.path;
  auto const stripped_path = path.substr(path.find_last_of('/') + 1);
  auto const pos = grpc_method_infos_.find(stripped_path);

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

absl::StatusOr<TranscodingType> Transcoder::Impl::mapRequestTo() const {
  assert(isInitialized());
  if (selected_grpc_method_ == nullptr) {
    return absl::FailedPreconditionError("No method to transcode selected. Abort.");
  }

  if (selected_grpc_method_->request_type_is_http_body) {
    return TranscodingType::HttpBody;
  }
  return TranscodingType::HttpJson;
}

absl::StatusOr<TranscodingType> Transcoder::Impl::mapResponseFrom() const {
  assert(isInitialized());
  if (selected_grpc_method_ == nullptr) {
    return absl::FailedPreconditionError("No method to transcode selected. Abort.");
  }

  if (selected_grpc_method_->response_type_is_http_body) {
    return TranscodingType::HttpBody;
  }
  return TranscodingType::HttpJson;
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

  auto const url = typeUrlFrom(selected_grpc_method_->request_descriptor);
  ENVOY_LOG(debug, "Attempt transcoding of type url {} to JSON", url);

  auto const status =
      Protobuf::util::BinaryToJsonString(type_resolver_.get(), url, grpc, &json, options);
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

  auto const url = typeUrlFrom(selected_grpc_method_->response_descriptor);
  ENVOY_LOG(debug, "Attempt transcoding of type url {} to GRPC", url);

  auto const status =
      Protobuf::util::JsonToBinaryString(type_resolver_.get(), url, json, &grpc, options);
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

absl::StatusOr<TranscodingType> Transcoder::mapRequestTo() const { return pimpl_->mapRequestTo(); }
absl::StatusOr<TranscodingType> Transcoder::mapResponseFrom() const {
  return pimpl_->mapResponseFrom();
}

absl::StatusOr<std::string> Transcoder::grpcRequestToJson(std::string const& grpc_data) const {
  return pimpl_->grpcRequestToJson(grpc_data);
}

absl::StatusOr<std::string> Transcoder::jsonResponseToGrpc(std::string const& json_data) const {
  return pimpl_->jsonResponseToGrpc(json_data);
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
