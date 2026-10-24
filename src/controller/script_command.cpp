#include "iris/controller/commands.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "iris/asr/asr.hpp"
#include "iris/core/pipeline.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/ops/asr_pipeline.hpp"
#include "iris/script/align.hpp"
#include "iris/ui/console.hpp"
#include <nlohmann/json.hpp>

namespace iris::controller {

namespace fs = std::filesystem;

int run_from_script(const FromScriptOptions& opts) {
    if (!fs::exists(opts.input)) {
        ui::failure(_("input not found: ", "输入文件不存在：") + opts.input);
        return 1;
    }
    if (!fs::exists(opts.script)) {
        ui::failure(_("script not found: ", "剧本文件不存在：") + opts.script);
        return 1;
    }

    // ---- 1) 读剧本 ----
    std::string error;
    const auto lines = script::load_script(opts.script, !opts.keep_speaker_labels_in_text, error);
    if (lines.empty()) {
        ui::failure(error);
        return 1;
    }
    ui::banner("iris from-script", _("align a script to the media", "把剧本对齐到音视频"));
    ui::section(_("Script", "剧本"));
    ui::kv_table({{_("file", "文件"), opts.script},
                  {_("lines", "行数"), std::to_string(lines.size())},
                  {_("language", "语言"), opts.language}});
    if (!lines.empty() && !lines.front().speaker.empty()) {
        ui::note(_("speaker labels detected in the script", "检测到剧本里的说话人标签"));
    }

    // ---- 2) 转写（复用 ASR 链路，取句子级时间码）----
    asr::AsrBackend backend{};
    if (!asr::parse_asr_backend(opts.asr_engine, backend)) {
        ui::failure(_("unknown ASR engine: ", "未知 ASR 引擎：") + opts.asr_engine);
        return 1;
    }
    if (!asr::asr_backend_available(backend)) {
        ui::failure(_("ASR runtime not available in this build", "该 ASR 运行时不随本次构建提供"));
        return 1;
    }

    model::IrDocument doc;
    doc.generator.version = kVersion;
    doc.generator.command = "from-script";
    doc.generator.generated_at = "now";
    doc.generator.input_file = opts.input;
    doc.config.input_file_path = opts.input;
    doc.config.command_name = "from-script";
    doc.config.language = opts.language;
    doc.config.transcriber_engine = opts.asr_engine;
    doc.config.transcriber_model = opts.asr_model;
    doc.config.script_file_path = opts.script;

    const fs::path work = fs::temp_directory_path() / "iris-from-script";
    std::error_code ec;
    fs::create_directories(work, ec);

    ui::section(_("Transcription (for timing anchors)", "转写（作为时间锚点）"));
    auto stages = std::make_shared<ui::StagePrinter>();
    core::Pipeline pipeline = ops::make_asr_pipeline(backend, opts.asr_model);
    core::PipelineContext ctx;
    ctx.doc = &doc;
    ctx.work_dir = work.string();
    ctx.on_progress = [stages](const std::string& stage, float) { stages->begin(stage); };

    const bool pipeline_ok = pipeline.run(ctx);
    stages->finish();
    if (!pipeline_ok) {
        ui::failure(_("transcription failed (see log above)", "转写失败（见上方日志）"));
        return 1;
    }

    // 取转写轨道的句子作为锚点
    std::vector<model::Sentence> anchors;
    std::int64_t total_ms = 0;
    for (const auto& track : doc.state.tracks) {
        if (track.type != "transcribe" || track.sentences.empty()) continue;
        anchors = track.sentences;
        total_ms = anchors.back().end_ms;
        break;
    }
    ui::note(_("anchors: ", "锚点：") + std::to_string(anchors.size()) + _(" ASR sentences",
                                                                         " 句转写结果") +
             ", " + ui::human_duration(static_cast<double>(total_ms) / 1000.0));

    // ---- 3) 对齐 ----
    ui::section(_("Alignment", "对齐"));
    const auto aligned = script::align(lines, anchors, total_ms);
    std::size_t matched = 0;
    std::size_t interpolated = 0;
    for (const auto& a : aligned) {
        if (a.matched_index >= 0) {
            ++matched;
        } else {
            ++interpolated;
        }
    }
    ui::table({_("result", "结果"), _("count", "数量")},
              {{_("matched to transcript", "与转写匹配"), std::to_string(matched)},
               {_("interpolated", "插值分配"), std::to_string(interpolated)},
               {_("total lines", "总行数"), std::to_string(aligned.size())}});
    if (matched == 0) {
        ui::warning(_("no line matched the transcript — timing was spread by character count. "
                      "Check the language or the ASR engine.",
                      "没有任何一行匹配上转写 —— 时间轴按字符数铺开。请检查语言或 ASR 引擎。"));
    } else if (static_cast<double>(matched) / static_cast<double>(aligned.size()) < 0.5) {
        ui::warning(_("less than half of the script matched", "不足一半的剧本行匹配成功"));
    }

    // ---- 4) 写 IR ----
    doc.state.tracks.clear();
    doc.state.tracks.push_back(script::to_track(aligned, opts.language));
    doc.provenance.push_back({"from-script", opts.input, std::nullopt, std::nullopt});

    const std::string out = opts.out_ir.empty() ? opts.input + ".script.iris.json" : opts.out_ir;
    doc.save(out);

    // 预览前几条
    ui::section(_("Preview", "预览"));
    std::vector<std::vector<std::string>> rows;
    int shown = 0;
    for (const auto& a : aligned) {
        if (shown++ >= 8) break;
        rows.push_back({std::to_string(a.start_ms), std::to_string(a.end_ms),
                        a.line.speaker.empty() ? "-" : a.line.speaker,
                        a.matched_index >= 0 ? ui::symbols().check : ui::dim("~"), a.line.text});
    }
    ui::table({_("start", "起"), _("end", "止"), _("speaker", "说话人"), _("", ""),
               _("text", "文本")},
              rows);
    if (aligned.size() > 8) {
        ui::note(_("... ", "…… 另有 ") + std::to_string(aligned.size() - 8) + _(" more lines",
                                                                                " 行"));
    }

    ui::finish(true, _("wrote ", "已写出 ") + out);
    ui::json_out(nlohmann::json({{"ok", true},
                                 {"command", "from-script"},
                                 {"input", opts.input},
                                 {"script", opts.script},
                                 {"ir", out},
                                 {"lines", aligned.size()},
                                 {"matched", matched},
                                 {"interpolated", interpolated},
                                 {"language", opts.language}})
                     .dump(2));
    return 0;
}

} // namespace iris::controller
