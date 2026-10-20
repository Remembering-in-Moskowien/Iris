#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace iris::model {

/// 全局配置（iris.config.json，对应 C# centurion.config.json）。
struct AppConfig {
    std::string download_proxy; ///< 工具/模型下载代理（对应 CENTURION_DOWNLOAD_PROXY）
    std::string tools_dir = "tools";
    std::string models_dir = "models";
    std::string log_level = "info";

    [[nodiscard]] nlohmann::json to_json() const;
    static AppConfig from_json(const nlohmann::json& j);
    /// 读取配置；不存在或损坏时返回默认配置（不抛异常）。
    static AppConfig load_or_default(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace iris::model
