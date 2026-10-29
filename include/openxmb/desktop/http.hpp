#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace openxmb::desktop {
struct HttpRequest {
  std::string url;
  std::vector<std::string> headers;
  std::size_t max_bytes{8u*1024u*1024u};
  unsigned timeout_seconds{20}, max_redirects{};
  bool sensitive{};
  std::function<bool(std::uint64_t,std::uint64_t)> progress;
};
struct HttpResponse {
  bool success{};
  long status{};
  std::string body, error;
};
using HttpTransport=std::function<HttpResponse(const HttpRequest&)>;
// Bounded HTTPS GET. Sensitive requests never redirect and no errors include
// URLs, headers, response bodies or credentials. Call outside the render thread.
[[nodiscard]] HttpResponse http_get(const HttpRequest& request);
} // namespace openxmb::desktop
