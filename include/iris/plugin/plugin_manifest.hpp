#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace iris::plugin {

/// T3 进程插件清单（plugin.json / *.plugin.json）。
///
/// 进程插件通过 stdin/stdout 的逐行 JSON 与宿主通信；宿主不加载其代码，
/// 因而崩溃、内存错误或依赖冲突只影响该插件进程。
struct PluginManifest {
    std::string id;      ///< 规范 ID，如 acme.check.cps
    std::string kind;    ///< check / render / textop / parse / asr / tts / mt / report / ...
    std::string version; ///< 插件自身版本（semver 字符串）
    std::string description;
    std::string command;             ///< 可执行文件；相对路径按 directory 解析
    std::vector<std::string> args;   ///< 命令行参数
    std::string cwd;                 ///< 子进程工作目录；空 = 清单所在目录
    std::vector<std::string> permissions; ///< fs:read / fs:write / net / exec / env
    int api_version = 1;             ///< 进程协议版本（当前仅 1）
    int timeout_ms = 10000;          ///< 单次调用超时
    bool enabled = true;
    std::string protocol = "stdio-jsonl";
    nlohmann::json raw = nlohmann::json::object();

    std::filesystem::path manifest_path;
    std::filesystem::path directory;

    /// 从目录（读 plugin.json）或单个 JSON 文件加载。
    static bool load(const std::string& path, PluginManifest& out, std::string& error);

    /// 解析已加载的 JSON；base_directory 用于解析相对 command/cwd。
    static bool parse(const nlohmann::json& document,
                      const std::filesystem::path& base_directory, PluginManifest& out,
                      std::string& error);

    /// 结构、API 版本、权限名与超时校验。
    [[nodiscard]] bool validate(std::string& error) const;

    /// 用于日志/清单展示的名字。
    [[nodiscard]] std::string display_name() const;

    /// 实际传给 CreateProcess/execvp 的命令：相对路径按 directory 解析。
    [[nodiscard]] std::string resolved_command() const;

    /// 子进程实际工作目录（空 cwd 时为 directory）。
    [[nodiscard]] std::string working_directory() const;
};

struct PluginDiscoveryIssue {
    std::string path;
    std::string error;
};

struct PluginDiscoveryResult {
    std::vector<PluginManifest> plugins;
    std::vector<PluginDiscoveryIssue> issues;
};

/// 默认搜索路径（项目优先）：
///   ./.iris/plugins -> ./plugins -> <data_root>/plugins -> <LOCALAPPDATA>/iris/plugins
std::vector<std::string> default_plugin_dirs();

/// 扫描目录并加载所有 plugin.json / *.plugin.json；同 ID 先出现者优先。
PluginDiscoveryResult discover_plugins(const std::vector<std::string>& extra_dirs = {});

} // namespace iris::plugin
