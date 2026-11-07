#include "iris/controller/commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/infra/i18n.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/subtitle/combine.hpp"
#include "iris/subtitle/parser.hpp"
#include "iris/subtitle/reconcile.hpp"
#include "iris/text/sat.hpp"
#include "iris/ui/console.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

bool has_json_ext(const std::string& path) {
    std::string ext = infra::path_to_utf8(infra::utf8_path(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".json";
}

/// 字幕文件没有语言元数据时按内容猜：CJK 字符占比高 → zh，否则 en。
std::string guess_language(const model::Track& track) {
    int cjk = 0;
    int latin = 0;
    std::size_t checked = 0;
    for (const auto& s : track.sentences) {
        if (checked++ >= 80) break;
        for (std::size_t i = 0; i < s.text.size();) {
            const auto b = static_cast<unsigned char>(s.text[i]);
            std::size_t len = 1;
            if ((b & 0xE0) == 0xC0) len = 2;
            else if ((b & 0xF0) == 0xE0) len = 3;
            else if ((b & 0xF8) == 0xF0) len = 4;
            if (len == 3 && i + 2 < s.text.size()) {
                const unsigned cp = ((b & 0x0F) << 12) |
                                    ((static_cast<unsigned char>(s.text[i + 1]) & 0x3F) << 6) |
                                    (static_cast<unsigned char>(s.text[i + 2]) & 0x3F);
                if ((cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF)) ++cjk;
            } else if (std::isalpha(b) != 0) {
                ++latin;
            }
            i += len;
        }
    }
    if (cjk == 0 && latin == 0) return {};
    return cjk * 3 >= latin ? "zh" : "en";
}

/// 读一侧输入：IR（取主轨）或字幕文件（SRT/ASS/VTT）。
bool load_side(const std::string& path, const std::string& fallback_language, model::IrDocument& doc,
               model::Track& track, std::string& error) {
    if (has_json_ext(path)) {
        try {
            doc = model::IrDocument::load(path);
        } catch (const std::exception& e) {
            error = "读取 IR 失败：" + path + "：" + e.what();
            return false;
        }
        const model::Track* picked = nullptr;
        for (const auto& t : doc.state.tracks) {
            if (t.type == "transcribe") {
                picked = &t;
                break;
            }
        }
        if (picked == nullptr && !doc.state.tracks.empty()) picked = &doc.state.tracks.front();
        if (picked == nullptr) {
            error = "IR 里没有轨道：" + path;
            return false;
        }
        track = *picked;
        if (track.language.empty()) track.language = doc.config.language;
        return true;
    }

    std::vector<subtitle::Cue> cues;
    if (!subtitle::parse_file(path, cues, error)) return false;
    track = model::Track{};
    track.type = "transcribe";
    track.language = fallback_language;
    track.sentences.reserve(cues.size());
    for (const auto& cue : cues) {
        model::Sentence s;
        s.start_ms = cue.start_ms;
        s.end_ms = cue.end_ms;
        s.text = cue.text;
        track.sentences.push_back(std::move(s));
    }
    const std::string guessed = guess_language(track);
    if (!guessed.empty()) track.language = guessed;
    return true;
}

std::string html_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += "<br>"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string timecode(std::int64_t ms) {
    char buf[32];
    const std::int64_t total = std::max<std::int64_t>(ms, 0);
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld", total / 3600000,
                  (total / 60000) % 60, (total / 1000) % 60, total % 1000);
    return buf;
}

bool apply_sat_split(model::Track& track, text::SatSegmenter& sat, std::string& error) {
    std::vector<model::Sentence> split;
    split.reserve(track.sentences.size() * 2);
    for (const auto& sentence : track.sentences) {
        std::vector<std::size_t> points;
        if (!sat.split_points(sentence.text, 0.5f, points, error) || points.empty()) {
            split.push_back(sentence);
            continue;
        }
        std::vector<std::string> parts;
        std::size_t begin = 0;
        for (const auto point : points) {
            if (point <= begin || point >= sentence.text.size()) continue;
            const auto part = sentence.text.substr(begin, point - begin);
            if (!part.empty()) parts.push_back(part);
            begin = point;
        }
        if (begin < sentence.text.size()) parts.push_back(sentence.text.substr(begin));
        if (parts.size() <= 1) {
            split.push_back(sentence);
            continue;
        }
        const auto duration = std::max<std::int64_t>(sentence.end_ms - sentence.start_ms, 1);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            auto child = sentence;
            child.text = parts[i];
            child.start_ms =
                sentence.start_ms + duration * static_cast<std::int64_t>(i) / parts.size();
            child.end_ms =
                sentence.start_ms + duration * static_cast<std::int64_t>(i + 1) / parts.size();
            child.words.clear();
            split.push_back(std::move(child));
        }
    }
    track.sentences = std::move(split);
    return true;
}

