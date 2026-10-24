#include "iris/controller/commands.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "iris/asr/asr.hpp"
#include "iris/core/pipeline.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/ui/console.hpp"
#include <nlohmann/json.hpp>
#include "iris/model/ir_document.hpp"
#include "iris/ops/analyze_emotion.hpp"
#include "iris/ops/asr_pipeline.hpp"
#include "iris/ops/dub_tts.hpp"
#include "iris/tts/tts.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

/// 输入是否为 IR（*.json）——是则跳过转写，直接对已有句子配音。
bool is_ir_path(const std::string& path) {
    return fs::path(path).extension() == ".json";
}

/// 解析 "SPEAKER=value" 形式的重复选项（值里可以带 = 与空格）。
std::vector<std::pair<std::string, std::string>> parse_pairs(const std::vector<std::string>& items) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& item : items) {
        const auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0) {
            spdlog::warn("dub: 忽略无法解析的音色参数 '{}'（格式 SPEAKER=值）", item);
            continue;
        }
        out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
    }
    return out;
}

/// 读取 --voices 的 JSON：{"default": {...}, "speakers": {"SPEAKER_1": {...}}}。
bool load_voices_json(const std::string& path, std::vector<ops::SpeakerVoice>& out,
                      std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "无法打开音色配置：" + path;
        return false;
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        error = std::string("音色配置解析失败：") + e.what();
        return false;
    }
    const auto read_one = [](const nlohmann::json& node, const std::string& speaker) {
        ops::SpeakerVoice v;
        v.speaker = speaker;
        v.ref_wav = node.value("ref", node.value("refWav", std::string()));
        v.ref_text = node.value("ref_text", node.value("refText", std::string()));
        v.language = node.value("language", std::string());
        return v;
    };
    if (doc.contains("default") && doc["default"].is_object()) {
        out.push_back(read_one(doc["default"], "default"));
    }
    if (doc.contains("speakers") && doc["speakers"].is_object()) {
        for (const auto& [speaker, node] : doc["speakers"].items()) {
            if (node.is_object()) out.push_back(read_one(node, speaker));
        }
    }
    if (out.empty()) {
        error = "音色配置里没有 default / speakers 条目：" + path;
        return false;
    }
    return true;
}

} // namespace

