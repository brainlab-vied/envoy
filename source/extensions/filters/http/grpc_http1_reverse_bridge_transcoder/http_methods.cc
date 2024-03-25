#include <algorithm>
#include <cctype>
#include <map>

#include "http_methods.h"

namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder {

absl::StatusOr<HttpMethod> httpMethodFrom(std::string_view maybe_http_method) {
  static const std::map<std::string, HttpMethod> mapping{
      {"GET", HttpMethod::GET},
      {"POST", HttpMethod::POST},
      {"PUT", HttpMethod::PUT},
      {"DELETE", HttpMethod::DELETE},
  };

  std::string normalized{maybe_http_method};
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](auto ch) { return std::toupper(ch); });

  auto const pos = mapping.find(normalized);
  if (pos == mapping.cend()) {
    return absl::NotFoundError(absl::StrCat("Unable to build HttpMethod from ", maybe_http_method));
  }
  return pos->second;
}

std::string httpMethodToString(HttpMethod method) {
  switch (method) {
  case HttpMethod::GET:
    return "GET";
  case HttpMethod::POST:
    return "POST";
  case HttpMethod::PUT:
    return "PUT";
  case HttpMethod::DELETE:
    return "DELETE";
  }
}
} // namespace Envoy::Extensions::HttpFilters::GrpcHttp1ReverseBridgeTranscoder