std::string render_report(const std::string& primary_path, const std::string& secondary_path,
                          const subtitle::ReconcileStats& stats,
                          const std::vector<subtitle::ReconcileRow>& rows) {
    std::ostringstream html;
    html << "<!doctype html><html lang=\"zh\"><meta charset=\"utf-8\">\n";
    html << "<title>iris merge report</title>\n";
    html << "<style>body{font:14px/1.6 system-ui,'Segoe UI',sans-serif;margin:32px;color:#1c1c1e;"
            "background:#fafafa}h1{font-size:20px}table{border-collapse:collapse;width:100%;"
            "background:#fff}th,td{border:1px solid #e2e2e5;padding:6px 10px;text-align:left;"
            "vertical-align:top}th{background:#f2f2f4}td.t{font-variant-numeric:tabular-nums;"
            "white-space:nowrap;color:#666}.k{display:inline-block;padding:1px 8px;border-radius:10px;"
            "background:#eef1f6;font-size:12px}.k.dropped{background:#fdecea;color:#8b2c25}"
            ".k.split-failed{background:#fff4e5;color:#8a5300}.k.cps-zh{background:#fff8e1;"
            "color:#7a5b00}.sum{display:flex;gap:18px;flex-wrap:wrap;margin:16px 0}"
            ".sum div{background:#fff;border:1px solid #e2e2e5;border-radius:8px;padding:8px 14px}"
            "</style>\n";
    html << "<h1>双轨合并报告</h1>\n";
    html << "<p>主轨：" << html_escape(infra::path_to_utf8(infra::utf8_path(primary_path).filename()))
         << "　译文轨："
         << html_escape(infra::path_to_utf8(infra::utf8_path(secondary_path).filename()))
         << "</p>\n";
    html << "<div class=\"sum\">"
         << "<div>主轨 " << stats.primary_cues << "</div>"
         << "<div>译文 " << stats.secondary_cues << "</div>"
         << "<div>配对上 " << stats.paired << "</div>"
         << "<div>译文拼接 " << stats.merged_secondary << "</div>"
         << "<div>译文拆分 " << stats.split_secondary << "</div>"
         << "<div>切分失败 " << stats.split_failed << "</div>"
         << "<div>英文合并 " << stats.english_merged << "</div>"
         << "<div>单语保留 " << stats.kept << "</div>"
         << "<div>单语丢弃 " << stats.dropped << "</div>"
         << "<div>术语补译 " << stats.glossed << "</div>"
         << "<div>注释 " << stats.secondary_only << "</div>"
         << "<div>未归属 " << stats.unlinked_secondary << "</div>"
         << "<div>CPS 超限（中） " << stats.cps_over_zh << "</div>"
         << "<div>CPS 超限（英） " << stats.cps_over_en << "</div>"
         << "</div>\n";

    if (rows.empty()) {
        html << "<p>没有需要人工复核的行。</p>\n";
    } else {
        html << "<table><tr><th>类型</th><th>时间</th><th>主轨</th><th>说明</th></tr>\n";
        for (const auto& row : rows) {
            html << "<tr><td><span class=\"k " << html_escape(row.kind) << "\">"
                 << html_escape(row.kind) << "</span></td><td class=\"t\">" << timecode(row.start_ms)
                 << " → " << timecode(row.end_ms) << "</td><td>" << html_escape(row.text)
                 << "</td><td>" << html_escape(row.detail) << "</td></tr>\n";
        }
        html << "</table>\n";
    }
    html << "</body></html>\n";
    return html.str();
}

} // namespace

