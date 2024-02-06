#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/transcoder.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

Transcoder::Transcoder(Api::Api& api, std::string proto_descriptor, std::string service_name) {
  auto fileOrError = api.fileSystem().fileReadToEnd(proto_descriptor);
  THROW_IF_STATUS_NOT_OK(fileOrError, throw);

  bool parsed = desc_set_.ParseFromString(fileOrError.value());
  if (!parsed) {
    throw EnvoyException(
        absl::StrCat("GrpcHttp1ReverseBridgeTranscoder: Failed to parse ", proto_descriptor));
  }

  for (const auto& file : desc_set_.file()) {
    desc_pool_.BuildFile(file);
  }

  resolver_ = std::unique_ptr<Protobuf::util::TypeResolver>{
      Protobuf::util::NewTypeResolverForDescriptorPool("", std::addressof(desc_pool_))};

  if (!resolver_) {
    throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Failed to create TypeResolver");
  }

  service_desc_ = desc_pool_.FindServiceByName(service_name);
  if (!service_desc_) {
    throw EnvoyException(absl::StrCat("GrpcHttp1ReverseBridgeTranscoder: Could not find '",
                                      service_name, "' in the proto descriptor"));
  }

  for (int i = 0; i < service_desc_->method_count(); i++) {
    const auto* method_desc = service_desc_->method(i);

    google::api::HttpRule http_rule;
    if (method_desc->options().HasExtension(google::api::http)) {
      http_rule = method_desc->options().GetExtension(google::api::http);
    }

    method_resolver_.emplace(method_desc->name(),
                             MethodInfo{.method_desc = method_desc,
                                        .request_desc = method_desc->input_type(),
                                        .response_desc = method_desc->output_type(),
                                        .http_rule = http_rule});
  }
}

std::pair<absl::Status, std::string> Transcoder::fromGrpcBufferToJson(Buffer::OwnedImpl& buffer) {
  auto inputData = buffer.toString();
  Protobuf::io::ArrayInputStream inputStream(inputData.data(), inputData.size());

  std::string outputData;
  Protobuf::io::StringOutputStream outputStream{std::addressof(outputData)};

  Protobuf::util::JsonPrintOptions opts;

  if (method_resolver_.contain(method_name_)) {

    auto status = Protobuf::util::BinaryToJsonStream(
        resolver_.get(), "/" + method_resolver_.findRequestDescByMethod(method_name_)->full_name(),
        std::addressof(inputStream), std::addressof(outputStream), opts);

    return {status, outputData};
  } else {
    return {absl::InternalError(absl::StrCat("Method does not exist: ", method_name_)), {}};
  }
}

std::pair<absl::Status, std::string> Transcoder::fromJsonBufferToGrpc(Buffer::OwnedImpl& buffer) {
  const auto inputData = buffer.toString();
  Protobuf::io::ArrayInputStream inputStream(inputData.data(), inputData.size());

  std::string outputData;
  Protobuf::io::StringOutputStream outputStream{std::addressof(outputData)};

  Protobuf::util::JsonParseOptions opts;

  if (method_resolver_.contain(method_name_)) {

    auto status = Protobuf::util::JsonToBinaryStream(
        resolver_.get(), "/" + method_resolver_.findResponseDescByMethod(method_name_)->full_name(),
        std::addressof(inputStream), std::addressof(outputStream), opts);

    return {status, outputData};
  } else {
    return {absl::InternalError(absl::StrCat("Method does not exist: ", method_name_)), {}};
  }
}

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy 