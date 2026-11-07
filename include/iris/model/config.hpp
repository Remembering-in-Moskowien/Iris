#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace iris::model {

// ---------------------------------------------------------------------------
// 全局配置。
//
// 落盘位置（按优先级）：
//   1. --config <path>
//   2. 环境变量 IRIS_CONFIG
//   3. %APPDATA%\iris\config.json          （Windows）
//      $XDG_CONFIG_HOME/iris/config.json   （Linux，缺省 ~/.config/iris/config.json）
//   4. 仓库内的 ./iris.config.json          （兼容旧用法）
//
// 模型路径一律相对 modelsDir 解析（绝对路径原样使用）；首次运行会在用户目录
// 生成配置并创建 models 目录，之后 `iris models install` 会把模型装到那里。
// ---------------------------------------------------------------------------

struct AppConfig {
    int version = 1;
    std::string download_proxy;                      ///< 下载代理
    std::string hf_mirror = "https://hf-mirror.com"; ///< HuggingFace 镜像（空 = 不重写）
    std::string tools_dir = "tools";
    std::string models_dir;                          ///< 空 = 首次运行自动决定
    std::string log_level = "info";
    int threads = 0;                                 ///< 0 = 自动（min(8, 核数)）
    std::string language = "zh";

    // 各类模型的默认路径（相对 modelsDir）
    std::string asr_engine = "whisper";
    std::string asr_model = "ggml/ggml-base.bin";
    std::string asr_split_model = "sat";
    std::string punc_model = "punc/fireredpunc-q4_k.gguf";
    std::string separation_model = "separation/htdemucs-q8_0.gguf";
    std::string diarize_model; ///< 空 = 自动在 ~/.cache/crispasr 里找
    std::string ocr_model_dir = "ocr";
    std::string sat_model_dir = "sat";

    std::string tts_model = "qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf";
    std::string tts_codec = "qwen3-tts/qwen3-tts-tokenizer-12hz.gguf";
    std::string tts_ref_wav;
    std::string tts_ref_text;

    std::string translate_engine = "auto";
    std::string translate_model = "translation/m2m100-418m-q8_0.gguf";

    [[nodiscard]] nlohmann::json to_json() const;
    static AppConfig from_json(const nlohmann::json& j);

    /// 读取配置；文件不存在/损坏时返回默认值（不抛异常）。
    static AppConfig load_or_default(const std::string& path);
    void save(const std::string& path) const;

    /// 解析模型路径：相对路径拼到 modelsDir 下，绝对路径原样返回。
    [[nodiscard]] std::string resolve(const std::string& value) const;
};

/// 数据根目录：
///   IRIS_HOME 环境变量（显式指定）
///   > Windows：可执行文件所在盘的根目录 + "iris"（如 D:\iris）——避免占用 C 盘
///   > POSIX：$XDG_DATA_HOME/iris 或 ~/.local/share/iris
/// 安装在哪块盘，数据就落在哪块盘，不会挤占系统盘空间。
std::string data_root();

/// 配置目录（= data_root()）。
std::string config_dir();

/// 默认配置文件路径（data_root()/config.json）。
std::string default_config_path();

/// 默认模型目录（data_root()/models）。
std::string default_models_dir();

/// 解析实际使用的配置文件：显式参数 > IRIS_CONFIG > 用户目录 > 仓库内旧文件。
std::string resolve_config_path(const std::string& explicit_path);

/// 读取（首次运行会创建）配置，并设置全局 models root。
AppConfig load_active_config(const std::string& explicit_path = {});

/// 上一次 load_active_config 使用的配置文件路径。
const std::string& active_config_path();

/// 全局模型根目录（供各处的「自动发现模型」使用）。
const std::string& models_root();
void set_models_root(const std::string& root);

/// 当前生效的配置。
const AppConfig& active_config();

} // namespace iris::model
