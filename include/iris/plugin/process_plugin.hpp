#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "iris/plugin/plugin_manifest.hpp"

namespace iris::plugin {

enum class PluginStatus { Stopped, Running, Degraded };

/// T3 进程插件宿主。
///
/// 协议：一行一个 JSON 对象，UTF-8，stdout 只写协议消息，stderr 留日志。
///   -> {"id":1,"method":"describe","params":{}}
///   -> {"id":2,"method":"invoke","params":{"kind":"check","input":{...},"params":{...}}}
///   <- {"id":1,"result":{...}}
///   <- {"id":2,"error":{"code":"...","message":"..."}}
///   <- {"method":"log","params":{"level":"info","message":"..."}}   (通知)
///
/// 调用是同步的；进程崩溃或超时后会标记 Degraded，下一次调用自动重启。
class ProcessPlugin {
public:
    explicit ProcessPlugin(PluginManifest manifest);
    ~ProcessPlugin();

    ProcessPlugin(const ProcessPlugin&) = delete;
    ProcessPlugin& operator=(const ProcessPlugin&) = delete;

    /// 启动进程并完成 describe 握手。
    bool start(std::string& error);
    bool restart(std::string& error);
    void stop();

    bool describe(nlohmann::json& result, std::string& error);
    bool invoke(const nlohmann::json& input, const nlohmann::json& params,
                nlohmann::json& result, std::string& error);

    /// 通用方法调用；timeout_ms <= 0 时使用清单里的 timeoutMs。
    bool call(const std::string& method, const nlohmann::json& params,
              nlohmann::json& result, std::string& error, int timeout_ms = 0);

    [[nodiscard]] PluginStatus status() const;
    [[nodiscard]] const PluginManifest& manifest() const;
    [[nodiscard]] int restarts() const;
    [[nodiscard]] bool degraded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace iris::plugin
