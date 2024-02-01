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
        throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Failed to parse /home/aled/envoy/custom_filter_test/hello.pb");
    }

    for (const auto& file : descSet_.file()) {
        descPool_.BuildFile(file);
    }

    auto service = descPool_.FindServiceByName("endpoints.Greeter"); // TODO: Service name as a param
    if (!service) {
        throw EnvoyException("GrpcHttp1ReverseBridgeTranscoder: Could not find 'endpoints.Greeter' in the proto descriptor");
    }

    resolver_ = std::unique_ptr<Protobuf::util::TypeResolver> {
        Protobuf::util::NewTypeResolverForDescriptorPool("", std::addressof(descPool_))
    };

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

    auto status = Protobuf::util::BinaryToJsonStream(resolver_.get(), "/endpoints.HelloRequest", 
                                                        std::addressof(inputStream), std::addressof(outputStream), opts);

    return {status, outputData};
}

std::pair<absl::Status, std::string> Transcoder::fromJsonBufferToGrpc(Buffer::OwnedImpl& buffer) {
    const auto inputData = buffer.toString();
    Protobuf::io::ArrayInputStream inputStream(inputData.data(), inputData.size());

    std::string outputData;
    Protobuf::io::StringOutputStream outputStream{std::addressof(outputData)};

    Protobuf::util::JsonParseOptions opts;

    auto status = Protobuf::util::JsonToBinaryStream(resolver_.get(), "/endpoints.HelloReply", 
                                        std::addressof(inputStream), std::addressof(outputStream), opts);

    return {status, outputData};
}

} // namespace GrpcHttp1ReverseBridgeTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
