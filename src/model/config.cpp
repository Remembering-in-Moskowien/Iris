#include "iris/model/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "iris/infra/log.hpp"
#include "iris/infra/paths.hpp"

namespace iris::model {

namespace fs = std::filesystem;

namespace {

/// 目录是否存在（不抛异常）。
bool dir_exists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(path, ec);
}

std::string read_str(const nlohmann::json& j, const char* key, const std::string& fallback) {
    return j.value(key, fallback);
}

std::string g_config_path;
std::string g_models_root = "models";
AppConfig g_config;

} // namespace

nlohmann::json AppConfig::to_json() const {
    return {{"version", version},
            {"downloadProxy", download_proxy},
            {"hfMirror", hf_mirror},
            {"toolsDir", tools_dir},
            {"modelsDir", models_dir},
            {"logLevel", log_level},
            {"threads", threads},
            {"language", language},
            {"asr",
             {{"engine", asr_engine}, {"model", asr_model}, {"splitModel", asr_split_model}}},
            {"punc", {{"model", punc_model}}},
            {"separation", {{"model", separation_model}}},
            {"diarize", {{"model", diarize_model}}},
            {"ocr", {{"modelDir", ocr_model_dir}}},
            {"sat", {{"modelDir", sat_model_dir}}},
            {"tts",
             {{"model", tts_model},
              {"codec", tts_codec},
              {"refWav", tts_ref_wav},
              {"refText", tts_ref_text}}},
            {"translate", {{"engine", translate_engine}, {"model", translate_model}}}};
}

AppConfig AppConfig::from_json(const nlohmann::json& j) {
    AppConfig c;
    c.version = j.value("version", 1);
    c.download_proxy = read_str(j, "downloadProxy", "");
    c.hf_mirror = read_str(j, "hfMirror", "https://hf-mirror.com");
    c.tools_dir = read_str(j, "toolsDir", "tools");
    c.models_dir = read_str(j, "modelsDir", "");
    c.log_level = read_str(j, "logLevel", "info");
    c.threads = j.value("threads", 0);
    c.language = read_str(j, "language", "zh");
    if (j.contains("asr") && j["asr"].is_object()) {
        const auto& a = j["asr"];
        c.asr_engine = a.value("engine", c.asr_engine);
        c.asr_model = a.value("model", c.asr_model);
        c.asr_split_model = a.value("splitModel", c.asr_split_model);
    }
    if (j.contains("punc") && j["punc"].is_object()) {
        c.punc_model = j["punc"].value("model", c.punc_model);
    }
    if (j.contains("separation") && j["separation"].is_object()) {
        c.separation_model = j["separation"].value("model", c.separation_model);
    }
    if (j.contains("diarize") && j["diarize"].is_object()) {
        c.diarize_model = j["diarize"].value("model", "");
    }
    if (j.contains("ocr") && j["ocr"].is_object()) {
        c.ocr_model_dir = j["ocr"].value("modelDir", c.ocr_model_dir);
    }
    if (j.contains("sat") && j["sat"].is_object()) {
        c.sat_model_dir = j["sat"].value("modelDir", c.sat_model_dir);
    }
    if (j.contains("tts") && j["tts"].is_object()) {
        const auto& t = j["tts"];
        c.tts_model = t.value("model", c.tts_model);
        c.tts_codec = t.value("codec", c.tts_codec);
        c.tts_ref_wav = t.value("refWav", "");
        c.tts_ref_text = t.value("refText", "");
    }
    if (j.contains("translate") && j["translate"].is_object()) {
        const auto& t = j["translate"];
        c.translate_engine = t.value("engine", c.translate_engine);
        c.translate_model = t.value("model", c.translate_model);
    }
    return c;
}

AppConfig AppConfig::load_or_default(const std::string& path) {
    try {
        std::ifstream in(path);
        if (!in) return AppConfig{};
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded()) return AppConfig{};
        return from_json(j);
    } catch (...) {
        return AppConfig{};
    }
}

void AppConfig::save(const std::string& path) const {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << to_json().dump(2) << '\n';
}

std::string AppConfig::resolve(const std::string& value) const {
    if (value.empty()) return value;
    const fs::path p(value);
    if (p.is_absolute()) return p.string();
    const fs::path base = models_dir.empty() ? fs::path("models") : fs::path(models_dir);
    // make_preferred：统一成平台分隔符（配置里通常写 /）
    fs::path rel = p;
    rel.make_preferred();
    return (base / rel).lexically_normal().string();
}

