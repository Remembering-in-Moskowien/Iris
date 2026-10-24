#include "iris/script/align.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "iris/infra/log.hpp"

namespace iris::script {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// 行首说话人标签：`名字:` / `名字：`（名字不含空格与标点，长度 ≤ 32）。
bool split_speaker(const std::string& line, std::string& speaker, std::string& text) {
    // 注意：不能用 find_first_of(":：")——它按字节匹配，会把「三」里的 0xBC
    // 当成全角冒号的一部分。必须整串查找。
    std::size_t colon = line.find(':');
    std::size_t colon_len = 1;
    const auto full = line.find("："); // 全角冒号占 3 字节，必须整段跳过
    if (full != std::string::npos && (colon == std::string::npos || full < colon)) {
        colon = full;
        colon_len = 3;
    }
    if (colon == std::string::npos || colon == 0 || colon > 32) return false;
    const std::string name = trim(line.substr(0, colon));
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size();) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x80 && !std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
    }
    const std::string rest = trim(line.substr(colon + colon_len));
    if (rest.empty()) return false;
    speaker = name;
    text = rest;
    return true;
}

/// UTF-8 字符数（用于插值时的长度权重）。
std::size_t char_count(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
        ++n;
    }
    return std::max<std::size_t>(n, 1);
}

} // namespace

std::vector<ScriptLine> load_script(const std::string& path, bool strip_speaker,
                                    std::string& error) {
    std::vector<ScriptLine> lines;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开剧本文件：" + path;
        return lines;
    }
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // 去 BOM
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }

    std::string current;
    const auto flush = [&] {
        const std::string line = trim(current);
        current.clear();
        if (line.empty()) return;
        ScriptLine sl;
        if (strip_speaker && split_speaker(line, sl.speaker, sl.text)) {
            lines.push_back(std::move(sl));
        } else {
            sl.text = line;
            lines.push_back(std::move(sl));
        }
    };
    for (char c : raw) {
        if (c == '\r') continue;
        if (c == '\n') {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();

    if (lines.empty()) error = "剧本里没有任何非空行：" + path;
    return lines;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string word;
    const auto flush_word = [&] {
        if (word.empty()) return;
        tokens.push_back(word);
        word.clear();
    };

    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > text.size()) len = 1;

        if (len > 1) {
            // 中文/全角：每个字单独成词元
            flush_word();
            tokens.push_back(text.substr(i, len));
        } else if (std::isalnum(c)) {
            word.push_back(static_cast<char>(std::tolower(c)));
        } else {
            flush_word(); // 标点/空白都是分隔
        }
        i += len;
    }
    flush_word();
    return tokens;
}

double similarity(const std::string& a, const std::string& b) {
    const auto ta = tokenize(a);
    const auto tb = tokenize(b);
    if (ta.empty() || tb.empty()) return 0.0;
    std::unordered_set<std::string> set_b(tb.begin(), tb.end());
    std::size_t common = 0;
    for (const auto& t : ta) {
        if (set_b.count(t) != 0) ++common;
    }
    return 2.0 * static_cast<double>(common) / static_cast<double>(ta.size() + tb.size());
}

