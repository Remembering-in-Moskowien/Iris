#include "iris/text/reference_diff.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>

namespace iris::text {

namespace {

std::string escape_html(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string mmss(std::int64_t ms) {
    const auto total = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld", (long long)(total / 60),
                  (long long)(total % 60));
    return buf;
}

int text_len(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((b & 0xE0) == 0xC0) len = 2;
        else if ((b & 0xF0) == 0xE0) len = 3;
        else if ((b & 0xF8) == 0xF0) len = 4;
        if (!std::isspace(b)) ++n;
        i += len;
    }
    return n;
}

bool has_cjk(const std::string& s) {
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        const auto b = static_cast<unsigned char>(s[i]);
        if ((b & 0xF0) != 0xE0) continue;
        const unsigned cp = ((b & 0x0F) << 12) |
                            ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                            (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
    }
    return false;
}

/// 对读用的文本：有译文时用译文（中文），否则用原文。
const std::string& display_text(const model::Sentence& s) {
    if (s.translated_text && !s.translated_text->empty()) return *s.translated_text;
    return s.text;
}

std::vector<std::uint32_t> codepoints(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((b & 0xE0) == 0xC0) len = 2;
        else if ((b & 0xF0) == 0xE0) len = 3;
        else if ((b & 0xF8) == 0xF0) len = 4;
        std::uint32_t cp = b;
        if (len > 1) {
            cp = (len == 2) ? (b & 0x1F) : (len == 3) ? (b & 0x0F) : (b & 0x07);
            for (std::size_t k = 1; k < len; ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!std::isspace(static_cast<unsigned char>(s[i]))) out.push_back(cp);
        i += len;
    }
    return out;
}

/// 字符级 LCS 相似度（0..1）；用来判断「同一时间段里是不是同一句话」。
double similarity(const std::string& a, const std::string& b) {
    const auto lhs = codepoints(a);
    const auto rhs = codepoints(b);
    if (lhs.empty() && rhs.empty()) return 1.0;
    if (lhs.empty() || rhs.empty()) return 0.0;
    std::vector<int> prev(rhs.size() + 1, 0), cur(rhs.size() + 1, 0);
    for (std::size_t i = 1; i <= lhs.size(); ++i) {
        for (std::size_t j = 1; j <= rhs.size(); ++j) {
            cur[j] = lhs[i - 1] == rhs[j - 1] ? prev[j - 1] + 1
                                              : std::max(prev[j], cur[j - 1]);
        }
        std::swap(prev, cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    const double lcs = static_cast<double>(prev[rhs.size()]);
    return lcs / static_cast<double>(std::max(lhs.size(), rhs.size()));
}

} // namespace

RefDiffReport diff_reference(const model::IrDocument& doc,
                             const std::vector<subtitle::Cue>& reference, double min_overlap,
                             int max_issues, bool cjk_only) {
    RefDiffReport report;
    report.reference_cues = static_cast<int>(reference.size());

    const model::Track* track = nullptr;
    for (const auto& t : doc.state.tracks) {
        if (t.type == "transcribe") {
            track = &t;
            break;
        }
    }
    if (track == nullptr && !doc.state.tracks.empty()) track = &doc.state.tracks.front();
    if (track == nullptr) return report;

    std::vector<subtitle::Cue> cues = reference;
    std::sort(cues.begin(), cues.end(),
              [](const subtitle::Cue& a, const subtitle::Cue& b) { return a.start_ms < b.start_ms; });

    int index = 0;
    std::vector<const model::Sentence*> considered;
    for (const auto& s : track->sentences) {
        if (s.comment) continue;
        const std::string& value = display_text(s);
        if (value.empty()) continue;
        if (cjk_only && !has_cjk(value)) continue;
        considered.push_back(&s);
    }
    std::sort(considered.begin(), considered.end(),
              [](const model::Sentence* a, const model::Sentence* b) {
                  return a->start_ms < b->start_ms;
              });

    for (const auto* sp : considered) {
        const auto& s = *sp;
        ++index;
        ++report.sentences;
        const std::string& value = display_text(s);

        // 只在「时间上可能重叠」的参考条里找，避免全文两两比对。
        const subtitle::Cue* best = nullptr;
        double best_sim = -1.0;
        double best_overlap = 0.0;
        auto begin = std::lower_bound(
            cues.begin(), cues.end(), s.start_ms - 20000,
            [](const subtitle::Cue& c, std::int64_t v) { return c.start_ms < v; });
        for (auto it = begin; it != cues.end() && it->start_ms <= s.end_ms; ++it) {
            const auto lo = std::max(s.start_ms, it->start_ms);
            const auto hi = std::min(s.end_ms, it->end_ms);
            if (hi <= lo) continue;
            const double overlap = static_cast<double>(hi - lo) /
                                   static_cast<double>(std::max<std::int64_t>(s.end_ms - s.start_ms, 1));
            const double sim = similarity(value, it->text);
            if (sim > best_sim || (sim == best_sim && overlap > best_overlap)) {
                best_sim = sim;
                best_overlap = overlap;
                best = &(*it);
            }
        }

        if (best == nullptr || best_sim < 0.55) {
            ++report.missing_in_reference;
            if (static_cast<int>(report.issues.size()) < max_issues) {
                report.issues.push_back({index, s.start_ms, s.end_ms, value,
                                         best == nullptr ? std::string() : best->text, "mismatch"});
            }
            continue;
        }

        ++report.matched;
        const double len_ratio = static_cast<double>(text_len(value)) /
                                 static_cast<double>(std::max(1, text_len(best->text)));
        if (len_ratio < 0.45 || len_ratio > 2.2) {
            ++report.length_mismatch;
            if (static_cast<int>(report.issues.size()) < max_issues) {
                report.issues.push_back({index, s.start_ms, s.end_ms, value, best->text, "length"});
            }
        }
    }

    // 参考里有、我们的时间轴上没有对应句子的条目（时间重叠 + 文本相似都满足才算覆盖）。
    for (const auto& cue : cues) {
        if (cue.text.empty()) continue;
        const std::int64_t dur = std::max<std::int64_t>(cue.end_ms - cue.start_ms, 1);
        bool covered = false;
        for (const auto* sp : considered) {
            const auto& s = *sp;
            if (s.start_ms > cue.end_ms) break;
            if (s.end_ms < cue.start_ms) continue;
            const auto lo = std::max(s.start_ms, cue.start_ms);
            const auto hi = std::min(s.end_ms, cue.end_ms);
            if (hi <= lo) continue;
            if (static_cast<double>(hi - lo) / static_cast<double>(dur) < min_overlap) continue;
            if (similarity(display_text(s), cue.text) < 0.55) continue;
            covered = true;
            break;
        }
        if (!covered) {
            ++report.missing_in_ours;
            if (static_cast<int>(report.issues.size()) < max_issues) {
                report.issues.push_back(
                    {0, cue.start_ms, cue.end_ms, "", cue.text, "uncovered"});
            }
        }
    }

    return report;
}

std::string ref_diff_html(const RefDiffReport& report, const std::string& title) {
    std::ostringstream os;
    os << "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n<meta charset=\"utf-8\">\n<title>"
       << escape_html(title) << "</title>\n<style>\n"
          "body{font-family:-apple-system,'Segoe UI',Roboto,'Noto Sans SC',sans-serif;"
          "margin:2rem auto;max-width:1200px;padding:0 1rem;color:#1f2328;background:#fff}\n"
          "h1{font-size:1.4rem;margin:0 0 .25rem}.sub{color:#57606a;margin:0 0 1.2rem}\n"
          ".cards{display:flex;flex-wrap:wrap;gap:.75rem;margin:1rem 0}\n"
          ".card{border:1px solid #d0d7de;border-radius:10px;padding:.6rem .9rem;min-width:120px}\n"
          ".card .k{color:#57606a;font-size:.78rem}.card .v{font-size:1.4rem;font-weight:600}\n"
          "table{border-collapse:collapse;width:100%;font-size:.88rem}\n"
          "th,td{border:1px solid #d0d7de;padding:.35rem .5rem;text-align:left;vertical-align:top}\n"
          "th{background:#f6f8fa}.mono{font-family:ui-monospace,Consolas,monospace}\n"
          ".warn{color:#9a6700;font-weight:600}\n"
          "</style>\n</head>\n<body>\n";
    os << "<h1>" << escape_html(title) << "</h1>\n";
    os << "<p class=\"sub\">参考字幕 " << report.reference_cues << " 条；当前字幕 "
       << report.sentences << " 句；按时间重叠配对。</p>\n";

    auto card = [&](const std::string& k, const std::string& v) {
        os << "<div class=\"card\"><div class=\"k\">" << escape_html(k) << "</div><div class=\"v\">"
           << escape_html(v) << "</div></div>\n";
    };
    os << "<div class=\"cards\">\n";
    card("配对成功", std::to_string(report.matched));
    card("参考有、我们缺", std::to_string(report.missing_in_ours));
    card("我们有、参考无", std::to_string(report.missing_in_reference));
    card("长度异常", std::to_string(report.length_mismatch));
    os << "</div>\n";

    os << "<table><tr><th>时间</th><th>类型</th><th>我们的字幕</th><th>参考字幕</th></tr>\n";
    for (const auto& p : report.issues) {
        os << "<tr><td class=\"mono\">" << mmss(p.start_ms) << "–" << mmss(p.end_ms)
           << "</td><td class=\"warn\">" << escape_html(p.kind) << "</td><td>"
           << escape_html(p.ours) << "</td><td>" << escape_html(p.reference) << "</td></tr>\n";
    }
    if (report.issues.empty()) os << "<tr><td colspan=\"4\">没有发现差异。</td></tr>\n";
    os << "</table>\n</body>\n</html>\n";
    return os.str();
}

} // namespace iris::text
