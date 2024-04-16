#pragma once

#include <memory>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "envoy/buffer/buffer.h"
#include "http_methods.h"
#include "http_body_utils.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

enum class TranscodingType { HttpJson, HttpBody };

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
   * @brief Query the data format a gRPC shall be mapped to.
   * @returns The type of mapping that should occur on success.
   *          An error is returned in case prepareTranscoding was not called.
   */
  absl::StatusOr<TranscodingType> mapRequestTo() const;

  /**
   * @brief Query the data format of response to map into gRPC.
   * @returns The type of mapping that should occur on success.
   *          An error is returned in case prepareTranscoding was not called.
   */
  absl::StatusOr<TranscodingType> mapResponseFrom() const;

  /**
   * @brief Convert gRPC request data to JSON data
   * @param[in] grpc_buffer buffer containing gRPC payload to try conversion on.
   * @returns On success: A string containing the JSON representation of @p grpc_buffer.
   *          On failure: The error indicating why the conversion failed.
   */
  absl::StatusOr<std::string> grpcRequestToJson(Buffer::Instance& grpc_buffer) const;

  /**
   * @brief Convert JSON response data to gRPC data
   * @param[in] json_data string containing JSON payload to try conversion on.
   * @returns On success: A string containing the gRPC representation of @p json_data.
   *          On failure: The error indicating why the conversion failed.
   */
  absl::StatusOr<std::string> jsonResponseToGrpc(std::string const& json_data) const;

  /**
   * @brief Convert gRPC request data to HTTP Body data
   * @param[in] grpc_buffer buffer containing gRPC payload to try conversion on.
   * @returns On success: A HTTP body protobuf message representing of @p grpc_buffer.
   *          On failure: The error indicating why the conversion failed.
   */
  absl::StatusOr<HttpBodyUtils::HttpBody>
  grpcRequestToHttpBody(Buffer::Instance& grpc_buffer) const;

  /**
   * @brief Convert HTTP Body message into gRPC response
   * @param[in] http_body_data http body message to try conversion on.
   * @returns On success: A series of bytes representing @p HTTP body protobuf message.
   *          On failure: The error indicating why the conversion failed.
   */
  absl::StatusOr<std::string>
  httpBodyResponseToGrpc(HttpBodyUtils::HttpBody const& http_body_data) const;

private:
  class Impl;
  std::unique_ptr<Impl> pimpl_;
};
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
