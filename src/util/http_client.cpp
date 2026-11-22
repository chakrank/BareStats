#include "http_client.h"

#include <cstring>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
constexpr SocketType INVALID_SOCK = INVALID_SOCKET;
#define CLOSESOCK(s) closesocket(s)
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
using SocketType = int;
constexpr SocketType INVALID_SOCK = -1;
#define CLOSESOCK(s) close(s)
#define SOCKET_ERROR -1
#endif

namespace BareStats {

#if defined(_WIN32)
static void EnsureWinsock() {
    static bool init = []() {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)init;
}
#endif

bool HttpClient::ParseUrl(
    std::string_view url,
    std::string& out_host,
    uint16_t& out_port,
    std::string& out_path
) {
    if (url.empty()) return false;

    // Reject URLs containing whitespace, \r, \n, or ASCII control characters
    for (unsigned char c : url) {
        if (std::isspace(c) || c < 0x20 || c == 0x7F) {
            return false;
        }
    }

    std::string_view u = url;
    if (u.rfind("http://", 0) == 0) {
        u.remove_prefix(7);
    } else if (u.rfind("https://", 0) == 0) {
        // Telemetry scrapers use unencrypted HTTP for local engines
        return false;
    }

    // Split host:port from path
    size_t slash_pos = u.find('/');
    std::string_view host_port = (slash_pos != std::string_view::npos) ? u.substr(0, slash_pos) : u;
    out_path = (slash_pos != std::string_view::npos) ? std::string(u.substr(slash_pos)) : "/";

    // Split host and port
    size_t colon_pos = host_port.find(':');
    if (colon_pos != std::string_view::npos) {
        out_host = std::string(host_port.substr(0, colon_pos));
        std::string port_str(host_port.substr(colon_pos + 1));
        try {
            int p = std::stoi(port_str);
            if (p <= 0 || p > 65535) return false;
            out_port = static_cast<uint16_t>(p);
        } catch (...) {
            return false;
        }
    } else {
        out_host = std::string(host_port);
        out_port = 80;
    }

    return !out_host.empty();
}

static bool SetNonBlocking(SocketType sock) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string HttpClient::Dechunk(std::string_view chunked_body) {
    std::string result;
    result.reserve(chunked_body.size());

    size_t pos = 0;
    const size_t len = chunked_body.size();

    while (pos < len) {
        size_t line_end = chunked_body.find("\r\n", pos);
        if (line_end == std::string_view::npos) break;

        std::string_view hex_str = chunked_body.substr(pos, line_end - pos);
        // Ignore chunk extensions if present
        size_t semi = hex_str.find(';');
        if (semi != std::string_view::npos) {
            hex_str = hex_str.substr(0, semi);
        }

        size_t chunk_size = 0;
        try {
            chunk_size = std::stoul(std::string(hex_str), nullptr, 16);
        } catch (...) {
            break;
        }

        if (chunk_size == 0) break; // Terminal 0-size chunk

        pos = line_end + 2; // Skip \r\n
        // Overflow-safe bounds check: pos + chunk_size > len rewritten without addition
        if (pos > len || chunk_size > len - pos) {
            // Truncated chunk or invalid length
            result.append(chunked_body.substr(pos));
            break;
        }

        result.append(chunked_body.substr(pos, chunk_size));
        pos += chunk_size + 2; // Skip chunk data + trailing \r\n
    }

    return result;
}

// --- DNS Resolution Caching Protection ---

struct DnsCacheEntry {
    sockaddr_in address{};
    std::chrono::steady_clock::time_point expires_at;
    bool is_failed = false;
};

static std::mutex g_dns_mutex;
static std::unordered_map<std::string, DnsCacheEntry> g_dns_cache;

bool HttpClient::ResolveHost(
    const std::string& host,
    uint16_t port,
    sockaddr_in& out_addr,
    uint32_t ttl_sec,
    uint32_t failed_retry_sec
) {
#if defined(_WIN32)
    EnsureWinsock();
#endif

    // 1. Fast path: Check if host is already an IPv4 literal
    sockaddr_in direct{};
    direct.sin_family = AF_INET;
    direct.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &direct.sin_addr) > 0) {
        out_addr = direct;
        return true;
    }

    // 2. Check DNS cache under lock
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_dns_mutex);
        auto it = g_dns_cache.find(host);
        if (it != g_dns_cache.end()) {
            if (now < it->second.expires_at) {
                if (it->second.is_failed) {
                    // Still in failed backoff window: fail fast to avoid blocking 1000ms loop
                    return false;
                }
                out_addr = it->second.address;
                out_addr.sin_port = htons(port);
                return true;
            }
        }
    }

    // 3. Cache miss or expired: Resolve via getaddrinfo
    struct addrinfo hints{};
    hints.ai_family = AF_INET; // IPv4 preferred
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);

    if (rc != 0 || !res) {
        // Record failed resolution with short backoff (e.g. 10s)
        std::lock_guard<std::mutex> lock(g_dns_mutex);
        DnsCacheEntry entry{};
        entry.is_failed = true;
        entry.expires_at = now + std::chrono::seconds(failed_retry_sec);
        g_dns_cache[host] = entry;
        return false;
    }

    sockaddr_in resolved = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);

    // 4. Record successful resolution with TTL
    {
        std::lock_guard<std::mutex> lock(g_dns_mutex);
        DnsCacheEntry entry{};
        entry.address = resolved;
        entry.is_failed = false;
        entry.expires_at = now + std::chrono::seconds(ttl_sec);
        g_dns_cache[host] = entry;
    }

    out_addr = resolved;
    out_addr.sin_port = htons(port);
    return true;
}

