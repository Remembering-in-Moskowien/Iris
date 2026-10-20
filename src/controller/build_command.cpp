#include "iris/controller/commands.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include "iris/infra/log.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/model/subtitle.hpp"
#include "iris/view/subtitle_renderer.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

int run_build(const std::string& ir_path, const std::string& format, const std::string& output) {
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

    // Model → View：构建字幕文档并渲染。
    model::SubtitleDocument sd;
    sd.title = doc.generator.tool;
    for (const auto& s : src->sentences) {
        model::SubtitleLine line;
        line.start = model::TimeCode(s.start_ms);
        line.end = model::TimeCode(s.end_ms);
        line.text = s.translated_text.value_or(s.text);
        sd.lines.push_back(std::move(line));
    }

    view::SubtitleFormat fmt = view::SubtitleFormat::Ass;
    std::string default_ext = "ass";
    if (!view::parse_subtitle_format(format, fmt)) {
        spdlog::error("build: 未知格式 '{}'（可选 ass | srt | txt）", format);
        return 1;
    }
    if (fmt == view::SubtitleFormat::Srt) default_ext = "srt";
    if (fmt == view::SubtitleFormat::Txt) default_ext = "txt";
    const std::string body = view::render_subtitles(sd, fmt);

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

} // namespace iris::controller
