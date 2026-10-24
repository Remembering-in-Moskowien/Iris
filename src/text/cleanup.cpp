#include "iris/text/cleanup.hpp"

#include <algorithm>
#include <cstddef>

namespace iris::text {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// 该字节是否属于多字节 UTF-8 字符（>= 0x80）。
bool is_utf8_byte(char c) { return static_cast<unsigned char>(c) >= 0x80; }

/// UTF-8 字符长度（非法首字节按 1 处理）。
std::size_t utf8_len(unsigned char c) {
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/// 中文语境下的半角 → 全角标点。
const char* fullwidth_for(char c) {
    switch (c) {
        case ',': return "，";
        case ';': return "；";
        case ':': return "：";
        case '!': return "！";
        case '?': return "？";
        default: return nullptr;
    }
}

/// 中文句末/句中标点（用于合并重复）。
bool is_cjk_punct(const std::string& s) {
    static const char* marks[] = {"。", "，", "！", "？", "；", "："};
    for (const char* m : marks) {
        if (s == m) return true;
    }
    return false;
}

} // namespace

std::string clean_text(const std::string& text, CleanupReport* report) {
    CleanupReport local;
    CleanupReport& rep = report != nullptr ? *report : local;

    const std::string original = text;

    // 1) 汉字旁的 ASCII 空格去掉；连续空白压成一个
    std::string no_space;
    no_space.reserve(text.size());
    bool prev_space = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!is_space(c)) {
            no_space.push_back(c);
            prev_space = false;
            continue;
        }
        // 只在「两侧都是中文」时删空格：数字/英文与中文之间的空格是有意义的
        const bool left_cjk = i > 0 && is_utf8_byte(text[i - 1]);
        const bool right_cjk = i + 1 < text.size() && is_utf8_byte(text[i + 1]);
        if (left_cjk && right_cjk) continue;
        if (prev_space) continue;
        no_space.push_back(' ');
        prev_space = true;
    }

    // 2) 半角标点转全角（仅当整句含中文时）
    const bool has_cjk = std::any_of(no_space.begin(), no_space.end(),
                                     [](char c) { return is_utf8_byte(c); });
    std::string punct_fixed;
    punct_fixed.reserve(no_space.size() + 8);
    for (std::size_t i = 0; i < no_space.size(); ++i) {
        const char c = no_space[i];
        const char* fw = has_cjk ? fullwidth_for(c) : nullptr;
        if (fw != nullptr) {
            // 数字里的逗号/冒号（如 1,000 或 12:30）保持半角
            const bool digits_around =
                i > 0 && i + 1 < no_space.size() && std::isdigit(static_cast<unsigned char>(no_space[i - 1])) &&
                std::isdigit(static_cast<unsigned char>(no_space[i + 1]));
            if (digits_around) {
                punct_fixed.push_back(c);
            } else {
                punct_fixed += fw;
            }
            continue;
        }
        punct_fixed.push_back(c);
    }

    // 3) 合并重复标点：连续的中文标点只保留一个，句末标点（。！？）优先。
    std::string collapsed;
    collapsed.reserve(punct_fixed.size());
    std::string pending_punct; // 尚未写入的连续中文标点
    const auto is_final_punct = [](const std::string& s) {
        return s == "。" || s == "！" || s == "？";
    };

    for (std::size_t i = 0; i < punct_fixed.size();) {
        const auto len = utf8_len(static_cast<unsigned char>(punct_fixed[i]));
        if (len == 1 || i + len > punct_fixed.size()) {
            const char c = punct_fixed[i];
            // ASCII 标点：连续重复只留一个
            if (!pending_punct.empty()) {
                collapsed += pending_punct;
                pending_punct.clear();
            }
            if (!collapsed.empty() && collapsed.back() == c &&
                std::string(".!?,;:").find(c) != std::string::npos) {
                ++rep.punctuation_fixed;
                ++i;
                continue;
            }
            collapsed.push_back(c);
            ++i;
            continue;
        }

        const std::string ch = punct_fixed.substr(i, len);
        if (is_cjk_punct(ch)) {
            if (pending_punct.empty()) {
                pending_punct = ch;
            } else {
                // 连续标点只留一个：句末标点胜出，且后面的句末标点覆盖前面的
                if (is_final_punct(ch)) {
                    pending_punct = ch;
                }
                ++rep.punctuation_fixed;
            }
        } else {
            if (!pending_punct.empty()) {
                collapsed += pending_punct;
                pending_punct.clear();
            }
            collapsed += ch;
        }
        i += len;
    }
    if (!pending_punct.empty()) collapsed += pending_punct;

    // 4) 去首尾空白
    std::size_t begin = 0;
    while (begin < collapsed.size() && is_space(collapsed[begin])) ++begin;
    std::size_t end = collapsed.size();
    while (end > begin && is_space(collapsed[end - 1])) --end;
    std::string result = collapsed.substr(begin, end - begin);

    if (result != original) ++rep.whitespace_fixed;
    return result;
}

void clean_sentences(std::vector<model::Sentence>& sentences, CleanupReport& report) {
    std::vector<model::Sentence> out;
    out.reserve(sentences.size());
    std::int64_t prev_end = -1;

    for (auto& s : sentences) {
        ++report.sentences_in;
        s.text = clean_text(s.text, &report);
        if (s.text.empty()) {
            ++report.dropped;
            continue;
        }
        if (s.end_ms <= s.start_ms) {
            s.end_ms = s.start_ms + 1;
            ++report.timing_fixed;
        }
        if (prev_end >= 0 && s.start_ms < prev_end) {
            s.start_ms = prev_end;
            ++report.overlap_fixed;
            if (s.end_ms <= s.start_ms) {
                ++report.dropped;
                continue;
            }
        }
        prev_end = s.end_ms;
        out.push_back(std::move(s));
    }
    sentences = std::move(out);
}

CleanupReport correct_document(model::IrDocument& doc) {
    CleanupReport report;
    for (auto& track : doc.state.tracks) {
        clean_sentences(track.sentences, report);
    }
    return report;
}

} // namespace iris::text
