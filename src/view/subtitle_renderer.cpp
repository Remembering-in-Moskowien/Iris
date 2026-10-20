#include "iris/view/subtitle_renderer.hpp"

#include <fstream>

#include "iris/infra/log.hpp"

namespace iris::view {

bool parse_subtitle_format(const std::string& format, SubtitleFormat& out) {
    if (format == "ass") {
        out = SubtitleFormat::Ass;
        return true;
    }
    if (format == "srt") {
        out = SubtitleFormat::Srt;
        return true;
    }
    if (format == "txt") {
        out = SubtitleFormat::Txt;
        return true;
    }
    return false;
}

std::string render_subtitles(const model::SubtitleDocument& doc, SubtitleFormat format) {
    switch (format) {
        case SubtitleFormat::Srt:
            return doc.render_srt();
        case SubtitleFormat::Txt: {
            std::string body;
            for (const auto& ln : doc.lines) body += ln.text + "\n";
            return body;
        }
        case SubtitleFormat::Ass:
        default:
            return doc.render_ass();
    }
}

bool SubtitleRenderer::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    const auto& st = ctx.doc->state;

    // 优先渲染 transcribe 轨道；其次翻译轨道（若有 translated_text）。
    const model::Track* src = nullptr;
    for (const auto& t : st.tracks) {
        if (t.type == "transcribe") {
            src = &t;
            break;
        }
    }
    if (!src) {
        spdlog::error("build: 没有可用的 transcribe 轨道");
        return false;
    }

    model::SubtitleDocument doc;
    doc.title = ctx.doc->generator.tool + " " + ctx.doc->generator.version;
    for (const auto& s : src->sentences) {
        model::SubtitleLine line;
        line.start = model::TimeCode(s.start_ms);
        line.end = model::TimeCode(s.end_ms);
        line.text = s.translated_text.value_or(s.text);
        doc.lines.push_back(std::move(line));
    }

    const std::string out_path = ctx.work_dir + "/output.ass";
    std::ofstream os(out_path, std::ios::binary);
    if (!os) {
        spdlog::error("build: 无法写入 {}", out_path);
        return false;
    }
    os << render_subtitles(doc, SubtitleFormat::Ass);
    if (!os) {
        spdlog::error("build: 写入失败 {}", out_path);
        return false;
    }

    ctx.record_provenance(name());
    spdlog::info("build: 已生成 {}（{} 行）", out_path, doc.lines.size());
    return true;
}

} // namespace iris::view