std::vector<AlignedLine> align(const std::vector<ScriptLine>& lines,
                               const std::vector<model::Sentence>& asr_sentences,
                               std::int64_t total_ms) {
    std::vector<AlignedLine> out;
    out.reserve(lines.size());
    for (const auto& line : lines) {
        AlignedLine a;
        a.line = line;
        out.push_back(std::move(a));
    }
    if (out.empty()) return out;

    // ---- 1) Needleman-Wunsch 全局对齐（脚本行 × ASR 句）----
    const std::size_t n = lines.size();
    const std::size_t m = asr_sentences.size();
    constexpr double kGap = -0.35;

    if (m > 0) {
        std::vector<std::vector<double>> score(n + 1, std::vector<double>(m + 1, 0.0));
        for (std::size_t i = 1; i <= n; ++i) score[i][0] = score[i - 1][0] + kGap;
        for (std::size_t j = 1; j <= m; ++j) score[0][j] = score[0][j - 1] + kGap;

        std::vector<std::vector<double>> sim(n, std::vector<double>(m, 0.0));
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < m; ++j) {
                sim[i][j] = similarity(lines[i].text, asr_sentences[j].text);
            }
        }

        for (std::size_t i = 1; i <= n; ++i) {
            for (std::size_t j = 1; j <= m; ++j) {
                const double diag = score[i - 1][j - 1] + (sim[i - 1][j - 1] - 0.25);
                const double up = score[i - 1][j] + kGap;
                const double left = score[i][j - 1] + kGap;
                score[i][j] = std::max({diag, up, left});
            }
        }

        // 回溯：只接受相似度足够高的对角线作为锚点
        std::size_t i = n;
        std::size_t j = m;
        while (i > 0 && j > 0) {
            const double diag = score[i - 1][j - 1] + (sim[i - 1][j - 1] - 0.25);
            if (std::abs(score[i][j] - diag) < 1e-9) {
                if (sim[i - 1][j - 1] >= 0.34) {
                    out[i - 1].start_ms = asr_sentences[j - 1].start_ms;
                    out[i - 1].end_ms = asr_sentences[j - 1].end_ms;
                    out[i - 1].matched_index = static_cast<int>(j - 1);
                }
                --i;
                --j;
            } else if (i > 0 && std::abs(score[i][j] - (score[i - 1][j] + kGap)) < 1e-9) {
                --i;
            } else {
                --j;
            }
        }
    }

    // ---- 2) 未匹配的行在锚点之间按字符数插值 ----
    std::int64_t cursor = 0; // 上一个锚点的结束时间
    std::size_t i = 0;
    while (i < n) {
        if (out[i].matched_index >= 0) {
            cursor = out[i].end_ms;
            ++i;
            continue;
        }
        // 找 [i, run_end) 这一段连续未匹配
        std::size_t run_end = i;
        while (run_end < n && out[run_end].matched_index < 0) ++run_end;

        const std::int64_t next_start =
            (run_end < n) ? out[run_end].start_ms : std::max<std::int64_t>(cursor + 1, total_ms);
        const std::int64_t span = std::max<std::int64_t>(next_start - cursor, 0);

        std::size_t weight_sum = 0;
        for (std::size_t k = i; k < run_end; ++k) weight_sum += char_count(out[k].line.text);

        std::int64_t acc = cursor;
        for (std::size_t k = i; k < run_end; ++k) {
            const auto w = char_count(out[k].line.text);
            const auto share =
                span > 0 && weight_sum > 0
                    ? static_cast<std::int64_t>(std::llround(static_cast<double>(span) * w /
                                                             static_cast<double>(weight_sum)))
                    : 0;
            out[k].start_ms = acc;
            out[k].end_ms = acc + share;
            acc += share;
        }
        if (run_end < n) {
            // 让最后一条未匹配贴着下一个锚点
            out[run_end - 1].end_ms = std::max(out[run_end - 1].end_ms, next_start);
        }
        cursor = run_end > 0 ? out[run_end - 1].end_ms : cursor;
        i = run_end;
    }

    // ---- 2b) 头部/尾部未匹配：与最近锚点共享它自己的时间窗 ----
    // （否则首尾的未匹配行只能拿到 0 毫秒，实际会变成 1 ms 的碎片）
    // run_before = true 表示未匹配段在锚点之前（先分给未匹配段），否则锚点在前。
    const auto split_window_with = [&](std::size_t anchor_index, std::size_t run_begin,
                                       std::size_t run_end, bool run_before) {
        if (run_begin >= run_end) return;
        std::size_t weight = char_count(out[anchor_index].line.text);
        for (std::size_t k = run_begin; k < run_end; ++k) weight += char_count(out[k].line.text);
        const auto start = out[anchor_index].start_ms;
        const auto span = out[anchor_index].end_ms - start;
        if (span <= 0 || weight == 0) return;

        std::int64_t acc = start;
        if (!run_before) {
            // 锚点在前：先分给锚点
            const auto anchor_share = static_cast<std::int64_t>(
                std::llround(static_cast<double>(span) * char_count(out[anchor_index].line.text) /
                             static_cast<double>(weight)));
            out[anchor_index].start_ms = acc;
            out[anchor_index].end_ms = acc + std::max<std::int64_t>(anchor_share, 1);
            acc = out[anchor_index].end_ms;
        }
        for (std::size_t k = run_begin; k < run_end; ++k) {
            const auto share = static_cast<std::int64_t>(
                std::llround(static_cast<double>(span) * char_count(out[k].line.text) /
                             static_cast<double>(weight)));
            out[k].start_ms = acc;
            out[k].end_ms = acc + std::max<std::int64_t>(share, 1);
            acc = out[k].end_ms;
        }
        if (run_before) {
            // 未匹配段在前：锚点拿剩下的窗口
            out[anchor_index].start_ms = acc;
            out[anchor_index].end_ms = start + span;
            if (out[anchor_index].end_ms <= out[anchor_index].start_ms) {
                out[anchor_index].end_ms = out[anchor_index].start_ms + 1;
            }
        }
    };

    for (std::size_t k = 0; k < n; ++k) {
        if (out[k].matched_index < 0) continue;
        if (k > 0 && out[k - 1].matched_index < 0) {
            std::size_t begin = k - 1;
            while (begin > 0 && out[begin - 1].matched_index < 0) --begin;
            split_window_with(k, begin, k, /*run_before=*/true);
        }
    }
    for (std::size_t k = n; k > 0; --k) {
        const std::size_t idx = k - 1;
        if (out[idx].matched_index < 0) continue;
        if (idx + 1 < n && out[idx + 1].matched_index < 0) {
            std::size_t end = idx + 1;
            while (end < n && out[end].matched_index < 0) ++end;
            split_window_with(idx, idx + 1, end, /*run_before=*/false);
            break;
        }
    }

    // ---- 3) 一个锚点都没有：按全片时长与字符数铺开 ----
    const bool any_matched =
        std::any_of(out.begin(), out.end(), [](const AlignedLine& a) { return a.matched_index >= 0; });
    if (!any_matched) {
        const std::int64_t total = std::max<std::int64_t>(total_ms, static_cast<std::int64_t>(n) * 1000);
        std::size_t weight_sum = 0;
        for (const auto& a : out) weight_sum += char_count(a.line.text);
        std::int64_t acc = 0;
        for (auto& a : out) {
            const auto w = char_count(a.line.text);
            const auto share = weight_sum > 0 ? static_cast<std::int64_t>(std::llround(
                                                    static_cast<double>(total) * w /
                                                    static_cast<double>(weight_sum)))
                                              : 0;
            a.start_ms = acc;
            a.end_ms = acc + std::max<std::int64_t>(share, 1);
            acc = a.end_ms;
        }
    }

    // ---- 4) 兜底：零长度/反序修正 ----
    for (auto& a : out) {
        if (a.end_ms <= a.start_ms) a.end_ms = a.start_ms + 1;
    }
    return out;
}

model::Track to_track(const std::vector<AlignedLine>& aligned, const std::string& language) {
    model::Track track;
    track.type = "transcribe";
    track.language = language;
    track.sentences.reserve(aligned.size());
    for (const auto& a : aligned) {
        model::Sentence s;
        s.start_ms = a.start_ms;
        s.end_ms = a.end_ms;
        s.text = a.line.text;
        if (!a.line.speaker.empty()) s.speaker = a.line.speaker;
        track.sentences.push_back(std::move(s));
    }
    return track;
}

} // namespace iris::script
