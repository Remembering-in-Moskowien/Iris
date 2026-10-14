#include "daisy/cli/cli.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <CLI/CLI.hpp>

#include "daisy/core/pipeline.hpp"
#include "daisy/core/provider.hpp"
#include "daisy/infra/log.hpp"
#include "daisy/infra/process.hpp"
#include "daisy/model/config.hpp"
#include "daisy/model/ir_document.hpp"
#include "daisy/model/subtitle.hpp"
#include "daisy/ops/audio_preprocess.hpp"
#include "daisy/ops/ass_build.hpp"
#include "daisy/ops/ffmpeg_convert.hpp"
#include "daisy/ops/sentence_split.hpp"
#include "daisy/ops/transcribe.hpp"
#include "daisy/providers/asr.hpp"

namespace daisy::cli {

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.1.0";

// ---------------------------------------------------------------------------
// asr：媒体 → IR（ffmpeg_convert → audio_preprocess → transcribe → sentence_split）
// ---------------------------------------------------------------------------
int cmd_asr(const std::string& input, const std::string& language,
            const std::string& engine, const std::string& model, const std::string& output) {
    if (!fs::exists(input)) {
        spdlog::error("asr: 输入文件不存在: {}", input);
        return 1;
    }
    if (engine != "whisper") {
        spdlog::error("asr: 暂不支持的引擎 '{}'（当前支持 whisper）", engine);
        return 1;
    }
    if (model.empty()) {
        spdlog::error("asr: 需要 --model 指定 whisper 模型路径（如 ggml-base.bin）");
        return 1;
    }

    // 工作目录
    const fs::path work = fs::temp_directory_path() / "daisy-run";
    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        spdlog::error("asr: 无法创建工作目录: {}", ec.message());
        return 1;
    }

    // IR 文档
    model::IrDocument doc;
    doc.generator.version = kVersion;
    doc.generator.command = "asr";
    doc.generator.generated_at = "now";
    doc.generator.input_file = input;
    doc.config.input_file_path = input;
    doc.config.command_name = "asr";
    doc.config.language = language;
    doc.config.transcriber_engine = engine;
    doc.config.transcriber_model = model;

    core::PipelineContext ctx;
    ctx.doc = &doc;
    ctx.work_dir = work.string();
    ctx.on_progress = [](const std::string& stage, float) {
        spdlog::info("→ 阶段: {}", stage);
    };

    core::Pipeline pipeline;
    pipeline.add(std::make_shared<ops::FfmpegConvert>());
    pipeline.add(std::make_shared<ops::AudioPreprocess>());
    pipeline.add(std::make_shared<ops::Transcribe>(
        std::make_shared<providers::WhisperCppAsr>("whisper-cli", model)));
    pipeline.add(std::make_shared<ops::SentenceSplit>());

    if (!pipeline.run(ctx)) {
        spdlog::error("asr: 管线执行失败");
        return 1;
    }

    const fs::path out_path = output.empty() ? (input + ".daisy.json") : fs::path(output);
    doc.save(out_path.string());
    spdlog::info("asr: 已保存 IR -> {}", out_path.string());
    return 0;
}

// ---------------------------------------------------------------------------
// build：IR → ASS/SRT/TXT
// ---------------------------------------------------------------------------
int cmd_build(const std::string& ir_path, const std::string& format, const std::string& output) {
    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        spdlog::error("build: {}", e.what());
        return 1;
    }

    const model::Track* src = nullptr;
    for (const auto& t : doc.state.tracks) {
        if (t.type == "transcribe") {
            src = &t;
            break;
        }
    }
    if (!src) {
        spdlog::error("build: IR 中没有 transcribe 轨道");
        return 1;
    }

    model::SubtitleDocument sd;
    sd.title = doc.generator.tool;
    for (const auto& s : src->sentences) {
        model::SubtitleLine line;
        line.start = model::TimeCode(s.start_ms);
        line.end = model::TimeCode(s.end_ms);
        line.text = s.translated_text.value_or(s.text);
        sd.lines.push_back(std::move(line));
    }

    std::string body;
    std::string default_ext = "ass";
    if (format == "srt") {
        body = sd.render_srt();
        default_ext = "srt";
    } else if (format == "txt") {
        for (const auto& ln : sd.lines) body += ln.text + "\n";
        default_ext = "txt";
    } else {
        body = sd.render_ass();
    }

    const fs::path out_path =
        output.empty() ? fs::path(ir_path).replace_extension(default_ext) : fs::path(output);
    std::ofstream os(out_path, std::ios::binary);
    if (!os) {
        spdlog::error("build: 无法写入 {}", out_path.string());
        return 1;
    }
    os << body;
    spdlog::info("build: 已生成 {}（{} 行）", out_path.string(), sd.lines.size());
    return 0;
}

