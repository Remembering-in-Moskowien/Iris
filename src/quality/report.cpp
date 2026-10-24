#include "iris/quality/report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <unordered_set>

namespace iris::quality {

namespace {

/// UTF-8 字符数（非法字节按 1 计）。
std::size_t utf8_chars(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > s.size()) len = 1;
        i += len;
        ++n;
    }
    return n;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

/// HTML 转义（& < > " '）。
std::string escape_html(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string fmt(const char* pattern, double v) {
    char buf[128];
    std::snprintf(buf, sizeof buf, pattern, v);
    return buf;
}

/// 两个数值的格式（避免把两个占位符塞给单参数 snprintf）。
std::string fmt2(const char* pattern, double a, double b) {
    char buf[128];
    std::snprintf(buf, sizeof buf, pattern, a, b);
    return buf;
}

} // namespace

double max_cps_for(const std::string& language) {
    // 语言标签无法判断时按中文的宽松值
    if (language == "en" || language == "de" || language == "fr" || language == "es" ||
        language == "ru") {
        return 20.0;
    }
    return 12.0;
}

TrackMetrics analyze_track(const model::Track& track, double max_cps) {
    TrackMetrics m;
    m.type = track.type;
    m.language = track.language;
    m.cues = static_cast<int>(track.sentences.size());
    if (track.sentences.empty()) return m;

    const auto& sents = track.sentences;
    const std::int64_t first = sents.front().start_ms;
    std::int64_t last = sents.front().end_ms;
    std::int64_t covered = 0;
    std::int64_t gap_sum = 0;
    int gap_count = 0;
    m.min_gap_ms = 0;
    m.max_gap_ms = 0;
    m.min_chars = -1;
    m.max_chars = 0;

    std::unordered_set<std::string> seen;
    std::int64_t prev_end = -1;

    for (std::size_t i = 0; i < sents.size(); ++i) {
        const auto& s = sents[i];
        const int index = static_cast<int>(i) + 1;
        last = std::max(last, s.end_ms);

        const std::string text = trim(s.text);
        const auto chars = static_cast<int>(utf8_chars(text));
        const auto dur_ms = std::max<std::int64_t>(s.end_ms - s.start_ms, 1);
        const double dur_s = static_cast<double>(dur_ms) / 1000.0;
        const double cps = static_cast<double>(chars) / dur_s;

        if (chars == 0) {
            ++m.empty_cues;
            m.issues.push_back({index, "empty", "error", "空字幕"});
        } else {
            m.min_chars = m.min_chars < 0 ? chars : std::min(m.min_chars, chars);
            m.max_chars = std::max(m.max_chars, chars);
            m.avg_chars += static_cast<double>(chars);
            m.avg_cps += cps;
            m.max_cps = std::max(m.max_cps, cps);
            if (cps > max_cps) {
                ++m.fast_cues;
                m.issues.push_back({index, "fast", "warning",
                                    fmt2("语速 %.1f 字/秒（上限 %.0f）", cps, max_cps)});
            }
            if (!seen.insert(text).second) {
                ++m.duplicates;
                m.issues.push_back({index, "duplicate", "warning", "与前面某条文本重复"});
            }
        }

        if (s.end_ms > s.start_ms) covered += s.end_ms - s.start_ms;

        if (prev_end >= 0) {
            const std::int64_t gap = s.start_ms - prev_end;
            if (gap < 0) {
                ++m.overlaps;
                m.issues.push_back({index, "overlap", "error",
                                    fmt("与上一条重叠 %.0f ms", static_cast<double>(-gap))});
            } else {
                gap_sum += gap;
                ++gap_count;
                m.min_gap_ms = gap_count == 1 ? gap : std::min(m.min_gap_ms, gap);
                m.max_gap_ms = std::max(m.max_gap_ms, gap);
                if (gap > 2000) {
                    ++m.long_gaps;
                    m.issues.push_back({index, "long-gap", "warning",
                                        fmt("与上一条间隔 %.1f s", static_cast<double>(gap) / 1000.0)});
                }
            }
        }
        prev_end = s.end_ms;

        if (s.speaker && !s.speaker->empty()) ++m.speakers[*s.speaker];
    }

    m.span_ms = std::max<std::int64_t>(last - first, 0);
    m.coverage = m.span_ms > 0 ? static_cast<double>(covered) / static_cast<double>(m.span_ms) : 0.0;
    m.avg_chars = m.cues > 0 ? m.avg_chars / static_cast<double>(m.cues) : 0.0;
    m.avg_cps = m.cues > 0 ? m.avg_cps / static_cast<double>(m.cues) : 0.0;
    m.avg_gap_ms = gap_count > 0 ? static_cast<double>(gap_sum) / gap_count : 0.0;
    if (m.min_chars < 0) m.min_chars = 0;
    return m;
}

Report analyze(const model::IrDocument& doc) {
    Report report;
    for (const auto& track : doc.state.tracks) {
        auto m = analyze_track(track, max_cps_for(track.language.empty() ? doc.config.language
                                                                        : track.language));
        for (const auto& issue : m.issues) {
            if (issue.severity == "error") {
                ++report.errors;
            } else if (issue.severity == "warning") {
                ++report.warnings;
            }
        }
        report.tracks.push_back(std::move(m));
    }
    return report;
}

std::string to_html(const model::IrDocument& doc, const Report& report) {
    std::ostringstream html;
    html << "<!doctype html>\n<html lang=\"zh\">\n<head>\n<meta charset=\"utf-8\">\n"
         << "<title>iris 字幕质量报告</title>\n<style>\n"
         << "body{font-family:-apple-system,'Segoe UI',Roboto,'Noto Sans SC',sans-serif;"
            "margin:2rem;background:#f6f7f9;color:#1f2328}\n"
         << "h1{font-size:1.5rem;margin:0 0 .25rem}\n"
         << ".sub{color:#57606a;margin-bottom:1.5rem}\n"
         << ".cards{display:flex;gap:1rem;flex-wrap:wrap;margin-bottom:1.5rem}\n"
         << ".card{background:#fff;border:1px solid #d0d7de;border-radius:8px;padding:1rem 1.25rem;"
            "min-width:9rem}\n"
         << ".card .k{color:#57606a;font-size:.8rem}\n.card .v{font-size:1.6rem;font-weight:600}\n"
         << "table{border-collapse:collapse;width:100%;background:#fff;border:1px solid #d0d7de;"
            "border-radius:8px;overflow:hidden;margin-bottom:1.5rem}\n"
         << "th,td{padding:.5rem .75rem;border-bottom:1px solid #eaeef2;text-align:left;"
            "font-size:.9rem;vertical-align:top}\n"
         << "th{background:#f6f8fa;font-weight:600}\n"
         << ".error{color:#cf222e;font-weight:600}\n.warning{color:#9a6700;font-weight:600}\n"
         << ".info{color:#0969da}\n.mono{font-family:ui-monospace,Consolas,monospace}\n"
         << "</style>\n</head>\n<body>\n";

    html << "<h1>iris 字幕质量报告</h1>\n<div class=\"sub\">"
         << escape_html(doc.generator.input_file.value_or("")) << " · 命令 "
         << escape_html(doc.generator.command.value_or("")) << " · 语言 "
         << escape_html(doc.config.language) << "</div>\n";

    html << "<div class=\"cards\">\n";
    const auto card = [&](const std::string& k, const std::string& v, const std::string& cls) {
        html << "<div class=\"card\"><div class=\"k\">" << escape_html(k) << "</div>"
             << "<div class=\"v " << cls << "\">" << escape_html(v) << "</div></div>\n";
    };
    const int total_cues = [&] {
        int n = 0;
        for (const auto& t : report.tracks) n += t.cues;
        return n;
    }();
    card("轨道数", std::to_string(report.tracks.size()), "");
    card("条目总数", std::to_string(total_cues), "");
    card("错误", std::to_string(report.errors), report.errors > 0 ? "error" : "");
    card("警告", std::to_string(report.warnings), report.warnings > 0 ? "warning" : "");
    html << "</div>\n";

    for (const auto& t : report.tracks) {
        html << "<h2>轨道 " << escape_html(t.type) << "（" << escape_html(t.language) << "）</h2>\n";
        html << "<table>\n<tr><th>指标</th><th>值</th><th>指标</th><th>值</th></tr>\n";
        const auto row2 = [&](const std::string& k1, const std::string& v1, const std::string& k2,
                              const std::string& v2) {
            html << "<tr><td>" << escape_html(k1) << "</td><td class=\"mono\">" << escape_html(v1)
                 << "</td><td>" << escape_html(k2) << "</td><td class=\"mono\">" << escape_html(v2)
                 << "</td></tr>\n";
        };
        row2("条目数", std::to_string(t.cues), "覆盖时长", fmt("%.1f s", t.span_ms / 1000.0));
        row2("覆盖率", fmt("%.1f%%", t.coverage * 100.0),
             "平均字符数", fmt("%.1f", t.avg_chars));
        row2("最短/最长", std::to_string(t.min_chars) + " / " + std::to_string(t.max_chars),
             "平均/最高 CPS", fmt2("%.1f / %.1f", t.avg_cps, t.max_cps));
        row2("重叠", std::to_string(t.overlaps), "过长空白", std::to_string(t.long_gaps));
        row2("最小/最大间隔",
             fmt2("%.0f / %.0f ms", static_cast<double>(t.min_gap_ms),
                  static_cast<double>(t.max_gap_ms)),
             "重复/空行", std::to_string(t.duplicates) + " / " + std::to_string(t.empty_cues));
        html << "</table>\n";

        if (!t.speakers.empty()) {
            html << "<table>\n<tr><th>说话人</th><th>条数</th></tr>\n";
            for (const auto& [speaker, count] : t.speakers) {
                html << "<tr><td>" << escape_html(speaker) << "</td><td class=\"mono\">" << count
                     << "</td></tr>\n";
            }
            html << "</table>\n";
        }

        if (!t.issues.empty()) {
            html << "<table>\n<tr><th>条目</th><th>级别</th><th>类型</th><th>说明</th></tr>\n";
            for (const auto& issue : t.issues) {
                html << "<tr><td class=\"mono\">" << issue.cue_index << "</td><td class=\""
                     << escape_html(issue.severity) << "\">" << escape_html(issue.severity)
                     << "</td><td>" << escape_html(issue.kind) << "</td><td>"
                     << escape_html(issue.detail) << "</td></tr>\n";
            }
            html << "</table>\n";
        }
    }

    // 前 50 条内容预览
    for (const auto& track : doc.state.tracks) {
        if (track.sentences.empty()) continue;
        html << "<h2>内容预览（" << escape_html(track.type) << "）</h2>\n";
        html << "<table>\n<tr><th>#</th><th>起</th><th>止</th><th>说话人</th><th>文本</th></tr>\n";
        int n = 0;
        for (const auto& s : track.sentences) {
            if (++n > 50) break;
            html << "<tr><td class=\"mono\">" << n << "</td><td class=\"mono\">"
                 << s.start_ms << "</td><td class=\"mono\">" << s.end_ms << "</td><td>"
                 << escape_html(s.speaker.value_or("")) << "</td><td>"
                 << escape_html(s.text) << "</td></tr>\n";
        }
        html << "</table>\n";
    }

    html << "</body>\n</html>\n";
    return html.str();
}

} // namespace iris::quality
