// TODO: Cleanup: Add documentation/header
#pragma once

#include <memory>
#include <absl/status/status.h>
#include <absl/status/statusor.h>

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

// TODO: Cleanup: Document interface
class Transcoder {
public:
  Transcoder();
  ~Transcoder();

  absl::Status init(Api::Api& api, std::string const& proto_descriptor, std::string const& service_name);
  absl::Status prepareTranscoding(std::string const& method_name);

  absl::StatusOr<std::string> grpcRequestToJson(std::string const& grpc_data) const;
  absl::StatusOr<std::string> jsonResponseToGrpc(std::string const& json_data) const;

private:
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
