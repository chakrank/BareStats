#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <functional>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

namespace BareStats {

struct HttpResponse {
    int         status_code = 0; // 200, 503, 404, or -1 on network/timeout error
    std::string body;
    std::string error_message;

    bool is_success() const { return status_code >= 200 && status_code < 300; }
};

class HttpClient {
public:
    // Performs a non-blocking HTTP/1.1 GET request with strict timeout (default 300 ms)
    // and strict response body cap (default 64 KB) for DoS/OOM defense
    static HttpResponse Get(
        std::string_view url,
        uint32_t timeout_ms = 300,
        size_t max_body_bytes = 65536
    );

    // Parses "http://host:port/path" into host, port, and path
    static bool ParseUrl(
        std::string_view url,
        std::string& out_host,
        uint16_t& out_port,
        std::string& out_path
    );

    // Mock handler hook for deterministic offline testing
    using MockHandler = std::function<HttpResponse(std::string_view url, uint32_t timeout_ms, size_t max_body_bytes)>;
    static void SetMockHandler(MockHandler handler);
    static void ClearMockHandler();

    // Dechunks HTTP chunked transfer encoding ("Transfer-Encoding: chunked")
    static std::string Dechunk(std::string_view chunked_body);

    // DNS Resolution Caching Protection (plan_v1.4 Point 7.4)
    // Resolves host once upon connection, caching sockaddr_in in memory.
    // In failure mode, enforces a 10s backoff (failed_retry_sec) so unreachable nodes
    // never block the 1000ms loop for healthy rigs.
    static bool ResolveHost(
        const std::string& host,
        uint16_t port,
        sockaddr_in& out_addr,
        uint32_t ttl_sec = 60,
        uint32_t failed_retry_sec = 10
    );

    static void ClearDnsCache();
    static size_t GetDnsCacheSize();
    static bool IsHostCached(const std::string& host);
    static void InvalidateHost(const std::string& host);
};

} // namespace BareStats
