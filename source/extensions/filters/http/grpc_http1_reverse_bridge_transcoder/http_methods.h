#pragma once

#include <string_view>
#include <absl/status/statusor.h>

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

/// String representing a http path
using HttpPath = std::string;

/**
 * Enum containing all supported HTTP Method types that might come up
 * during encoding.
 */
enum class HttpMethod { GET, POST, PUT, DELETE };

/**
 * Try to convert a given string_view to the HttpMethod enum.
 * @param[in] maybe_http_method a string_view possibly containing an enum value.
 * @returns On success: the converted enum.
 *          On failure: A status containing an error.
 */
absl::StatusOr<HttpMethod> httpMethodFrom(std::string_view maybe_http_method);

/**
 * Convert HttpMethod enum value to a string.
 * @param[in] method the http method to convert into a string.
 * @returns The string representation of @p method.
 */
std::string httpMethodToString(HttpMethod method);

/**
 * Type tying a http method and a path together
 */
struct HttpMethodAndPath {
  HttpMethod method;
  HttpPath path;
};

} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
