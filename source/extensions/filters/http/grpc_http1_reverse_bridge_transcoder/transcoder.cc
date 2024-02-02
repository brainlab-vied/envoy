#include "source/extensions/filters/http/grpc_http1_reverse_bridge_transcoder/transcoder.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridgeTranscoder {

Transcoder::Transcoder(Api::Api& api) {
  auto fileOrError = api.fileSystem().fileReadToEnd("/home/aled/envoy/custom_filter_test/hello.pb");
  THROW_IF_STATUS_NOT_OK(fileOrError, throw);

  bool parsed = descSet_.ParseFromString(fileOrError.value());
  if (!parsed) {
    throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Failed to parse "
                         "/home/aled/envoy/custom_filter_test/hello.pb");
  }

  for (const auto& file : descSet_.file()) {
    descPool_.BuildFile(file);
  }

  serviceDesc_ = descPool_.FindServiceByName("endpoints.Greeter"); // TODO: Service name as a param
  if (!serviceDesc_) {
    throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Could not find 'endpoints.Greeter' in "
                         "the proto descriptor");
  }

  resolver_ = std::unique_ptr<Protobuf::util::TypeResolver>{
      Protobuf::util::NewTypeResolverForDescriptorPool("", std::addressof(descPool_))};

  if (!resolver_) {
    throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Failed to create TypeResolver");
  }
}

std::pair<absl::Status, std::string> Transcoder::fromGrpcBufferToJson(Buffer::OwnedImpl& buffer) {
  auto inputData = buffer.toString();
  Protobuf::io::ArrayInputStream inputStream(inputData.data(), inputData.size());

  std::string outputData;
  Protobuf::io::StringOutputStream outputStream{std::addressof(outputData)};

  Protobuf::util::JsonPrintOptions opts;

  const auto* methodDesc = serviceDesc_->FindMethodByName("SayHello");

  if (!methodDesc) {
    throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Method does not exist: endpoints.Greeter/SayHello");    
  }

  auto status = Protobuf::util::BinaryToJsonStream(resolver_.get(), "/" + methodDesc->input_type()->full_name(),
                                                   std::addressof(inputStream),
                                                   std::addressof(outputStream), opts);

  return {status, outputData};
}

std::pair<absl::Status, std::string> Transcoder::fromJsonBufferToGrpc(Buffer::OwnedImpl& buffer) {
  const auto inputData = buffer.toString();
  Protobuf::io::ArrayInputStream inputStream(inputData.data(), inputData.size());

  std::string outputData;
  Protobuf::io::StringOutputStream outputStream{std::addressof(outputData)};

  Protobuf::util::JsonParseOptions opts;

  const auto* methodDesc = serviceDesc_->FindMethodByName("SayHello");

  if (!methodDesc) {
    throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Method does not exist: endpoints.Greeter/SayHello");    
  }
  
  auto status = Protobuf::util::JsonToBinaryStream(
      resolver_.get(), "/" + methodDesc->output_type()->full_name(), std::addressof(inputStream),
      std::addressof(outputStream), opts);

  return {status, outputData};
}

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy