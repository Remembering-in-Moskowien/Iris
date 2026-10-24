#include "iris/subtitle/combine.hpp"

#include <algorithm>
#include <string>

namespace iris::subtitle {

bool merge_cues(std::vector<Cue>& cues, std::int64_t tolerance_ms, CombineStats& stats,
                std::string& error) {
    if (cues.empty()) {
        error = "没有可合并的字幕";
        return false;
    }
    stats.cues_in += static_cast<int>(cues.size());

    std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) {
        if (a.start_ms != b.start_ms) return a.start_ms < b.start_ms;
        return a.end_ms < b.end_ms;
    });

    std::vector<Cue> merged;
    merged.reserve(cues.size());
    for (auto& cue : cues) {
        if (cue.end_ms <= cue.start_ms) cue.end_ms = cue.start_ms + 1;
        if (!merged.empty()) {
            const Cue& prev = merged.back();
            const bool same_text = prev.text == cue.text;
            const bool near_time = std::llabs(cue.start_ms - prev.start_ms) <= tolerance_ms &&
                                   std::llabs(cue.end_ms - prev.end_ms) <= tolerance_ms;
            if (same_text && near_time) {
                ++stats.duplicates;
                continue;
            }
        }
        merged.push_back(std::move(cue));
    }

    stats.cues_out += static_cast<int>(merged.size());
    cues = std::move(merged);
    return true;
}

void align_translations(std::vector<Cue>& primary, const std::vector<Cue>& second,
                        double overlap_ratio, CombineStats& stats) {
    if (second.empty()) return;
    for (auto& cue : primary) {
        const std::int64_t dur = std::max<std::int64_t>(cue.end_ms - cue.start_ms, 1);
        std::int64_t best_overlap = 0;
        const Cue* best = nullptr;
        for (const auto& other : second) {
            const auto lo = std::max(cue.start_ms, other.start_ms);
            const auto hi = std::min(cue.end_ms, other.end_ms);
            const auto overlap = hi - lo;
            if (overlap <= 0) continue;
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best = &other;
            }
        }
        if (best != nullptr &&
            static_cast<double>(best_overlap) / static_cast<double>(dur) >= overlap_ratio) {
            cue.translation = best->text;
            ++stats.paired;
        } else {
            ++stats.unpaired;
        }
    }
}

} // namespace iris::subtitle
