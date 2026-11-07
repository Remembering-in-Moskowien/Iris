#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/subtitle/ass_script.hpp"

namespace iris::model {

// ---------------------------------------------------------------------------
// IR 中间文件模型（*.iris.json）。
// IR 字段契约（schemaVersion "1.0"）：
//   schema_version + generator + provenance[] + config + state
// ---------------------------------------------------------------------------

/// 工具生成信息。
struct GeneratorInfo {
    std::string tool = "iris";
    std::string version;
    std::optional<std::string> command;
    std::string generated_at; ///< ISO 8601
    std::optional<std::string> input_file;
    std::optional<std::string> output_file;

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static GeneratorInfo from_json(const nlohmann::json& j);
};

/// 单条溯源记录（对应 C# ProvenanceEntry）。
struct ProvenanceEntry {
    std::string op; ///< 操作符名，如 "transcribe"
    std::optional<std::string> model;
    std::optional<std::string> parameters_hash; ///< 配置指纹 SHA-256 hex
    std::optional<std::string> applied_at;      ///< ISO 8601

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static ProvenanceEntry from_json(const nlohmann::json& j);
};

/// 词级数据（对应 C# Word）。
struct Word {
    std::string text;
    double start = 0.0; ///< 秒
    double end = 0.0;
    double score = 1.0;
    std::optional<std::string> speaker;

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static Word from_json(const nlohmann::json& j);
};

/// 句子（对应 C# Sentence）。
struct Sentence {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text;
    std::optional<std::string> translated_text;
    std::optional<std::string> speaker;
    std::vector<Word> words;
    // ---- 成品信息（IR 1.0 的加法扩展；旧文件缺省为空） ----
    std::optional<std::string> style; ///< ASS 样式名（如 chi / eng-Twilight）
    std::optional<std::string> raw;   ///< 原始 ASS 文本（含 {\K..} 等覆盖标签）
    std::optional<std::string> translated_raw; ///< 译文行的 ASS 文本（含 \K）
    std::optional<std::string> role;  ///< dialogue | sdh | lyric | interjection | annotation
    int layer = 0;                    ///< ASS 层
    bool comment = false;             ///< Comment 行

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static Sentence from_json(const nlohmann::json& j);
};

/// 轨道：同一语言/类型下的句子集合（对应 C# IRTrack）。
struct Track {
    std::string type; ///< "transcribe" | "translation" | "script" | ...
    std::string language;
    std::string kind = "dialogue"; ///< dialogue | lyric
    std::vector<Sentence> sentences;

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static Track from_json(const nlohmann::json& j);
};

/// 工作流配置（对应 C# WorkflowConfig 常用字段）。
struct WorkflowConfig {
    std::string input_file_path;
    std::string command_name;
    std::optional<std::string> subtitle_file_path;
  std::optional<std::string> script_file_path;   ///< from-script 使用的剧本文本
    std::optional<std::string> output_file_path;
    std::string device = "auto"; ///< auto|cpu|cuda|vulkan|directml
    std::string transcriber_engine = "whisper";
    std::optional<std::string> transcriber_model;
    std::string language = "en";
    std::string split_strategy = "rule"; ///< rule|llm|sat
    int max_sentence_length = 80;
    int max_chars_per_line = 18;
    double max_cps = 5.0;

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static WorkflowConfig from_json(const nlohmann::json& j);
};

/// 工作流状态（对应 C# WorkflowState 常用字段；运行期产物不持久化）。
struct WorkflowState {
    std::optional<std::string> pipeline_temp_directory;
    std::optional<std::string> converted_audio_path;
    std::optional<std::string> preprocessed_audio_path;
    std::optional<std::string> vocals_path;
    std::optional<std::string> dubbed_audio_path; ///< dub 链路的配音音轨（WAV）
    std::vector<Track> tracks;

    // ---- 成品信息（IR 1.0 的加法扩展） ----
    std::vector<subtitle::AssStyle> styles;                          ///< 样式表
    std::vector<std::pair<std::string, std::string>> script_info;    ///< [Script Info] 顺序键值
    std::int64_t timeline_offset_ms = 0;                             ///< 整片偏移（渲染时叠加）

    bool is_audio_converted = false;
    bool is_vocals_separated = false;
    bool is_transcribed = false;
    bool is_dubbed = false;

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static WorkflowState from_json(const nlohmann::json& j);
};

/// IR 文档根对象。
struct IrDocument {
    std::string schema_version = "1.0";
    GeneratorInfo generator;
    std::vector<ProvenanceEntry> provenance;
    WorkflowConfig config;
    WorkflowState state;

    /// 序列化为 nlohmann::json。
    [[nodiscard]] nlohmann::json to_json() const;
    static IrDocument from_json(const nlohmann::json& j);

    [[nodiscard]] std::string dump(int indent = 2) const;
    static IrDocument load(const std::string& path);
    void save(const std::string& path, int indent = 2) const;
};

/// 文档校验结果。
struct ValidationResult {
    bool is_valid = false;
    std::string version;
    std::vector<std::string> issues;
};

/// 校验文档结构（版本支持、必填结构）。
[[nodiscard]] ValidationResult validate_document(const IrDocument& doc);
/// 校验磁盘上的 IR 文件。
[[nodiscard]] ValidationResult validate_file(const std::string& path);

} // namespace iris::model
