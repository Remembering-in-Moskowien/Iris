#include "iris/server/http_server.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "iris/infra/log.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace iris::server {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

void close_socket(socket_t s) {
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/// 不抛异常的 send-all。
bool send_all(socket_t sock, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = ::send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

const char* reason_for(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Internal Server Error";
    }
}

} // namespace

HttpResponse HttpResponse::json(int status, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.body = body;
    return r;
}

HttpResponse HttpResponse::text(int status, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.content_type = "text/plain; charset=utf-8";
    r.body = body;
    return r;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
            out.push_back(s[i]);
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto amp = query.find('&', start);
        const std::string pair =
            query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!pair.empty()) {
            const auto eq = pair.find('=');
            if (eq == std::string::npos) {
                out[url_decode(pair)] = "";
            } else {
                out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

bool parse_request_head(const std::string& head, HttpRequest& req, std::string& error) {
    std::istringstream in(head);
    std::string line;
    if (!std::getline(in, line)) {
        error = "空请求";
        return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::istringstream first(line);
    std::string target;
    std::string version;
    if (!(first >> req.method >> target >> version)) {
        error = "请求行格式错误";
        return false;
    }
    if (req.method.empty()) {
        error = "缺少方法";
        return false;
    }

    const auto qmark = target.find('?');
    if (qmark == std::string::npos) {
        req.path = url_decode(target);
    } else {
        req.path = url_decode(target.substr(0, qmark));
        req.query = parse_query(target.substr(qmark + 1));
    }

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return true;
}

std::string path_param(const std::string& pattern, const std::string& path) {
    const auto open = pattern.find('{');
    if (open == std::string::npos) return {};
    const auto close = pattern.find('}', open);
    if (close == std::string::npos) return {};
    const std::string prefix = pattern.substr(0, open);
    const std::string suffix = pattern.substr(close + 1);
    if (path.size() <= prefix.size() + suffix.size()) return {};
    if (path.compare(0, prefix.size(), prefix) != 0) return {};
    if (!suffix.empty() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {};
    }
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& path, Handler handler) {
    routes_.push_back({method, path, std::move(handler)});
}

HttpResponse HttpServer::dispatch(const HttpRequest& req) {
    for (const auto& r : routes_) {
        if (r.method != req.method) continue;
        if (r.path == req.path || !path_param(r.path, req.path).empty()) return r.handler(req);
    }
    return HttpResponse::json(404, R"({"error":"not found"})");
}

bool HttpServer::listen(const std::string& host, int port, std::string& error) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup 失败";
        return false;
    }
#endif
    const socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kInvalidSocket) {
        error = "无法创建 socket";
        return false;
    }
#ifdef _WIN32
    BOOL yes = TRUE;
#else
    int yes = 1;
#endif
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close_socket(sock);
        error = "无法解析监听地址：" + host;
        return false;
    }
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close_socket(sock);
        error = "端口被占用或无权绑定：" + std::to_string(port);
        return false;
    }
    if (::listen(sock, 64) != 0) {
        close_socket(sock);
        error = "listen 失败";
        return false;
    }

    listen_socket_ = static_cast<std::uintptr_t>(sock);
    running_ = true;
    stop_requested_ = false;
    spdlog::info("serve: 监听 http://{}:{}/", host.empty() ? "0.0.0.0" : host, port);

    while (!stop_requested_) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_len = sizeof peer;
#else
        socklen_t peer_len = sizeof peer;
#endif
        const socket_t client = ::accept(sock, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (client == kInvalidSocket) {
            if (stop_requested_) break;
            continue;
        }
        handle_connection(static_cast<std::uintptr_t>(client));
    }

    running_ = false;
    close_socket(sock);
    listen_socket_ = 0;
#ifdef _WIN32
    WSACleanup();
#endif
    return true;
}

void HttpServer::stop() {
    if (!running_ || stop_requested_) return;
    stop_requested_ = true;
    // 用 shutdown 让阻塞中的 accept 立刻返回；socket 本身由 listen() 结尾关闭。
    if (listen_socket_ != 0) {
        ::shutdown(static_cast<socket_t>(listen_socket_), 2);
    }
}

void HttpServer::handle_connection(std::uintptr_t socket_handle) {
    const auto sock = static_cast<socket_t>(socket_handle);
    std::string buffer;
    char chunk[8192];

    std::size_t head_end = std::string::npos;
    while (head_end == std::string::npos && buffer.size() <= 64 * 1024) {
        const auto n = ::recv(sock, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<std::size_t>(n));
        head_end = buffer.find("\r\n\r\n");
    }

    HttpRequest req;
    std::string error;
    if (head_end == std::string::npos || !parse_request_head(buffer.substr(0, head_end), req, error)) {
        const auto resp = HttpResponse::json(400, R"({"error":"bad request"})");
        std::string out = "HTTP/1.1 400 Bad Request\r\nContent-Type: " + resp.content_type +
                          "\r\nContent-Length: " + std::to_string(resp.body.size()) +
                          "\r\nConnection: close\r\n\r\n" + resp.body;
        send_all(sock, out);
        close_socket(sock);
        return;
    }

    std::size_t want = 0;
    if (const auto it = req.headers.find("content-length"); it != req.headers.end()) {
        want = static_cast<std::size_t>(std::max(0, std::atoi(it->second.c_str())));
    }
    if (want > 8 * 1024 * 1024) {
        const auto resp = HttpResponse::json(413, R"({"error":"payload too large"})");
        std::string out = "HTTP/1.1 413 Payload Too Large\r\nContent-Type: " + resp.content_type +
                          "\r\nContent-Length: " + std::to_string(resp.body.size()) +
                          "\r\nConnection: close\r\n\r\n" + resp.body;
        send_all(sock, out);
        close_socket(sock);
        return;
    }
    req.body = buffer.substr(head_end + 4);
    while (req.body.size() < want) {
        const auto n = ::recv(sock, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        req.body.append(chunk, static_cast<std::size_t>(n));
    }
    if (req.body.size() > want) req.body.resize(want);

    const auto resp = dispatch(req);
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + reason_for(resp.status) + "\r\n";
    out += "Content-Type: " + resp.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += resp.body;
    send_all(sock, out);
    close_socket(sock);
}

} // namespace iris::server
