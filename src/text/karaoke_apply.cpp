#include "iris/text/karaoke_apply.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "iris/text/karaoke.hpp"

namespace iris::text {

namespace {

std::int64_t line_cs(const model::Sentence& s) {
    return (s.end_ms - s.start_ms + 5) / 10;
}

bool time_matches(const model::Sentence& a, const model::Sentence& b, int tolerance_ms) {
    return std::llabs(a.start_ms - b.start_ms) <= tolerance_ms &&
           std::llabs(a.end_ms - b.end_ms) <= tolerance_ms;
}

bool karaoke_from_words(const model::Sentence& s, std::string& out, int target_cs) {
    std::vector<TimedToken> tokens;
    for (const auto& w : s.words) {
        if (w.text.empty() || w.end <= w.start) continue;
        tokens.push_back({w.text, w.start, w.end});
    }
    std::string raw = karaoke_from_timed_tokens(
        tokens, static_cast<double>(s.start_ms) / 1000.0, target_cs);
    if (raw.empty()) return false;
    out = std::move(raw);
    return true;
}

bool role_matches(const model::Sentence& s, const std::string& only_role) {
    if (only_role.empty()) return true;
    return s.role.has_value() && *s.role == only_role;
}

bool needs_translation(const model::Sentence& s, bool overwrite) {
    if (s.comment || !s.raw || !has_karaoke(*s.raw)) return false;
    if (!s.translated_text || s.translated_text->empty()) return false;
    if (s.translated_raw && !s.translated_raw->empty() && !overwrite) return false;
    return true;
}

} // namespace

void apply_karaoke(model::IrDocument& doc, const KaraokeApplyOptions& options,
                   KaraokeApplyStats& stats) {
    for (auto& track : doc.state.tracks) {
        // 0) 已有词级时间（align 产物）→ 直接生成 \K。
        for (auto& s : track.sentences) {
            if (s.comment || s.words.empty() || !role_matches(s, options.only_role)) continue;
            if (s.raw && has_karaoke(*s.raw)) continue;
            std::string raw;
            if (karaoke_from_words(s, raw, static_cast<int>(line_cs(s)))) {
                s.raw = std::move(raw);
                ++stats.from_words;
            }
        }

        // 1) 同一句自带源节奏（英文原文 \K）+ 译文 → 投影出译文的 \K。
        for (auto& s : track.sentences) {
            if (!role_matches(s, options.only_role)) continue;
            if (!needs_translation(s, options.overwrite)) continue;
            KaraokeResult result;
            std::string error;
            if (project_translation(*s.raw, *s.translated_text, static_cast<int>(line_cs(s)),
                                    result, &error)) {
                s.translated_raw = std::move(result.raw);
                ++stats.translated;
            } else {
                ++stats.skipped;
            }
        }

        // 2) 同文件里双语分轨（chi / eng 是两条事件）：用同码的英文行给中文行打轴。
        //    只把「本轮之前就已有 \K」的行当参照，避免把刚生成的结果再投影出去。
        std::vector<const model::Sentence*> sources;
        for (const auto& s : track.sentences) {
            if (!s.comment && s.raw && has_karaoke(*s.raw)) sources.push_back(&s);
        }
        for (auto& s : track.sentences) {
            if (s.comment || s.text.empty()) continue;
            if (!role_matches(s, options.only_role)) continue;
            if (s.raw && has_karaoke(*s.raw)) continue;
            const model::Sentence* source = nullptr;
            for (const auto* other : sources) {
                if (other == &s || other->comment) continue;
                if (!time_matches(s, *other, options.tolerance_ms)) continue;
                source = other;
                break;
            }
            KaraokeResult result;
            std::string error;
            if (source != nullptr &&
                project_translation(*source->raw, s.text, static_cast<int>(line_cs(s)), result,
                                    &error)) {
                s.raw = std::move(result.raw);
                ++stats.projected;
            } else if (options.fill_even && line_cs(s) > 0) {
                result = options.even_syllables
                             ? distribute_even_weighted(s.text, static_cast<int>(line_cs(s)))
                             : distribute_even(s.text, static_cast<int>(line_cs(s)));
                if (!result.raw.empty()) {
                    s.raw = std::move(result.raw);
                    ++stats.even;
                } else {
                    ++stats.skipped;
                }
            } else {
                ++stats.skipped;
            }
        }

        // 3) 兜底生成 \K 之后再投影一次译文：上一步刚给英文歌词打完轴，
        //    译文行在第一轮里还没有源节奏可投影。
        for (auto& s : track.sentences) {
            if (!role_matches(s, options.only_role)) continue;
            if (!needs_translation(s, options.overwrite)) continue;
            KaraokeResult result;
            std::string error;
            if (project_translation(*s.raw, *s.translated_text, static_cast<int>(line_cs(s)),
                                    result, &error)) {
                s.translated_raw = std::move(result.raw);
                ++stats.translated;
            } else {
                ++stats.skipped;
            }
        }
    }
}

} // namespace iris::text
