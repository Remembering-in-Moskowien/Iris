#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace iris::server {

// ---------------------------------------------------------------------------
// 极简 HTTP/1.1 服务（自写，不引第三方库）。
//
// 只做 iris 需要的事：解析请求行/头/Content-Length 正文，按 (method, path)
// 路由到处理函数，回一个 JSON 或文本响应，然后关闭连接。
//
// 并发模型：accept 与请求处理在同一个线程里串行（一次一个请求）；
// `serve` 的耗时工作都是异步子进程，所以服务本身不会被长任务阻塞。
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;                           ///< 不含查询串
    std::map<std::string, std::string> query;   ///< 查询参数（已解码）
    std::map<std::string, std::string> headers; ///< 头名统一小写
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;

    static HttpResponse json(int status, const std::string& body);
    static HttpResponse text(int status, const std::string& body);
};

using Handler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// 注册路由；path 支持 "/jobs/{id}" 形式的单段通配。
    void route(const std::string& method, const std::string& path, Handler handler);

    /// 绑定并开始服务（阻塞，直到 stop()）。
    bool listen(const std::string& host, int port, std::string& error);

    /// 请求停止（可从另一个线程调用）。
    void stop();

    [[nodiscard]] bool running() const { return running_; }

private:
    struct Route {
        std::string method;
        std::string path;
        Handler handler;
    };

    HttpResponse dispatch(const HttpRequest& req);
    void handle_connection(std::uintptr_t socket_handle);

    std::vector<Route> routes_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::uintptr_t listen_socket_ = 0;
};

// ---- 便于单测的纯函数 ------------------------------------------------------

/// 解析请求行与头（不含正文）。失败返回 false。
bool parse_request_head(const std::string& head, HttpRequest& req, std::string& error);

/// 解析查询串 "a=1&b=2"（做百分号解码）。
std::map<std::string, std::string> parse_query(const std::string& query);

/// 百分号解码（%20 → 空格，+ → 空格）。
std::string url_decode(const std::string& s);

/// JSON 字符串转义（不含外层引号）。
std::string json_escape(const std::string& s);

/// 从 "/jobs/abc123" 里按 "/jobs/{id}" 模板取路径参数；不匹配返回空串。
std::string path_param(const std::string& pattern, const std::string& path);

} // namespace iris::server
