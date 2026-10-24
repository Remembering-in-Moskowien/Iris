#include "iris/controller/commands.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "iris/asr/asr.hpp"
#include "iris/core/pipeline.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/ui/console.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/ops/asr_pipeline.hpp"
#include "iris/ops/translate.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

bool is_ir_path(const std::string& path) { return fs::path(path).extension() == ".json"; }

/// 目标语言后缀：out.zh.iris.json
std::string default_out_path(const std::string& input, const std::string& target) {
    const fs::path p(input);
    const std::string stem = p.stem().string();
    const std::string clean = stem.empty() ? "out" : stem;
    return (p.parent_path() / (clean + "." + target + ".iris.json")).string();
}

} // namespace

int run_translate(const TranslateCommandOptions& opts) {
    if (!fs::exists(opts.input)) {
        std::cerr << _("translate: input not found: ", "translate: 输入文件不存在：") << opts.input
                  << '\n';
        return 1;
    }
    if (opts.model.empty() || !fs::exists(opts.model)) {
        std::cerr << _("translate: model not found: ", "translate: 翻译模型不存在：") << opts.model
                  << '\n';
        return 1;
    }

    const bool from_ir = is_ir_path(opts.input);
    const fs::path work = fs::temp_directory_path() / "iris-translate";
    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        std::cerr << _("translate: cannot create work dir: ", "translate: 无法创建工作目录：")
                  << ec.message() << '\n';
        return 1;
    }

    model::IrDocument doc;
    core::Pipeline pipeline;

    if (from_ir) {
        doc = model::IrDocument::load(opts.input);
        spdlog::info("translate: 复用 IR {}", opts.input);
    } else {
        asr::AsrBackend backend{};
        if (!asr::parse_asr_backend(opts.asr_engine, backend)) {
            std::cerr << _("translate: unknown ASR engine '", "translate: 未知 ASR 引擎 '")
                      << opts.asr_engine << "'\n";
            return 1;
        }
        if (opts.asr_model.empty() || !fs::exists(opts.asr_model)) {
            std::cerr << _("translate: --asr-model is required for media input\n",
                           "translate: 媒体输入需要 --asr-model\n");
            return 1;
        }
        doc.generator.version = kVersion;
        doc.generator.command = "translate";
        doc.generator.generated_at = "now";
        doc.generator.input_file = opts.input;
        doc.config.input_file_path = opts.input;
        doc.config.command_name = "translate";
        doc.config.language = opts.from;
        doc.config.transcriber_engine = opts.asr_engine;
        doc.config.transcriber_model = opts.asr_model;
        pipeline = ops::make_asr_pipeline(backend, opts.asr_model);
    }

    ops::Translate::Options tr;
    tr.mt.model = opts.model;
    tr.mt.engine = opts.engine;
    tr.mt.source = opts.from;
    tr.mt.target = opts.to;
    tr.mt.beam_size = opts.beam_size;
    tr.mt.max_tokens = opts.max_tokens;
    tr.after_asr = !from_ir;
    pipeline.add(std::make_shared<ops::Translate>(std::move(tr)));

    core::PipelineContext ctx;
    ctx.doc = &doc;
    ctx.work_dir = work.string();
    auto stages = std::make_shared<ui::StagePrinter>();
    ctx.on_progress = [stages](const std::string& stage, float) { stages->begin(stage); };

    const bool pipeline_ok = pipeline.run(ctx);
    stages->finish();
    if (!pipeline_ok) {
        std::cerr << _("translate: pipeline failed (see log above)\n",
                       "translate: 管线执行失败（见上方日志）\n");
        return 1;
    }

    const std::string out = !opts.out_ir.empty()
                                ? opts.out_ir
                                : (from_ir ? default_out_path(opts.input, opts.to) : std::string());
    if (!out.empty()) {
        doc.save(out);
        spdlog::info("translate: IR → {}", out);
    }

    // 输出译文（便于直接观察质量）
    for (const auto& track : doc.state.tracks) {
        if (track.type != "translation") continue;
        for (const auto& s : track.sentences) {
            if (s.translated_text) std::cout << *s.translated_text << '\n';
        }
    }
    return 0;
}

} // namespace iris::controller
