#include "iris/controller/commands.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/model/subtitle.hpp"
#include "iris/subtitle/ass_script.hpp"
#include "iris/view/ass_builder.hpp"
#include "iris/view/subtitle_renderer.hpp"
#include "iris/ui/console.hpp"
#include <nlohmann/json.hpp>

namespace iris::controller {

namespace fs = std::filesystem;

int run_build(const std::string& ir_path, const std::string& format, const std::string& output,
              const view::AssBuildOptions& options) {
    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        spdlog::error("build: {}", e.what());
        return 1;
    }

    view::SubtitleFormat fmt = view::SubtitleFormat::Ass;
    std::string default_ext = "ass";
    if (!view::parse_subtitle_format(format, fmt)) {
        spdlog::error("build: 未知格式 '{}'（可选 ass | srt | txt）", format);
        return 1;
    }
    if (fmt == view::SubtitleFormat::Srt) default_ext = "srt";
    if (fmt == view::SubtitleFormat::Txt) default_ext = "txt";

    const fs::path out_path =
        output.empty() ? infra::utf8_path(ir_path).replace_extension(default_ext)
                       : infra::utf8_path(output);

    int lines = 0;
    int translated = 0;
    if (fmt == view::SubtitleFormat::Ass) {
        // 成品渲染：模板 / 双语 / 偏移 / 缩放。
        view::AssBuildResult result;
        std::string error;
        if (!view::build_ass(doc, options, result, error)) {
            spdlog::error("build: {}", error);
            return 1;
        }
        std::ofstream os(out_path, std::ios::binary);
        if (!os) {
            spdlog::error("build: 无法写入 {}", infra::path_to_utf8(out_path));
            return 1;
        }
        os << result.text;
        lines = result.stats.lines;
        translated = result.stats.translated;
        for (std::size_t i = 0; i < result.warnings.size() && i < 5; ++i)
            spdlog::warn("build: {}", result.warnings[i]);
        if (result.warnings.size() > 5)
            spdlog::warn("build: 另有 {} 条样式警告", result.warnings.size() - 5);
    } else {
        const model::Track* src = nullptr;
        for (const auto& t : doc.state.tracks) {
            if (t.type == "transcribe") {
                src = &t;
                break;
            }
        }
        if (src == nullptr) {
            spdlog::error("build: IR 中没有 transcribe 轨道");
            return 1;
        }
        model::SubtitleDocument sd;
        sd.title = doc.generator.tool;
        for (const auto& s : src->sentences) {
            if (s.comment) continue;
            model::SubtitleLine line;
            line.start = model::TimeCode(s.start_ms + options.shift_ms);
            line.end = model::TimeCode(s.end_ms + options.shift_ms);
            line.text = s.translated_text.value_or(s.text);
            sd.lines.push_back(std::move(line));
        }
        const std::string body = view::render_subtitles(sd, fmt);
        std::ofstream os(out_path, std::ios::binary);
        if (!os) {
            spdlog::error("build: 无法写入 {}", infra::path_to_utf8(out_path));
            return 1;
        }
        os << body;
        lines = static_cast<int>(sd.lines.size());
    }

    const std::string out_path_utf8 = infra::path_to_utf8(out_path);
    spdlog::info("build: 已生成 {}（{} 行）", out_path_utf8, lines);
    ui::json_out(nlohmann::json({{"ok", true},
                                 {"command", "build"},
                                 {"ir", ir_path},
                                 {"output", out_path_utf8},
                                 {"format", format},
                                 {"lines", lines},
                                 {"translatedLines", translated},
                                 {"bilingual", options.bilingual},
                                 {"template", options.template_path},
                                 {"styleSheet", options.style_sheet_path}})
                     .dump(2));
    return 0;
}

} // namespace iris::controller
