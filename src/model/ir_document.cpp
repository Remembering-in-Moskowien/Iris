#include "daisy/model/ir_document.hpp"

#include <ctime>
#include <fstream>
#include <stdexcept>

namespace daisy::model {

namespace {

constexpr const char* kToolName = "daisy";
constexpr const char* kCurrentVersion = "1.0";

std::string now_iso8601() {
    // 简化实现：本地时间的 ISO 8601。生产实现可用 std::chrono::zoned_time / fmt。
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return buf;
}

template <typename T>
void set_opt(const nlohmann::json& j, const char* key, T& out) {
    if (j.contains(key) && !j[key].is_null()) {
        out = j[key].get<T>();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// JSON 序列化（字段名与 C# System.Text.Json 源生成器契约保持一致）
// ---------------------------------------------------------------------------
nlohmann::json GeneratorInfo::to_json() const {
    nlohmann::json j = {{"tool", tool}, {"version", version}};
    if (command) j["command"] = *command;
    j["generatedAt"] = generated_at;
    if (input_file) j["inputFile"] = *input_file;
    if (output_file) j["outputFile"] = *output_file;
    return j;
}

GeneratorInfo GeneratorInfo::from_json(const nlohmann::json& j) {
    GeneratorInfo g;
    g.tool = j.value("tool", std::string(kToolName));
    g.version = j.value("version", std::string());
    set_opt(j, "command", g.command);
    g.generated_at = j.value("generatedAt", now_iso8601());
    set_opt(j, "inputFile", g.input_file);
    set_opt(j, "outputFile", g.output_file);
    return g;
}

nlohmann::json ProvenanceEntry::to_json() const {
    nlohmann::json j = {{"operator", op}};
    if (model) j["model"] = *model;
    if (parameters_hash) j["parametersHash"] = *parameters_hash;
    if (applied_at) j["appliedAt"] = *applied_at;
    return j;
}

ProvenanceEntry ProvenanceEntry::from_json(const nlohmann::json& j) {
    ProvenanceEntry p;
    p.op = j.value("operator", std::string());
    set_opt(j, "model", p.model);
    set_opt(j, "parametersHash", p.parameters_hash);
    set_opt(j, "appliedAt", p.applied_at);
    return p;
}

nlohmann::json WorkflowConfig::to_json() const {
    nlohmann::json j = {{"inputFilePath", input_file_path},
                        {"commandName", command_name},
                        {"device", device},
                        {"transcriberEngine", transcriber_engine},
                        {"language", language},
                        {"splitStrategy", split_strategy},
                        {"maxSentenceLength", max_sentence_length},
                        {"maxCharsPerLine", max_chars_per_line},
                        {"maxCps", max_cps}};
    if (subtitle_file_path) j["subtitleFilePath"] = *subtitle_file_path;
    if (output_file_path) j["outputFilePath"] = *output_file_path;
    if (transcriber_model) j["transcriberModel"] = *transcriber_model;
    return j;
}

WorkflowConfig WorkflowConfig::from_json(const nlohmann::json& j) {
    WorkflowConfig c;
    c.input_file_path = j.value("inputFilePath", std::string());
    c.command_name = j.value("commandName", std::string());
    set_opt(j, "subtitleFilePath", c.subtitle_file_path);
    set_opt(j, "outputFilePath", c.output_file_path);
    c.device = j.value("device", std::string("auto"));
    c.transcriber_engine = j.value("transcriberEngine", std::string("whisper"));
    set_opt(j, "transcriberModel", c.transcriber_model);
    c.language = j.value("language", std::string("en"));
    c.split_strategy = j.value("splitStrategy", std::string("rule"));
    c.max_sentence_length = j.value("maxSentenceLength", 80);
    c.max_chars_per_line = j.value("maxCharsPerLine", 18);
    c.max_cps = j.value("maxCps", 5.0);
    return c;
}

nlohmann::json Word::to_json() const {
    nlohmann::json j = {{"text", text}, {"start", start}, {"end", end}, {"score", score}};
    if (speaker) j["speaker"] = *speaker;
    return j;
}

Word Word::from_json(const nlohmann::json& j) {
    Word w;
    w.text = j.value("text", std::string());
    w.start = j.value("start", 0.0);
    w.end = j.value("end", 0.0);
    w.score = j.value("score", 1.0);
    set_opt(j, "speaker", w.speaker);
    return w;
}

nlohmann::json Sentence::to_json() const {
    nlohmann::json j = {{"startMs", start_ms}, {"endMs", end_ms}, {"text", text}};
    if (translated_text) j["translatedText"] = *translated_text;
    if (speaker) j["speaker"] = *speaker;
    if (!words.empty()) {
        j["words"] = nlohmann::json::array();
        for (const auto& w : words) j["words"].push_back(w.to_json());
    }
    return j;
}

Sentence Sentence::from_json(const nlohmann::json& j) {
    Sentence s;
    s.start_ms = j.value("startMs", std::int64_t{0});
    s.end_ms = j.value("endMs", std::int64_t{0});
    s.text = j.value("text", std::string());
    set_opt(j, "translatedText", s.translated_text);
    set_opt(j, "speaker", s.speaker);
    if (j.contains("words") && j["words"].is_array()) {
        for (const auto& w : j["words"]) s.words.push_back(Word::from_json(w));
    }
    return s;
}

nlohmann::json Track::to_json() const {
    nlohmann::json j = {{"type", type}, {"language", language}};
    j["sentences"] = nlohmann::json::array();
    for (const auto& s : sentences) j["sentences"].push_back(s.to_json());
    return j;
}

Track Track::from_json(const nlohmann::json& j) {
    Track t;
    t.type = j.value("type", std::string());
    t.language = j.value("language", std::string());
    if (j.contains("sentences") && j["sentences"].is_array()) {
        for (const auto& s : j["sentences"]) t.sentences.push_back(Sentence::from_json(s));
    }
    return t;
}

nlohmann::json WorkflowState::to_json() const {
    nlohmann::json j = {{"isAudioConverted", is_audio_converted},
                        {"isVocalsSeparated", is_vocals_separated},
                        {"isTranscribed", is_transcribed}};
    if (pipeline_temp_directory) j["pipelineTempDirectory"] = *pipeline_temp_directory;
    if (converted_audio_path) j["convertedAudioPath"] = *converted_audio_path;
    if (preprocessed_audio_path) j["preprocessedAudioPath"] = *preprocessed_audio_path;
    if (vocals_path) j["vocalsPath"] = *vocals_path;
    if (!tracks.empty()) {
        j["tracks"] = nlohmann::json::array();
        for (const auto& t : tracks) j["tracks"].push_back(t.to_json());
    }
    return j;
}

WorkflowState WorkflowState::from_json(const nlohmann::json& j) {
    WorkflowState st;
    set_opt(j, "pipelineTempDirectory", st.pipeline_temp_directory);
    set_opt(j, "convertedAudioPath", st.converted_audio_path);
    set_opt(j, "preprocessedAudioPath", st.preprocessed_audio_path);
    set_opt(j, "vocalsPath", st.vocals_path);
    if (j.contains("tracks") && j["tracks"].is_array()) {
        for (const auto& t : j["tracks"]) st.tracks.push_back(Track::from_json(t));
    }
    st.is_audio_converted = j.value("isAudioConverted", false);
    st.is_vocals_separated = j.value("isVocalsSeparated", false);
    st.is_transcribed = j.value("isTranscribed", false);
    return st;
}

// ---------------------------------------------------------------------------
// IrDocument
// ---------------------------------------------------------------------------
nlohmann::json IrDocument::to_json() const {
    nlohmann::json j = {{"schemaVersion", schema_version},
                        {"generator", generator.to_json()}};
    j["provenance"] = nlohmann::json::array();
    for (const auto& p : provenance) j["provenance"].push_back(p.to_json());
    j["config"] = config.to_json();
    j["state"] = state.to_json();
    return j;
}

IrDocument IrDocument::from_json(const nlohmann::json& j) {
    IrDocument d;
    d.schema_version = j.value("schemaVersion", std::string(kCurrentVersion));
    if (j.contains("generator")) d.generator = GeneratorInfo::from_json(j["generator"]);
    if (j.contains("provenance") && j["provenance"].is_array()) {
        for (const auto& p : j["provenance"]) d.provenance.push_back(ProvenanceEntry::from_json(p));
    }
    if (j.contains("config")) d.config = WorkflowConfig::from_json(j["config"]);
    if (j.contains("state")) d.state = WorkflowState::from_json(j["state"]);
    return d;
}

std::string IrDocument::dump(int indent) const { return to_json().dump(indent); }

IrDocument IrDocument::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开 IR 文件: " + path);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error("IR 文件不是合法 JSON: " + path);
    return from_json(j);
}

void IrDocument::save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("无法写入 IR 文件: " + path);
    out << dump(2) << '\n';
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------
ValidationResult validate_document(const IrDocument& doc) {
    ValidationResult r;
    r.version = doc.schema_version;
    if (doc.schema_version != kCurrentVersion) {
        r.issues.push_back("不支持的 schemaVersion: " + doc.schema_version);
    }
    if (doc.generator.tool.empty()) {
        r.issues.push_back("generator.tool 缺失");
    }
    if (doc.config.input_file_path.empty() && doc.state.tracks.empty()) {
        r.issues.push_back("既无输入文件也无已处理轨道");
    }
    r.is_valid = r.issues.empty();
    return r;
}

ValidationResult validate_file(const std::string& path) {
    try {
        const auto doc = IrDocument::load(path);
        return validate_document(doc);
    } catch (const std::exception& e) {
        ValidationResult r;
        r.is_valid = false;
        r.issues.push_back(e.what());
        return r;
    }
}

} // namespace daisy::model