// ---------------------------------------------------------------------------
// validate
// ---------------------------------------------------------------------------
int cmd_validate(const std::string& ir_path) {
    const auto r = model::validate_file(ir_path);
    std::cout << (r.is_valid ? "OK: " : "FAIL: ") << ir_path << " (version " << r.version
              << ")\n";
    for (const auto& issue : r.issues) std::cout << "  - " << issue << '\n';
    return r.is_valid ? 0 : 1;
}

// ---------------------------------------------------------------------------
// doctor：环境诊断
// ---------------------------------------------------------------------------
int cmd_doctor() {
    std::cout << "daisy doctor\n";
    std::cout << "------------\n";
    const auto ffmpeg = infra::run_process({"ffmpeg", "-version"});
    std::cout << "ffmpeg: "
              << (ffmpeg.ok() && ffmpeg.value().success() ? "可用" : "缺失")
              << '\n';
    const auto whisper = infra::run_process({"whisper-cli", "--help"});
    std::cout << "whisper-cli: "
              << (whisper.ok() && whisper.value().success() ? "可用" : "缺失")
              << '\n';

    const auto cfg = model::AppConfig::load_or_default("daisy.config.json");
    std::cout << "tools dir: " << cfg.tools_dir << '\n';
    std::cout << "models dir: " << cfg.models_dir << '\n';
    std::cout << "download proxy: " << (cfg.download_proxy.empty() ? "(none)" : cfg.download_proxy)
              << '\n';
    return 0;
}

// ---------------------------------------------------------------------------
// models list
// ---------------------------------------------------------------------------
int cmd_models_list() {
    core::ProviderRegistry registry;
    registry.register_provider({"whisper.cpp", "asr", "local", {core::Capability::Asr}});
    registry.register_provider({"crispasr", "asr", "local", {core::Capability::Asr}});
    registry.register_provider({"demucs", "vocal-separation", "local", {core::Capability::VocalSeparation}});
    registry.register_provider({"qwen3-tts", "tts", "local", {core::Capability::Tts}});

    std::cout << "已注册提供者:\n";
    for (const auto& p : registry.all()) {
        std::cout << "  " << p.name << " [" << p.domain << ", " << p.kind << "]\n";
    }
    return 0;
}

} // namespace

int run(int argc, char** argv) {
    CLI::App app{"daisy — speech in, subtitles out (C++ migration of Centurion)"};
    app.set_version_flag("--version", std::string("daisy ") + kVersion);

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Verbose logging");

    // ---- asr ----
    auto* asr = app.add_subcommand("asr", "Transcribe media to an IR file (*.daisy.json)");
    std::string asr_input, asr_lang = "en", asr_engine = "whisper", asr_model, asr_output;
    asr->add_option("input", asr_input, "Input media file")->required();
    asr->add_option("--language,-l", asr_lang, "Language code (e.g. en, zh)");
    asr->add_option("--engine,-e", asr_engine, "ASR engine (whisper)");
    asr->add_option("--model,-m", asr_model, "Whisper model path (ggml-*.bin)");
    asr->add_option("--output,-o", asr_output, "Output IR path");

    // ---- build ----
    auto* build = app.add_subcommand("build", "Render an IR file to subtitles");
    std::string build_ir, build_fmt = "ass", build_out;
    build->add_option("ir", build_ir, "IR file (*.daisy.json)")->required();
    build->add_option("--format,-f", build_fmt, "ass|srt|txt")
        ->check(CLI::IsMember({"ass", "srt", "txt"}));
    build->add_option("--output,-o", build_out, "Output path");

    // ---- validate ----
    auto* validate = app.add_subcommand("validate", "Validate an IR file");
    std::string v_ir;
    validate->add_option("ir", v_ir, "IR file")->required();

    // ---- doctor ----
    app.add_subcommand("doctor", "Environment diagnostics");

    // ---- models ----
    auto* models = app.add_subcommand("models", "Model & provider registry");
    std::string models_sub = "list";
    models->add_option("sub", models_sub, "Subcommand (list)");

    // ---- init ----
    app.add_subcommand("init", "Interactive setup (stub, planned)");

    CLI11_PARSE(app, argc, argv);

    infra::init_logger(verbose);

    if (asr->parsed()) return cmd_asr(asr_input, asr_lang, asr_engine, asr_model, asr_output);
    if (build->parsed()) return cmd_build(build_ir, build_fmt, build_out);
    if (validate->parsed()) return cmd_validate(v_ir);
    if (models->parsed()) return cmd_models_list();
    if (app.get_subcommand("doctor")->parsed()) return cmd_doctor();
    if (app.get_subcommand("init")->parsed()) {
        std::cout << "init: 交互式设置将在后续版本实现\n";
        return 0;
    }

    // 无子命令
    std::cout << app.help() << '\n';
    return 0;
}

} // namespace daisy::cli
