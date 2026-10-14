#include "daisy/providers/asr.hpp"

#include <cmath>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "daisy/infra/log.hpp"
#include "daisy/infra/process.hpp"

namespace daisy::providers {

bool WhisperCppAsr::transcribe(const std::string& audio_path, const std::string& language,
                               std::vector<model::Sentence>& out, std::string& error) {
    // whisper-cli -m <model> -l <lang> -f <audio> -oj -of <prefix>
    const std::string prefix = audio_path + ".whisper";
    const auto result = infra::run_process(
        {cli_path_, "-m", model_path_, "-l", language, "-f", audio_path, "-oj", "-of", prefix});
    if (!result.ok()) {
        error = result.error();
        return false;
    }
    if (!result.value().success()) {
        error = "whisper-cli 退出码 " + std::to_string(result.value().exit_code) + ": " +
                result.value().stdout_text;
        return false;
    }

    // 解析 prefix.json（whisper.cpp -oj 输出：根含 segments 数组；部分版本根即数组）。
    try {
        std::ifstream in(prefix + ".json");
        if (!in) {
            error = "找不到 whisper 输出文件 " + prefix + ".json";
            return false;
        }
        const nlohmann::json j = nlohmann::json::parse(in);
        const nlohmann::json* segs = nullptr;
        if (j.contains("segments") && j["segments"].is_array()) {
            segs = &j["segments"];
        } else if (j.is_array()) {
            segs = &j;
        }
        if (!segs) {
            error = "无法识别的 whisper JSON 输出结构";
            return false;
        }

        out.clear();
        out.reserve(segs->size());
        for (const auto& seg : *segs) {
            model::Sentence s;
            s.start_ms = static_cast<std::int64_t>(seg.value("start", 0.0) * 1000.0 + 0.5);
            s.end_ms = static_cast<std::int64_t>(seg.value("end", 0.0) * 1000.0 + 0.5);
            s.text = seg.value("text", std::string());
            if (s.text.empty()) continue;
            out.push_back(std::move(s));
        }
        spdlog::info("whisper.cpp: {} 段", out.size());
        return true;
    } catch (const std::exception& e) {
        error = std::string("解析 whisper 输出失败: ") + e.what();
        return false;
    }
}

} // namespace daisy::providers
