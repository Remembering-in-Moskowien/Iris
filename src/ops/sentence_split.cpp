#include "iris/ops/sentence_split.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"

namespace iris::ops {

// 简易 rule 策略：按标点切分长句（对应 C# SentenceSplitStrategy.Rule）。
namespace {

// 常见句末标点（含 UTF-8 中文标点）。
const std::vector<std::string> kBoundaryMarks = {"。", "！", "？", ".", "!", "?"};

// 判断 cur 的最后一个字符是否为句末标点（支持多字节 UTF-8）。
bool ends_with_boundary(const std::string& cur) {
    for (const auto& m : kBoundaryMarks) {
        if (cur.size() >= m.size() &&
            cur.compare(cur.size() - m.size(), m.size(), m) == 0) {
            return true;
        }
    }
    return false;
}

// 在 text 的标点边界处切分；超长句按 max_length（字符数）硬切。
// 必须按 UTF-8 字符推进：按字节切会把汉字劈成两半，产出非法 UTF-8。
std::vector<std::string> split_text(const std::string& text, std::size_t max_length) {
    std::vector<std::string> parts;
    std::string cur;
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > text.size()) len = 1; // 末尾残缺序列：按单字节复制
        cur.append(text, i, len);
        i += len;
        if (++chars >= max_length || ends_with_boundary(cur)) {
            parts.push_back(cur);
            cur.clear();
            chars = 0;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

} // namespace

bool SentenceSplit::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;
    const auto max_len = static_cast<std::size_t>(ctx.doc->config.max_sentence_length);

    for (auto& track : st.tracks) {
        std::vector<model::Sentence> out;
        out.reserve(track.sentences.size() * 2);
        for (const auto& s : track.sentences) {
            if (s.text.empty()) continue;
            const auto parts = split_text(s.text, max_len);
            if (parts.size() <= 1) {
                out.push_back(s);
                continue;
            }
            // 按时长比例分配子句时间轴。
            const auto dur = std::max<std::int64_t>(s.end_ms - s.start_ms, 1);
            for (std::size_t i = 0; i < parts.size(); ++i) {
                model::Sentence ns = s;
                ns.text = parts[i];
                const auto frac_start = static_cast<std::int64_t>(
                    static_cast<double>(i) * dur / static_cast<double>(parts.size()));
                const auto frac_end = static_cast<std::int64_t>(
                    static_cast<double>(i + 1) * dur / static_cast<double>(parts.size()));
                ns.start_ms = s.start_ms + frac_start;
                ns.end_ms = s.start_ms + frac_end;
                ns.words.clear();
                out.push_back(std::move(ns));
            }
        }
        track.sentences = std::move(out);
    }
    ctx.record_provenance(name());
    spdlog::info("sentence_split: 完成（{} 个轨道）", st.tracks.size());
    return true;
}

} // namespace iris::ops