// ---------------------------------------------------------------------------
// 路径解析
// ---------------------------------------------------------------------------
std::string data_root() {
    // 1) 显式指定
    if (const char* env = std::getenv("IRIS_HOME"); env != nullptr && *env != '\0') {
        return fs::path(env).string();
    }
#ifdef _WIN32
    // 2) 跟着安装盘走：D:\...\iris.exe → D:\iris
    const fs::path exe_dir = infra::executable_dir();
    if (!exe_dir.empty()) {
        const std::string drive = exe_dir.root_name().string(); // 例："D:"
        if (!drive.empty()) {
            // 注意：fs::path("D:") / "iris" 是「驱动器相对路径」D:iris，
            // 必须显式补上分隔符才是 D:\iris。
            return drive + std::string(1, fs::path::preferred_separator) + "iris";
        }
    }
    // 兜底：实在拿不到盘符才用 AppData
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        return (fs::path(appdata) / "iris").string();
    }
#else
    // 2) 数据目录约定（不占系统配置空间）
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return (fs::path(xdg) / "iris").string();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return (fs::path(home) / ".local" / "share" / "iris").string();
    }
#endif
    return "iris-data";
}

std::string config_dir() { return data_root(); }

std::string default_config_path() {
    return (fs::path(data_root()) / "config.json").string();
}

std::string default_models_dir() {
    return (fs::path(data_root()) / "models").string();
}

std::string legacy_appdata_config() {
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr && *appdata != '\0') {
        return (fs::path(appdata) / "iris" / "config.json").string();
    }
    return {};
}

std::string resolve_config_path(const std::string& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    if (const char* env = std::getenv("IRIS_CONFIG"); env != nullptr && *env != '\0') {
        return env;
    }
    const std::string root_config = default_config_path();
    std::error_code ec;
    if (fs::exists(root_config, ec)) return root_config;
    // 仓库内旧配置（开发时的习惯位置）
    if (fs::exists("iris.config.json", ec)) return "iris.config.json";

    // 旧版写在 %APPDATA% 的配置：迁移到数据根目录，避免继续把模型指向系统盘
    const std::string legacy = legacy_appdata_config();
    if (!legacy.empty() && fs::exists(legacy, ec)) {
        const auto migrated = AppConfig::load_or_default(legacy);
        AppConfig fresh = migrated;
        const char* appdata_env = std::getenv("APPDATA");
        const bool models_on_system_drive =
            fresh.models_dir.empty() ||
            (appdata_env != nullptr && fresh.models_dir.rfind(appdata_env, 0) == 0);
        if (models_on_system_drive) fresh.models_dir = default_models_dir();
        try {
            fresh.save(root_config);
            spdlog::info("config: 已从 {} 迁移到 {}（模型目录 {}）", legacy, root_config,
                         fresh.models_dir);
        } catch (...) {
            return legacy; // 迁移失败就继续用旧文件
        }
        return root_config;
    }
    return root_config;
}

const std::string& active_config_path() { return g_config_path; }

const std::string& models_root() { return g_models_root; }

void set_models_root(const std::string& root) {
    if (!root.empty()) g_models_root = root;
}

const AppConfig& active_config() { return g_config; }

AppConfig load_active_config(const std::string& explicit_path) {
    g_config_path = resolve_config_path(explicit_path);
    std::error_code ec;
    const bool existed = fs::exists(g_config_path, ec);
    g_config = AppConfig::load_or_default(g_config_path);

    if (!existed || g_config.models_dir.empty()) {
        if (g_config.models_dir.empty()) {
            // 仓库内已有 models/ 时继续用它，避免打断现有工作流；否则用用户目录。
            g_config.models_dir = dir_exists("models") ? fs::absolute("models").string()
                                                       : default_models_dir();
        }
        fs::create_directories(g_config.models_dir, ec);
        if (!existed) {
            try {
                g_config.save(g_config_path);
                spdlog::info("config: 已创建 {}（模型目录 {}）", g_config_path, g_config.models_dir);
            } catch (...) {
                // 写不了配置也不致命：继续用内存里的默认值
            }
        }
    } else {
        fs::create_directories(g_config.models_dir, ec);
    }

    set_models_root(g_config.models_dir);
    return g_config;
}

} // namespace iris::model