int run_dub(const DubOptions& opts) {
    if (!fs::exists(opts.input)) {
        std::cerr << _("dub: input not found: ", "dub: 输入文件不存在：") << opts.input << '\n';
        return 1;
    }

    tts::TtsEngine engine{};
    if (!tts::parse_tts_engine(opts.engine, engine)) {
        std::cerr << _("dub: unknown TTS engine '", "dub: 未知 TTS 引擎 '") << opts.engine
                  << _("' (supported: qwen3-tts | indextts)\n",
                       "'（支持: qwen3-tts | indextts）\n");
        return 1;
    }
    if (!tts::tts_engine_available(engine)) {
        std::cerr << _("dub: engine '", "dub: 引擎 '") << opts.engine
                  << _("' runtime is not available in this build (see `iris doctor`)\n",
                       "' 的推理运行时未随本次构建提供（用 `iris doctor` 查看）\n");
        return 1;
    }

    const bool from_ir = is_ir_path(opts.input);

    // 工作目录
    const fs::path work = fs::temp_directory_path() / "iris-dub";
    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        std::cerr << _("dub: cannot create work dir: ", "dub: 无法创建工作目录：") << ec.message()
                  << '\n';
        return 1;
    }

    model::IrDocument doc;
    core::Pipeline pipeline;

    if (from_ir) {
        doc = model::IrDocument::load(opts.input);
        spdlog::info("dub: 复用 IR {}", opts.input);
        if (!opts.emotion_model.empty()) {
            spdlog::warn("dub: IR 输入没有音频产物，跳过情感分析");
        }
    } else {
        asr::AsrBackend backend{};
        if (!asr::parse_asr_backend(opts.asr_engine, backend)) {
            std::cerr << _("dub: unknown ASR engine '", "dub: 未知 ASR 引擎 '") << opts.asr_engine
                      << _("' (supported: whisper | qwen3-asr | whisper-ct2)\n",
                           "'（支持: whisper | qwen3-asr | whisper-ct2）\n");
            return 1;
        }
        if (opts.asr_model.empty() || !fs::exists(opts.asr_model)) {
            std::cerr << _("dub: --asr-model is required for media input (path: ",
                           "dub: 媒体输入需要 --asr-model 指定 ASR 模型（路径：")
                      << opts.asr_model << ")\n";
            return 1;
        }
        if (!asr::asr_backend_available(backend)) {
            std::cerr << _("dub: ASR runtime for '", "dub: ASR 引擎 '") << opts.asr_engine
                      << _("' is not available in this build\n", "' 的运行时未随本次构建提供\n");
            return 1;
        }

        doc.generator.version = kVersion;
        doc.generator.command = "dub";
        doc.generator.generated_at = "now";
        doc.generator.input_file = opts.input;
        doc.config.input_file_path = opts.input;
        doc.config.command_name = "dub";
        doc.config.language = opts.language;
        doc.config.transcriber_engine = opts.asr_engine;
        doc.config.transcriber_model = opts.asr_model;

        pipeline = ops::make_asr_pipeline(backend, opts.asr_model);
        if (!opts.emotion_model.empty()) {
            pipeline.add(std::make_shared<ops::AnalyzeEmotion>(opts.emotion_model));
        }
    }

    ops::DubTts::Options dub;
    dub.request.engine = engine;
    dub.request.model = opts.model;
    dub.request.codec_model = opts.codec;
    dub.request.ref_wav = opts.ref_wav;
    dub.request.ref_text = opts.ref_text;
    dub.request.language = opts.language;
    dub.request.steps = opts.steps;
    dub.request.n_threads = opts.n_threads;
    dub.request.cfg_scale = opts.cfg_scale;
    dub.output_path = opts.out_wav;
    dub.after_asr = !from_ir;
    dub.fit_timing = opts.fit_timing;
    dub.gap_ms = opts.gap_ms;
    dub.max_speed = opts.max_speed;

    // 多音色：JSON 配置 + 命令行 SPEAKER=ref / SPEAKER=逐字稿
    if (!opts.voices_json.empty()) {
        std::string voice_error;
        if (!load_voices_json(opts.voices_json, dub.voices, voice_error)) {
            std::cerr << "dub: " << voice_error << '\n';
            return 1;
        }
    }
    auto merge_voice = [&](const std::string& speaker, const std::string& value, bool is_text) {
        auto it = std::find_if(dub.voices.begin(), dub.voices.end(),
                               [&](const ops::SpeakerVoice& v) { return v.speaker == speaker; });
        if (it == dub.voices.end()) {
            ops::SpeakerVoice v;
            v.speaker = speaker;
            dub.voices.push_back(v);
            it = std::prev(dub.voices.end());
        }
        if (is_text) {
            it->ref_text = value;
        } else {
            it->ref_wav = value;
        }
    };
    for (const auto& [speaker, path] : parse_pairs(opts.voice_refs)) merge_voice(speaker, path, false);
    for (const auto& [speaker, text] : parse_pairs(opts.voice_texts)) merge_voice(speaker, text, true);
    if (!dub.voices.empty()) {
        spdlog::info("dub: 多音色 {} 个", dub.voices.size());
    }

    pipeline.add(std::make_shared<ops::DubTts>(std::move(dub)));

    core::PipelineContext ctx;
    ctx.doc = &doc;
    ctx.work_dir = work.string();
    auto stages = std::make_shared<ui::StagePrinter>();
    ctx.on_progress = [stages](const std::string& stage, float) { stages->begin(stage); };

    const bool pipeline_ok = pipeline.run(ctx);
    stages->finish();
    if (!pipeline_ok) {
        std::cerr << _("dub: pipeline failed (see log above)\n",
                       "dub: 管线执行失败（见上方日志）\n");
        return 1;
    }

    // IR 落盘：媒体输入默认与 asr 一致写成 <input>.iris.json，也支持 --ir-out 指定。
    const std::string ir_path = !opts.ir_out.empty() ? opts.ir_out
                                                     : (from_ir ? std::string() : opts.input + ".iris.json");
    if (!ir_path.empty()) {
        doc.save(ir_path);
        spdlog::info("dub: IR → {}", ir_path);
    }

    if (!doc.state.dubbed_audio_path) {
        std::cerr << _("dub: no dub track produced\n", "dub: 没有生成配音音轨\n");
        return 1;
    }
    std::cout << _("dub: wrote ", "dub: 写出 ") << *doc.state.dubbed_audio_path << '\n';
    return 0;
}

} // namespace iris::controller
