#pragma once

#include <memory>
#include <absl/status/status.h>
#include <absl/status/statusor.h>
#include "http_methods.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

/**
 * Type handling the transformation from gRPC requests to JSON requests and from JSON responses to
 * gRPC responses.
 */
class Transcoder {
public:
  /**
   * @brief CTor.
   * @note Full object initialization might fail. To avoid raising exceptions within the Constructor
   *       the init() method was added. To construct a fully functional object call init() after
   *       construction.
   */
  Transcoder();
  ~Transcoder();

  /**
   * @brief Initialize the transcoder.
   * @param[in] api api used to access @p proto_descriptor_path.
   * @param[in] proto_descriptor_path path to the proto descriptor file used for transcoding.
   * @param[in] service_name the service name to transcode.
   * @returns Result of the initialization attempt.
   */
  absl::Status init(Api::Api& api, std::string const& proto_descriptor_path,
                    std::string const& service_name);

  /**
   * @brief Prepare the transcoder for transcoding a specific request.
   * @param[in] method_and_path HTTP method and path to transcode.
   * @returns Result of the preparation attempt. It fails if given path
   *          in @p method_and_path is can't be found in
   *          the underlying protobuf descriptor.
   */
  absl::Status prepareTranscoding(HttpMethodAndPath method_and_path);

  /**
   * @brief Get HTTP request path.
   * @returns Returns the updated HTTP path to use with the transcoded HTTP request.
   *          In case the path was not annotated in the protobuf descriptor, the
   *          current path is returned. The Method fails in case 
   *          "prepareTranscoding" was not called before.
   */
  absl::StatusOr<HttpPath> getHttpRequestPath() const;

  /**
   * @brief Convert gRPC request data to JSON data
   * @param[in] grpc_data string containing gRPC payload to try conversion on.
   * @returns On success: A string containing the JSON representation of @p grpc_data.
   *          On failure: The error indicating why the conversion failed.
   */
  absl::StatusOr<std::string> grpcRequestToJson(std::string const& grpc_data) const;

  /**
   * @brief Convert JSON response data to gRPC data
   * @param[in] json_data string containing JSON payload to try conversion on.
   * @returns On success: A string containing the gRPC representation of @p json_data.
   *          On failure: The error indicating why the conversion failed.
   */
  absl::StatusOr<std::string> jsonResponseToGrpc(std::string const& json_data) const;

private:
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
