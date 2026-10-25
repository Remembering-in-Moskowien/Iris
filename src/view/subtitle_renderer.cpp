#include "iris/view/subtitle_renderer.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

#include "iris/core/extension_registry.hpp"
#include "iris/infra/log.hpp"

namespace iris::view {
namespace {

using RenderFunction = std::string (*)(const model::SubtitleDocument&);

std::string render_ass(const model::SubtitleDocument& document) {
    return document.render_ass();
}

std::string render_srt(const model::SubtitleDocument& document) {
    return document.render_srt();
}

std::string render_txt(const model::SubtitleDocument& document) {
    std::string body;
    for (const auto& line : document.lines) body += line.text + "\n";
    return body;
}

struct RenderRegistration {
    SubtitleFormat format;
    RenderFunction render;
};

const core::ExtensionRegistry<RenderRegistration>& renderers() {
    static const auto registry = [] {
        core::ExtensionRegistry<RenderRegistration> result;
        const auto add = [&](std::string id, SubtitleFormat format, RenderFunction render) {
            std::string error;
            if (!result.register_extension(std::move(id), {}, {format, render}, error)) {
                throw std::logic_error("invalid built-in renderer registration: " + error);
            }
        };
        add("ass", SubtitleFormat::Ass, render_ass);
        add("srt", SubtitleFormat::Srt, render_srt);
        add("txt", SubtitleFormat::Txt, render_txt);
        return result;
    }();
    return registry;
}

const core::ExtensionRegistry<RenderRegistration>::Entry* find_renderer(
    SubtitleFormat format) {
    const auto& entries = renderers().entries();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const auto& entry) {
                                     return entry.extension.format == format;
                                 });
    return it == entries.end() ? nullptr : &*it;
}

} // namespace

bool parse_subtitle_format(const std::string& format, SubtitleFormat& out) {
    const auto* entry = renderers().find(format);
    if (!entry) return false;
    out = entry->extension.format;
    return true;
}

std::string render_subtitles(const model::SubtitleDocument& doc, SubtitleFormat format) {
    const auto* entry = find_renderer(format);
    return entry ? entry->extension.render(doc) : render_ass(doc);
}

std::vector<RenderFormatInfo> registered_render_formats() {
    std::vector<RenderFormatInfo> out;
    for (const auto& entry : renderers().entries()) {
        out.push_back({entry.id, entry.extension.format});
    }
    return out;
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

    ctx.subtitle_path = out_path;
    ctx.record_provenance(name());
    spdlog::info("build: 已生成 {}（{} 行）", out_path, doc.lines.size());
    return true;
}

} // namespace iris::view
