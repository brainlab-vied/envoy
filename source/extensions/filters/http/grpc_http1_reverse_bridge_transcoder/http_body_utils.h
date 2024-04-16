#pragma once
/**
 * NOTE: The http body utilities are a tweaked copy of the implementation files
 * from the grcp_json_transcoder extension. Since compilation units are not accessible
 * across extensions, we maintain our own variant. The main difference are the data types
 * in used.
 */
#include "absl/status/statusor.h"
#include "google/api/httpbody.pb.h"
#include "envoy/buffer/buffer.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder::HttpBodyUtils {

/// Shortcut to gRPCs built-in http body type
using HttpBody = ::google::api::HttpBody;
/// Type describing a protobuf messages sequence of fields / the inner structure.
using ProtoMessageFields = std::vector<ProtobufWkt::Field const*>;

/**
 * @brief Try to parse a HTTP Body message from a given buffer and a protobuf field
 * description of the assumed content in the buffer.
 * @param[in] buffer the buffer to read the http body message from.
 * @param[in] field_path Fields of a protobuf message that is assumed to be serialized in @p buffer.
 * @returns On success: The parsed HttpBody message.
 *          On failure: A non okay status containing the occurred error.
 */
absl::StatusOr<HttpBody> parseByMessageFields(Buffer::Instance& buffer,
                                              ProtoMessageFields const& field_path);

/**
 * @brief Try to serialize a HTTP Body message with the help protobuf field description.
 * into a series of bytes in gRPC wire format.
 * @param[in] http_body_data the http body message to serialize.
 * @param[in] field_path Fields of a protobuf message that contains the structure of the protobuf
 * fields to serialize.
 * @returns On success: A string containing @p http_body_data in gRPC wire format.
 *          On failure: A non okay status containing the occurred error.
 */
absl::StatusOr<std::string> serializeByMessageFields(HttpBody const& http_body_data,
                                                     ProtoMessageFields const& field_path);
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder::HttpBodyUtils