int run_merge(const std::string& primary_path, const std::string& secondary_path,
              const std::string& out_path, const MergeOptions& options) {
    model::IrDocument primary_doc;
    model::IrDocument secondary_doc;
    model::Track primary;
    model::Track secondary;
    std::string error;

    if (!load_side(primary_path, options.primary_language, primary_doc, primary, error)) {
        std::cerr << "merge: " << error << '\n';
        return 1;
    }
    if (!load_side(secondary_path, options.secondary_language, secondary_doc, secondary, error)) {
        std::cerr << "merge: " << error << '\n';
        return 1;
    }

    // 参数顺序写反也能救：按 IR 里记录的语言判断谁是主轨。
    if (!primary.language.empty() && !secondary.language.empty() &&
        primary.language != options.primary_language &&
        secondary.language == options.primary_language) {
        std::swap(primary, secondary);
        std::swap(primary_doc, secondary_doc);
        ui::warning(_("merge: inputs were swapped (second file is the primary track)",
                      "merge: 两个输入顺序反了，已按语言自动互换"));
    }

    if (options.split_strategy != "rule" && options.split_strategy != "sat") {
        std::cerr << "merge: unsupported split strategy: " << options.split_strategy << '\n';
        return 1;
    }
    if (options.split_strategy == "sat") {
        text::SatSegmenter sat;
        if (!sat.open(options.split_model, error)) {
            ui::warning(_("merge: SaT unavailable, keeping original cues: ",
                          "merge: SaT 不可用，保留原始字幕句：") + error);
        } else {
            if (!apply_sat_split(primary, sat, error) || !apply_sat_split(secondary, sat, error)) {
                std::cerr << "merge: SaT split failed: " << error << '\n';
                return 1;
            }
        }
    }

    subtitle::ReconcileOptions ro;
    ro.primary_language = primary.language.empty() ? options.primary_language : primary.language;
    ro.secondary_language =
        secondary.language.empty() ? options.secondary_language : secondary.language;
    ro.policy.sdh = options.policy_sdh;
    ro.policy.interjection = options.policy_interjection;
    ro.policy.dialogue = options.policy_dialogue;
    ro.policy.lyric = options.policy_lyric;
    ro.glossary_path = options.glossary_path;
    ro.allow_english_merge = options.allow_english_merge;
    ro.max_chars_en = options.max_chars_en;
    ro.max_lines_en = options.max_lines_en;
    ro.max_cps_en = options.max_cps_en;
    ro.max_cps_zh = options.max_cps_zh;

    model::Track merged;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    if (!subtitle::reconcile(primary, secondary, ro, merged, stats, review, error)) {
        std::cerr << "merge: " << error << '\n';
        return 1;
    }

    model::IrDocument doc;
    doc.generator.version = kVersion;
    doc.generator.command = "merge";
    doc.generator.generated_at = "now";
    doc.generator.input_file = primary_path;
    doc.config.input_file_path = primary_path;
    doc.config.command_name = "merge";
    doc.config.language = merged.language;
    if (!primary_doc.state.styles.empty()) {
        doc.state.styles = primary_doc.state.styles;
        doc.state.script_info = primary_doc.state.script_info;
    }
    doc.state.tracks.push_back(merged);
    doc.provenance.push_back({"merge", primary_path, std::nullopt, std::nullopt});
    doc.provenance.push_back({"merge", secondary_path, std::nullopt, std::nullopt});

    const std::string out = out_path.empty() ? primary_path + ".bi.iris.json" : out_path;
    doc.save(out);

    if (!options.report_path.empty()) {
        std::ofstream f(infra::utf8_path(options.report_path), std::ios::binary);
        if (!f) {
            std::cerr << _("merge: cannot write report ", "merge: 无法写入报告 ")
                      << options.report_path << '\n';
        } else {
            f << render_report(primary_path, secondary_path, stats, review);
        }
    }

    ui::success(_("merge: ", "merge: ") + std::to_string(stats.paired) +
                _(" paired, ", " 条配上译文，") + std::to_string(stats.merged_secondary) +
                _(" merged, ", " 条拼接，") + std::to_string(stats.split_secondary) +
                _(" split, ", " 条拆分，") + std::to_string(stats.english_merged) +
                _(" EN merged, ", " 条英文合并，") + std::to_string(stats.kept) +
                _(" kept, ", " 条单语保留，") + std::to_string(stats.dropped) +
                _(" dropped", " 条丢弃") + _(" → ", " → ") + out);
    if (stats.split_failed > 0) {
        ui::warning(_("merge: ", "merge: ") + std::to_string(stats.split_failed) +
                    _(" translation line(s) could not be split", " 条译文切不开（见报告）"));
    }
    if (!options.report_path.empty()) ui::note("report: " + options.report_path);

    ui::json_out(nlohmann::json({{"ok", true},
                                 {"command", "merge"},
                                 {"ir", out},
                                 {"primaryEvents", stats.primary_cues},
                                 {"secondaryCues", stats.secondary_cues},
                                 {"paired", stats.paired},
                                 {"mergedSecondary", stats.merged_secondary},
                                 {"splitSecondary", stats.split_secondary},
                                 {"splitFailed", stats.split_failed},
                                 {"englishMerged", stats.english_merged},
                                 {"kept", stats.kept},
                                 {"dropped", stats.dropped},
                                 {"glossed", stats.glossed},
                                 {"annotations", stats.secondary_only},
                                 {"unlinked", stats.unlinked_secondary},
                                 {"cpsOverZh", stats.cps_over_zh},
                                 {"cpsOverEn", stats.cps_over_en},
                                 {"report", options.report_path}})
                     .dump(2));
    return 0;
}

} // namespace iris::controller