void HttpClient::ClearDnsCache() {
    std::lock_guard<std::mutex> lock(g_dns_mutex);
    g_dns_cache.clear();
}

size_t HttpClient::GetDnsCacheSize() {
    std::lock_guard<std::mutex> lock(g_dns_mutex);
    return g_dns_cache.size();
}

bool HttpClient::IsHostCached(const std::string& host) {
    std::lock_guard<std::mutex> lock(g_dns_mutex);
    auto it = g_dns_cache.find(host);
    if (it != g_dns_cache.end()) {
        return !it->second.is_failed;
    }
    return false;
}

void HttpClient::InvalidateHost(const std::string& host) {
    std::lock_guard<std::mutex> lock(g_dns_mutex);
    g_dns_cache.erase(host);
}

static std::mutex g_mock_mutex;
static HttpClient::MockHandler g_mock_handler = nullptr;

void HttpClient::SetMockHandler(MockHandler handler) {
    std::lock_guard<std::mutex> lock(g_mock_mutex);
    g_mock_handler = std::move(handler);
}

void HttpClient::ClearMockHandler() {
    std::lock_guard<std::mutex> lock(g_mock_mutex);
    g_mock_handler = nullptr;
}

HttpResponse HttpClient::Get(
    std::string_view url,
    uint32_t timeout_ms,
    size_t max_body_bytes
) {
    {
        std::lock_guard<std::mutex> lock(g_mock_mutex);
        if (g_mock_handler) {
            return g_mock_handler(url, timeout_ms, max_body_bytes);
        }
    }

#if defined(_WIN32)
    EnsureWinsock();
#endif

    HttpResponse resp;
    resp.status_code = -1;

    std::string host, path;
    uint16_t port = 80;
    if (!ParseUrl(url, host, port, path)) {
        resp.error_message = "Invalid URL format";
        return resp;
    }

    // Resolve address using in-memory DNS cache with failure backoff
    sockaddr_in dest_addr{};
    if (!ResolveHost(host, port, dest_addr)) {
        resp.error_message = "DNS resolution failed";
        return resp;
    }

    SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCK) {
        resp.error_message = "Socket creation failed";
        return resp;
    }

#if !defined(_WIN32)
    if (sock < 0 || sock >= FD_SETSIZE) {
        CLOSESOCK(sock);
        resp.error_message = "Socket descriptor exceeds FD_SETSIZE";
        return resp;
    }
#endif

    // Set non-blocking mode
    if (!SetNonBlocking(sock)) {
        CLOSESOCK(sock);
        resp.error_message = "Failed to set non-blocking socket";
        return resp;
    }

    auto start_time = std::chrono::steady_clock::now();
    auto deadline = start_time + std::chrono::milliseconds(timeout_ms);

    // Non-blocking connect
    int c_res = connect(sock, reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));

    if (c_res == SOCKET_ERROR) {
#if defined(_WIN32)
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            CLOSESOCK(sock);
            resp.error_message = "Connection failed immediately";
            return resp;
        }
#else
        if (errno != EINPROGRESS) {
            CLOSESOCK(sock);
            resp.error_message = "Connection failed immediately";
            return resp;
        }
#endif

        // Wait for connect completion via select()
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            CLOSESOCK(sock);
            resp.error_message = "Connection timeout";
            return resp;
        }

#if !defined(_WIN32)
        if (sock < 0 || sock >= FD_SETSIZE) {
            CLOSESOCK(sock);
            resp.error_message = "Socket descriptor exceeds FD_SETSIZE";
            return resp;
        }
#endif
        uint32_t rem_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(sock, &wfds);
        FD_SET(sock, &efds);

        timeval tv{};
        tv.tv_sec = rem_ms / 1000;
        tv.tv_usec = (rem_ms % 1000) * 1000;

        int sel = select(static_cast<int>(sock + 1), nullptr, &wfds, &efds, &tv);
        if (sel <= 0) {
            CLOSESOCK(sock);
            resp.error_message = (sel == 0) ? "Connection timeout" : "Select failed";
            return resp;
        }

        int so_error = 0;
#if defined(_WIN32)
        int optlen = sizeof(so_error);
#else
        socklen_t optlen = sizeof(so_error);
#endif
        getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &optlen);
        if (so_error != 0) {
            CLOSESOCK(sock);
            resp.error_message = "Connection rejected";
            return resp;
        }
    }

    // Build standard HTTP/1.1 request
    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "User-Agent: BareStats/1.4\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n";

    // Send request
    size_t total_sent = 0;
    while (total_sent < request.size()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            CLOSESOCK(sock);
            resp.error_message = "Write timeout";
            return resp;
        }

#if !defined(_WIN32)
        if (sock < 0 || sock >= FD_SETSIZE) {
            CLOSESOCK(sock);
            resp.error_message = "Socket descriptor exceeds FD_SETSIZE";
            return resp;
        }
#endif
        uint32_t rem_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        timeval tv{};
        tv.tv_sec = rem_ms / 1000;
        tv.tv_usec = (rem_ms % 1000) * 1000;

        int sel = select(static_cast<int>(sock + 1), nullptr, &wfds, nullptr, &tv);
        if (sel <= 0) {
            CLOSESOCK(sock);
            resp.error_message = "Write select timeout";
            return resp;
        }

        int sent = send(sock, request.data() + total_sent, static_cast<int>(request.size() - total_sent), 0);
        if (sent <= 0) {
            CLOSESOCK(sock);
            resp.error_message = "Send failed";
            return resp;
        }
        total_sent += sent;
    }

    // Receive response up to max_body_bytes + 4096 (headers)
    std::string raw_response;
    char buffer[4096];
    const size_t max_total = max_body_bytes + 4096;

    while (raw_response.size() < max_total) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

#if !defined(_WIN32)
        if (sock < 0 || sock >= FD_SETSIZE) {
            CLOSESOCK(sock);
            resp.error_message = "Socket descriptor exceeds FD_SETSIZE";
            return resp;
        }
#endif
        uint32_t rem_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        timeval tv{};
        tv.tv_sec = rem_ms / 1000;
        tv.tv_usec = (rem_ms % 1000) * 1000;

        int sel = select(static_cast<int>(sock + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) break; // Timeout or error

        int bytes_read = recv(sock, buffer, sizeof(buffer), 0);
        if (bytes_read == 0) {
            // Connection closed normally
            break;
        } else if (bytes_read < 0) {
#if defined(_WIN32)
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
            if (errno == EWOULDBLOCK || errno == EAGAIN) continue;
#endif
            break;
        }

        raw_response.append(buffer, bytes_read);
    }

    CLOSESOCK(sock);

    if (raw_response.empty()) {
        resp.error_message = "Empty response or timeout";
        return resp;
    }

    // Parse HTTP Status Line
    size_t line_end = raw_response.find("\r\n");
    if (line_end == std::string::npos) {
        resp.error_message = "Malformed HTTP status line";
        return resp;
    }

    std::string status_line = raw_response.substr(0, line_end);
    std::istringstream ss(status_line);
    std::string http_ver;
    int code = 0;
    if (!(ss >> http_ver >> code)) {
        resp.error_message = "Failed to parse HTTP status code";
        return resp;
    }
    resp.status_code = code;

    // Split headers and body
    size_t header_end = raw_response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        resp.body = "";
        return resp;
    }

    std::string_view headers(raw_response.data(), header_end);
    std::string_view raw_body(raw_response.data() + header_end + 4, raw_response.size() - (header_end + 4));

    // Check if Transfer-Encoding: chunked
    std::string lower_headers;
    lower_headers.reserve(headers.size());
    for (char ch : headers) lower_headers.push_back(static_cast<char>(std::tolower(ch)));

    if (lower_headers.find("transfer-encoding: chunked") != std::string::npos) {
        try {
            resp.body = Dechunk(raw_body);
        } catch (...) {
            resp.body.clear();
        }
    } else {
        resp.body = std::string(raw_body);
    }

    // Enforce max body bytes limit
    if (resp.body.size() > max_body_bytes) {
        resp.body.resize(max_body_bytes);
    }

    return resp;
}

} // namespace BareStats
