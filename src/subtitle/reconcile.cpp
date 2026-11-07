#include "iris/subtitle/reconcile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "iris/infra/paths.hpp"

namespace iris::subtitle {

namespace {

constexpr std::int64_t kMinDurationMs = 1;

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool pseudo_blank(const std::string& text) {
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) return false;
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80) return false;
    }
    return true;
}

std::string strip_outer_dialogue_dashes(std::string text) {
    text = trim(text);
    while (!text.empty() && (text.front() == '-' || text.front() == '\xE2')) {
        if (text.front() == '\xE2' && text.compare(0, 3, "\xE2\x80\x94") != 0 &&
            text.compare(0, 3, "\xE2\x80\x93") != 0) {
            break;
        }
        if (text.front() == '\xE2') {
            text.erase(0, 3);
        } else {
            text.erase(0, 1);
        }
        text = trim(text);
    }
    while (!text.empty()) {
        if (text.back() == '-' ) {
            text.pop_back();
        } else if (text.size() >= 3 && text.compare(text.size() - 3, 3, "\xE2\x80\x94") == 0) {
            text.erase(text.size() - 3);
        } else if (text.size() >= 3 && text.compare(text.size() - 3, 3, "\xE2\x80\x93") == 0) {
            text.erase(text.size() - 3);
        } else {
            break;
        }
        text = trim(text);
    }
    return text;
}

std::string normalize_stutter(std::string text) {
    const std::size_t end = text.find_first_of(" \t,.;:!?");
    const std::size_t token_end = end == std::string::npos ? text.size() : end;
    const std::string token = text.substr(0, token_end);
    if (token.size() < 5) return text;

    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin < token.size()) {
        const auto dash = token.find('-', begin);
        parts.push_back(token.substr(begin, dash == std::string::npos ? std::string::npos
                                                                       : dash - begin));
        if (dash == std::string::npos) break;
        begin = dash + 1;
    }
    if (parts.size() < 2 || parts.back().empty()) return text;
    const char initial = static_cast<char>(
        std::tolower(static_cast<unsigned char>(parts.back().front())));
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (parts[i].size() != 1 ||
            static_cast<char>(std::tolower(static_cast<unsigned char>(parts[i].front()))) !=
                initial) {
            return text;
        }
    }
    return parts.back() + text.substr(token_end);
}

std::string strip_switch_prefix(std::string text) {
    text = trim(text);
    for (;;) {
        if (text.rfind("- -", 0) == 0) {
            text = trim(text.substr(3));
        } else if (text.rfind("- ", 0) == 0) {
            text = trim(text.substr(2));
        } else if (text.rfind("\xE2\x80\x94", 0) == 0) {
            text = trim(text.substr(3));
        } else {
            break;
        }
    }
    return text;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool is_cjk_cp(unsigned cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

/// 逐个 UTF-8 字符回调：返回字符占据的字节数与码点。
template <typename Fn>
void for_each_cp(const std::string& s, Fn&& fn) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((b & 0xE0) == 0xC0) len = 2;
        else if ((b & 0xF0) == 0xE0) len = 3;
        else if ((b & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = 1;
        unsigned cp = b;
        if (len == 3) {
            cp = ((b & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        } else if (len == 2) {
            cp = ((b & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
        }
        fn(i, len, cp);
        i += len;
    }
}

/// 术语表键：小写、空白压成一个空格、去首尾空白。
std::string glossary_key(const std::string& text) {
    std::string out;
    bool space = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::int64_t overlap_ms(std::int64_t a0, std::int64_t a1, std::int64_t b0, std::int64_t b1) {
    const std::int64_t lo = std::max(a0, b0);
    const std::int64_t hi = std::min(a1, b1);
    return hi > lo ? hi - lo : 0;
}

const std::string& policy_for(CueRole role, const ReconcileOptions& options) {
    switch (role) {
        case CueRole::sdh:
        case CueRole::annotation: return options.policy.sdh;
        case CueRole::lyric: return options.policy.lyric;
        case CueRole::interjection: return options.policy.interjection;
        case CueRole::dialogue: break;
    }
    return options.policy.dialogue;
}

const char* style_for(CueRole role) {
    switch (role) {
        case CueRole::lyric: return "eng-lyric";
        case CueRole::sdh:
        case CueRole::annotation: return "eng-sdh";
        default: return "eng";
    }
}

/// 在空格/标点处按比例切开一条译文。ratio = 第一段应占的字数比例。
bool split_text_at(const std::string& text, double ratio, int min_chars, std::string& left,
                   std::string& right) {
    const int total = visible_length(text);
    if (total < min_chars * 2) return false;

    struct Candidate {
        std::size_t pos = 0; ///< 切点（字节偏移，归入右段）
        int left_len = 0;
        bool boundary = false;
    };
    std::vector<Candidate> candidates;

    int consumed = 0;
    for_each_cp(text, [&](std::size_t i, std::size_t len, unsigned cp) {
        const std::size_t after = i + len;
        const bool is_space = cp == ' ' || cp == '\t';
        const bool is_punct = (cp == 0x3002 || cp == 0xFF0C || cp == 0xFF01 || cp == 0xFF1F ||
                               cp == 0x3001 || cp == 0xFF1B || cp == 0xFF1A || cp == ',' ||
                               cp == '.' || cp == '!' || cp == '?' || cp == ';' || cp == ':');
        // 标点后再切（ASCII 标点带一个空格时也允许）
        std::size_t pos = after;
        if (is_space && after < text.size()) {
            // 空格本身归左段
            pos = i;
        }
        if (!is_space && !is_punct) {
            ++consumed;
            return;
        }
        const int left_len = visible_length(text.substr(0, pos));
        if (left_len >= min_chars && total - left_len >= min_chars) {
            candidates.push_back({pos, left_len, true});
        }
        if (is_space) ++consumed;
    });
    (void)consumed;

    if (candidates.empty()) {
        // 没有标点/空格：退化为任意字之间的等距切点
        int idx = 0;
        for_each_cp(text, [&](std::size_t i, std::size_t len, unsigned cp) {
            (void)cp;
            const int left_len = idx + 1;
            if (left_len >= min_chars && total - left_len >= min_chars) {
                candidates.push_back({i + len, left_len, false});
            }
            ++idx;
        });
    }
    if (candidates.empty()) return false;

    const Candidate* best = nullptr;
    double best_score = 1e9;
    for (const auto& c : candidates) {
        const double r = static_cast<double>(c.left_len) / static_cast<double>(total);
        double score = std::abs(r - ratio);
        if (c.boundary) score -= 0.04; // 同分时优先标点/空格
        if (score < best_score) {
            best_score = score;
            best = &c;
        }
    }
    if (best == nullptr) return false;
    // 宁可切得糙一点，也不要让一条英文没有中文：候选位置都保证了每侧 ≥ min_chars。

    left = trim(text.substr(0, best->pos));
    right = trim(text.substr(best->pos));
    return visible_length(left) >= min_chars && visible_length(right) >= min_chars;
}

struct Cue {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::string text;
    std::string speaker; ///< 从 `[Kinger] ...` 里剥出来的说话人；写进 ASS 的 Name 列
    CueRole role = CueRole::dialogue;
    bool dead = false;
    bool claimed = false;
    /// 译文片段（起点 + 文本）；同一主轨可能吃掉多条副轨，拼接前要按时间排序。
    std::vector<std::pair<std::int64_t, std::string>> translations;
};

constexpr std::size_t kMaxAlignmentGroup = 4;
constexpr double kCueSkipCost = 0.8;

struct AlignmentGroup {
    std::size_t primary = 0;
    std::size_t primary_count = 0;
    std::size_t secondary = 0;
    std::size_t secondary_count = 0;
};

std::vector<std::pair<std::int64_t, std::int64_t>> cue_intervals(const std::vector<Cue>& cues,
                                                                 std::size_t start,
                                                                 std::size_t count) {
    std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
    intervals.reserve(count);
    for (std::size_t i = start; i < start + count; ++i) {
        intervals.emplace_back(cues[i].start, cues[i].end);
    }
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> merged;
    for (const auto& interval : intervals) {
        if (merged.empty() || interval.first > merged.back().second) {
            merged.push_back(interval);
        } else {
            merged.back().second = std::max(merged.back().second, interval.second);
        }
    }
    return merged;
}

std::int64_t intervals_length(
    const std::vector<std::pair<std::int64_t, std::int64_t>>& intervals) {
    std::int64_t length = 0;
    for (const auto& interval : intervals) length += interval.second - interval.first;
    return std::max<std::int64_t>(length, 1);
}

std::int64_t covered_duration(const Cue& cue, const std::vector<Cue>& other,
                              std::size_t start, std::size_t count) {
    std::vector<std::pair<std::int64_t, std::int64_t>> intersections;
    intersections.reserve(count);
    for (std::size_t i = start; i < start + count; ++i) {
        const std::int64_t begin = std::max(cue.start, other[i].start);
        const std::int64_t end = std::min(cue.end, other[i].end);
        if (end > begin) intersections.emplace_back(begin, end);
    }
    if (intersections.empty()) return 0;
    std::sort(intersections.begin(), intersections.end());
    std::int64_t covered = 0;
    std::int64_t begin = intersections.front().first;
    std::int64_t end = intersections.front().second;
    for (std::size_t i = 1; i < intersections.size(); ++i) {
        if (intersections[i].first > end) {
            covered += end - begin;
            begin = intersections[i].first;
            end = intersections[i].second;
        } else {
            end = std::max(end, intersections[i].second);
        }
    }
    return covered + end - begin;
}

double alignment_cost(const std::vector<Cue>& primary, std::size_t pi, std::size_t pn,
                      const std::vector<Cue>& secondary, std::size_t si, std::size_t sn) {
    double coverage = 0.0;
    for (std::size_t i = pi; i < pi + pn; ++i) {
        const auto duration = std::max<std::int64_t>(primary[i].end - primary[i].start, 1);
        coverage += static_cast<double>(covered_duration(primary[i], secondary, si, sn)) /
                    static_cast<double>(duration);
    }

    for (std::size_t i = si; i < si + sn; ++i) {
        const auto duration = std::max<std::int64_t>(secondary[i].end - secondary[i].start, 1);
        coverage += static_cast<double>(covered_duration(secondary[i], primary, pi, pn)) /
                    static_cast<double>(duration);
    }
    coverage /= static_cast<double>(pn + sn);

    const auto primary_intervals = cue_intervals(primary, pi, pn);
    const auto secondary_intervals = cue_intervals(secondary, si, sn);
    const auto primary_duration = intervals_length(primary_intervals);
    const auto secondary_duration = intervals_length(secondary_intervals);
    const auto span = std::max(primary_duration, secondary_duration);
    const double endpoint_drift =
        static_cast<double>(std::abs(primary[pi].start - secondary[si].start) +
                            std::abs(primary_intervals.back().second -
                                     secondary_intervals.back().second)) /
        (2.0 * static_cast<double>(std::max<std::int64_t>(span, 1)));
    const double duration_ratio =
        std::abs(std::log(static_cast<double>(primary_duration) /
                          static_cast<double>(secondary_duration)));

    double neighboring_coverage = 0.0;
    const auto consider_primary_neighbor = [&](std::size_t index) {
        if (primary[index].role == CueRole::sdh || !primary[index].translations.empty()) return;
        const auto duration =
            std::max<std::int64_t>(primary[index].end - primary[index].start, 1);
        neighboring_coverage =
            std::max(neighboring_coverage,
                     static_cast<double>(covered_duration(primary[index], secondary, si, sn)) /
                         static_cast<double>(duration));
    };
    const auto consider_secondary_neighbor = [&](std::size_t index) {
        if (secondary[index].role == CueRole::annotation) return;
        const auto duration =
            std::max<std::int64_t>(secondary[index].end - secondary[index].start, 1);
        neighboring_coverage =
            std::max(neighboring_coverage,
                     static_cast<double>(covered_duration(secondary[index], primary, pi, pn)) /
                         static_cast<double>(duration));
    };
    if (pn > 1 || sn > 1) {
        if (pi > 0) consider_primary_neighbor(pi - 1);
        if (pi + pn < primary.size()) consider_primary_neighbor(pi + pn);
        if (si > 0) consider_secondary_neighbor(si - 1);
        if (si + sn < secondary.size()) consider_secondary_neighbor(si + sn);
    }

    return 1.6 * (1.0 - std::min(coverage, 1.0)) + 0.15 * endpoint_drift +
           0.2 * duration_ratio + 0.75 * static_cast<double>(pn + sn - 2) +
           0.6 * neighboring_coverage;
}

double interval_iou(const std::vector<Cue>& primary, std::size_t pi, std::size_t pn,
                    const std::vector<Cue>& secondary, std::size_t si, std::size_t sn) {
    const auto p = cue_intervals(primary, pi, pn);
    const auto s = cue_intervals(secondary, si, sn);
    std::size_t i = 0;
    std::size_t j = 0;
    std::int64_t intersection = 0;
    while (i < p.size() && j < s.size()) {
        intersection += std::max<std::int64_t>(
            0, std::min(p[i].second, s[j].second) - std::max(p[i].first, s[j].first));
        if (p[i].second < s[j].second) ++i;
        else ++j;
    }
    const auto p_len = intervals_length(p);
    const auto s_len = intervals_length(s);
    const auto uni = p_len + s_len - intersection;
    return uni > 0 ? static_cast<double>(intersection) / static_cast<double>(uni) : 0.0;
}

double fallback_semantic_similarity(const std::vector<Cue>& primary, std::size_t pi,
                                    std::size_t pn, const std::vector<Cue>& secondary,
                                    std::size_t si, std::size_t sn) {
    int p_chars = 0;
    int s_chars = 0;
    int p_punct = 0;
    int s_punct = 0;
    for (std::size_t i = pi; i < pi + pn; ++i) {
        p_chars += visible_length(primary[i].text);
        for (const char c : primary[i].text)
            p_punct += std::ispunct(static_cast<unsigned char>(c)) != 0;
    }
    for (std::size_t i = si; i < si + sn; ++i) {
        s_chars += visible_length(secondary[i].text);
        for (const char c : secondary[i].text)
            s_punct += std::ispunct(static_cast<unsigned char>(c)) != 0;
    }
    const double length_score =
        1.0 - std::min(1.0, std::abs(std::log(static_cast<double>(std::max(p_chars, 1)) /
                                               static_cast<double>(std::max(s_chars, 1)))));
    const double punct_score =
        1.0 - std::min(1.0, std::abs(p_punct - s_punct) / 4.0);
    return std::clamp(0.7 * length_score + 0.3 * punct_score, 0.0, 1.0);
}

bool valid_alignment_group(const std::vector<Cue>& primary, std::size_t pi, std::size_t pn,
                           const std::vector<Cue>& secondary, std::size_t si,
                           std::size_t sn) {
    for (std::size_t i = pi; i < pi + pn; ++i) {
        if (primary[i].role == CueRole::sdh || !primary[i].translations.empty()) return false;
    }
    for (std::size_t i = si; i < si + sn; ++i) {
        if (secondary[i].role == CueRole::annotation) return false;
    }
    if (pn == 1) {
        for (std::size_t i = si; i < si + sn; ++i) {
            const auto overlap = overlap_ms(primary[pi].start, primary[pi].end,
                                            secondary[i].start, secondary[i].end);
            const auto shorter_duration =
                std::max<std::int64_t>(1, std::min(primary[pi].end - primary[pi].start,
                                                   secondary[i].end - secondary[i].start));
            if (overlap * 100 < shorter_duration * 45) return false;
        }
        if (sn > 1 && pi + 1 < primary.size()) {
            const auto& trailing = secondary[si + sn - 1];
            const auto trailing_duration =
                std::max<std::int64_t>(1, trailing.end - trailing.start);
            if (overlap_ms(primary[pi + 1].start, primary[pi + 1].end, trailing.start,
                           trailing.end) *
                    100 >=
                trailing_duration * 45) {
                return false;
            }
        }
    } else if (sn == 1) {
        for (std::size_t i = pi; i < pi + pn; ++i) {
            const auto duration = std::max<std::int64_t>(primary[i].end - primary[i].start, 1);
            if (overlap_ms(primary[i].start, primary[i].end, secondary[si].start,
                           secondary[si].end) *
                    10 <
                duration * 6) {
                return false;
            }
        }
    }
    return true;
}

std::vector<AlignmentGroup> align_cues(
    const std::vector<Cue>& primary, const std::vector<Cue>& secondary,
    const ReconcileOptions& options, int& rejected, double& confidence_sum) {
    const std::size_t columns = secondary.size() + 1;
    constexpr std::size_t kCostRows = kMaxAlignmentGroup + 1;
    std::vector<double> costs(kCostRows * columns, std::numeric_limits<double>::infinity());
    std::vector<std::uint8_t> backtrack((primary.size() + 1) * columns, 0);
    const auto group_end_times = [](const std::vector<Cue>& cues) {
        std::vector<std::int64_t> ends(cues.size());
        for (std::size_t i = 0; i < cues.size(); ++i) {
            ends[i] = cues[i].end;
            for (std::size_t k = 1; k < kMaxAlignmentGroup && i + k < cues.size(); ++k) {
                ends[i] = std::max(ends[i], cues[i + k].end);
            }
        }
        return ends;
    };
    const auto primary_group_ends = group_end_times(primary);
    const auto secondary_group_ends = group_end_times(secondary);
    const auto cost_at = [&](std::size_t i, std::size_t j) -> double& {
        return costs[(i % kCostRows) * columns + j];
    };
    const auto action_at = [&](std::size_t i, std::size_t j) -> std::uint8_t& {
        return backtrack[i * columns + j];
    };
    cost_at(0, 0) = 0.0;

    const auto relax = [&](std::size_t from_i, std::size_t from_j, std::size_t to_i,
                           std::size_t to_j, double cost, std::uint8_t action) {
        const double candidate = cost_at(from_i, from_j) + cost;
        double& destination = cost_at(to_i, to_j);
        if (candidate < destination - 1e-9) {
            destination = candidate;
            action_at(to_i, to_j) = action;
        }
    };

    for (std::size_t i = 0; i <= primary.size(); ++i) {
        for (std::size_t j = 0; j <= secondary.size(); ++j) {
            if (!std::isfinite(cost_at(i, j))) continue;
            if (i < primary.size()) {
                const double cost = !primary[i].translations.empty() ? 0.0 : kCueSkipCost;
                relax(i, j, i + 1, j, cost, 1);
            }
            if (j < secondary.size()) {
                const double cost =
                    secondary[j].role == CueRole::annotation ? 0.0 : kCueSkipCost;
                relax(i, j, i, j + 1, cost, 2);
            }
            if (i == primary.size() || j == secondary.size()) continue;
            if (primary[i].start >= secondary_group_ends[j] ||
                secondary[j].start >= primary_group_ends[i]) {
                continue;
            }

            const auto max_primary = std::min(kMaxAlignmentGroup, primary.size() - i);
            const auto max_secondary = std::min(kMaxAlignmentGroup, secondary.size() - j);
            for (std::size_t pn = 1; pn <= max_primary; ++pn) {
                for (std::size_t sn = 1; sn <= max_secondary; ++sn) {
                    if (!valid_alignment_group(primary, i, pn, secondary, j, sn)) continue;
                    const double iou = interval_iou(primary, i, pn, secondary, j, sn);
                    if (iou < options.min_iou) {
                        ++rejected;
                        continue;
                    }
                    double semantic = fallback_semantic_similarity(primary, i, pn, secondary, j, sn);
                    if (options.semantic_similarity) {
                        double sum = 0.0;
                        int count = 0;
                        for (std::size_t pi = i; pi < i + pn; ++pi) {
                            for (std::size_t si = j; si < j + sn; ++si) {
                                sum += std::clamp(options.semantic_similarity(primary[pi].text,
                                                                               secondary[si].text),
                                                  0.0, 1.0);
                                ++count;
                            }
                        }
                        if (count > 0) semantic = sum / static_cast<double>(count);
                    }
                    const double confidence = 0.65 * iou + 0.35 * semantic;
                    if (options.semantic_similarity &&
                        confidence < options.min_alignment_confidence) {
                        ++rejected;
                        continue;
                    }
                    relax(i, j, i + pn, j + sn,
                          alignment_cost(primary, i, pn, secondary, j, sn) +
                              (options.semantic_similarity
                                   ? options.semantic_weight * (1.0 - semantic)
                                   : 0.0),
                          static_cast<std::uint8_t>(3 + (pn - 1) * kMaxAlignmentGroup + sn - 1));
                }
            }
        }
        std::fill_n(costs.begin() + static_cast<std::ptrdiff_t>((i % kCostRows) * columns),
                    columns, std::numeric_limits<double>::infinity());
    }

    std::vector<AlignmentGroup> groups;
    std::size_t i = primary.size();
    std::size_t j = secondary.size();
    while (i > 0 || j > 0) {
        const std::uint8_t action = action_at(i, j);
        if (action == 0) break;
        if (action == 1) {
            --i;
        } else if (action == 2) {
            --j;
        } else {
            const std::size_t encoded = action - 3;
            const std::size_t pn = encoded / kMaxAlignmentGroup + 1;
            const std::size_t sn = encoded % kMaxAlignmentGroup + 1;
            groups.push_back({i - pn, pn, j - sn, sn});
            i -= pn;
            j -= sn;
        }
    }
    std::reverse(groups.begin(), groups.end());
    for (const auto& group : groups) {
        const double iou = interval_iou(primary, group.primary, group.primary_count, secondary,
                                         group.secondary, group.secondary_count);
        double semantic = fallback_semantic_similarity(
            primary, group.primary, group.primary_count, secondary, group.secondary,
            group.secondary_count);
        if (options.semantic_similarity) {
            double sum = 0.0;
            int count = 0;
            for (std::size_t pi = group.primary; pi < group.primary + group.primary_count; ++pi) {
                for (std::size_t si = group.secondary;
                     si < group.secondary + group.secondary_count; ++si) {
                    sum += std::clamp(
                        options.semantic_similarity(primary[pi].text, secondary[si].text), 0.0,
                        1.0);
                    ++count;
                }
            }
            if (count > 0) semantic = sum / static_cast<double>(count);
        }
        confidence_sum += 0.65 * iou + 0.35 * semantic;
    }
    return groups;
}

/// 剥离开头的 `[Kinger]` / `-[Jax]`：说话人单独返回，正文里不再保留标签。
/// 只认 1–2 个首字母大写的词；`[Zooble groans]`、`[music]` 这类音效描述不动。
std::string take_speaker_tag(std::string& text) {
    std::size_t i = 0;
    const auto skip_ws = [&]() {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };
    skip_ws();
    while (i < text.size() && text[i] == '-') {
        ++i;
        skip_ws();
    }
    if (i >= text.size() || text[i] != '[') return {};
    const std::size_t close = text.find(']', i);
    if (close == std::string::npos) return {};
    const std::string name = trim(text.substr(i + 1, close - i - 1));
    if (name.empty() || name.size() > 24) return {};
    std::string lower_name = name;
    for (char& c : lower_name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* kSoundParts[] = {
        "voice", "voices", "echo", "echoing", "doorbell", "rings", "ringing",
        "music", "sound", "noise", "wailing", "whispering", "laughing"};
    for (const char* part : kSoundParts) {
        if (lower_name.find(part) != std::string::npos) return {};
    }

    int words = 0;
    bool new_word = true;
    for (char c : name) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            new_word = true;
            continue;
        }
        if (new_word) {
            if (std::isupper(static_cast<unsigned char>(c)) == 0) return {};
            ++words;
            new_word = false;
        } else if (std::isalpha(static_cast<unsigned char>(c)) == 0 && c != '\'' && c != '.' &&
                   c != '-') {
            return {};
        }
    }
    if (words < 1 || words > 2) return {};

    // `[Whimpers]`、`[Sighs]` 这类是音效描述，不是说话人
    if (words == 1) {
        static const char* kSoundWords[] = {
            "whimpers", "sighs",    "groans",   "screams",  "laughs",   "gasps",    "pants",
            "sobs",     "sniffles", "stammers", "coughs",   "mutters",  "whispers", "shouts",
            "yelps",    "growls",   "grunts",   "shrieks",  "wails",    "cries",    "chuckles",
            "giggles",  "scoffs",   "sniffs",   "exhales",  "inhales",  "breathing", "sobbing",
            "crying",   "laughing", "screaming", "groaning", "panting", "sighing",  "mumbling",
            "humming",  "singing",  "narration", "both",    "everyone", "all",      "crowd",
            "voice",    "voices",   "music",    "static",   "silence",  "thud",     "bang",
            "clang",    "crash",    "splash",   "echo"};
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* w : kSoundWords) {
            if (lower == w) return {};
        }
    }

    std::string rest = trim(text.substr(close + 1));
    if (rest.empty()) return {};
    text = std::move(rest);
    return name;
}

/// 一行多说话人（`-A -B`）：按行首/空格后的 '-' 切成多段，返回不少于 2 段时才拆。
std::vector<std::string> speaker_segments(const std::string& text) {
    std::string s = text;
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    struct Marker {
        std::size_t pos;
        std::size_t end;
    };
    std::vector<Marker> markers;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '-' || (i != 0 && !std::isspace(static_cast<unsigned char>(s[i - 1])))) continue;
        std::size_t end = i + 1;
        if (end < s.size() && s[end] == '[') {
            const auto close = s.find(']', end + 1);
            if (close == std::string::npos) continue;
            std::string label = s.substr(end + 1, close - end - 1);
            for (char& c : label)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            static const char* kSoundParts[] = {
                "voice", "voices", "echo", "echoing", "doorbell", "rings", "ringing",
                "music", "sound", "noise", "wailing", "whispering", "laughing"};
            for (const char* part : kSoundParts) {
                if (label.find(part) != std::string::npos) return {};
            }
            end = close + 1;
        } else {
            const auto begin = end;
            while (end < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_' ||
                    s[end] == '\'')) {
                ++end;
            }
            if (end == begin) {
                while (end < s.size() &&
                       !std::isspace(static_cast<unsigned char>(s[end])) && s[end] != '-') {
                    ++end;
                }
            }
            if (end == begin || (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])) &&
                                 s[end] != '-' && s[end] != '.' && s[end] != '!' &&
                                 s[end] != '?' && s[end] != ',')) {
                continue;
            }
        }
        markers.push_back({i, end});
    }
    if (markers.size() < 2) return {};

    // `-A -B text` is a compact simultaneous-dialogue form. Apply the same
    // text to both speakers until a later forced aligner can provide words.
    if (markers[0].pos == 0 && markers.size() == 2 &&
        trim(s.substr(markers[0].end, markers[1].pos - markers[0].end)).empty() &&
        markers.size() == 2) {
        const std::string body = trim(s.substr(markers[1].end));
        if (!body.empty()) {
            return {trim(s.substr(markers[0].pos, markers[0].end - markers[0].pos) + " " + body),
                    trim(s.substr(markers[1].pos, markers[1].end - markers[1].pos) + " " + body)};
        }
    }

    std::vector<std::string> out;
    out.reserve(markers.size());
    for (std::size_t k = 0; k < markers.size(); ++k) {
        const std::size_t end = (k + 1 < markers.size()) ? markers[k + 1].pos : s.size();
        std::string seg = trim(s.substr(markers[k].pos, end - markers[k].pos));
        if (seg.empty()) return {};
        out.push_back(std::move(seg));
    }
    return out;
}

/// Remove a leading `-[Name]` or `-Name` label and return the speaker.
std::string take_dash_speaker_tag(std::string& text) {
    text = trim(text);
    if (text.empty() || text.front() != '-') return {};
    std::size_t i = 1;
    if (i < text.size() && text[i] == '[') {
        const auto close = text.find(']', i + 1);
        if (close == std::string::npos || close + 1 >= text.size()) return {};
        const std::string name = trim(text.substr(i + 1, close - i - 1));
        std::string candidate = "-[" + name + "] text";
        if (take_speaker_tag(candidate).empty()) return {};
        text = trim(text.substr(close + 1));
        return name;
    }
    const std::size_t begin = i;
    while (i < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
        ++i;
    }
    if (i == begin || i >= text.size() || !std::isspace(static_cast<unsigned char>(text[i]))) return {};
    const std::string name = text.substr(begin, i - begin);
    std::string lower_name = name;
    for (char& c : lower_name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower_name == "voice" || lower_name == "voices" || lower_name == "music" ||
        lower_name == "sound" || lower_name == "noise") {
        return {};
    }
    text = trim(text.substr(i));
    return name;
}

bool looks_like_speaker_label(const std::string& label) {
    std::string candidate = "-[" + trim(label) + "] text";
    return !take_speaker_tag(candidate).empty();
}

/// Remove inline SDH annotations while preserving leading [Speaker] labels.
/// A malformed leading bracket is treated as an SDH-only cue.
std::string strip_inline_sdh(std::string text) {
    std::string out;
    std::size_t cursor = 0;
    bool removed = false;
    while (cursor < text.size()) {
        const auto open = text.find('[', cursor);
        if (open == std::string::npos) {
            out += text.substr(cursor);
            break;
        }
        out += text.substr(cursor, open - cursor);
        const auto close = text.find(']', open + 1);
        if (close == std::string::npos) {
            if (trim(out).empty()) return {};
            out += text.substr(open);
            break;
        }
        const std::string label = text.substr(open + 1, close - open - 1);
        const bool leading = trim(out).empty();
        const bool dash_label = open > 0 && text[open - 1] == '-';
        if ((leading || dash_label) && !removed && looks_like_speaker_label(label)) {
            out += text.substr(open, close - open + 1);
        } else {
            removed = true;
        }
        cursor = close + 1;
    }
    return removed ? trim(out) : text;
}

/// 一段文本切成 N 份：weights 决定每份的字数比例，顺序即时间顺序。
bool split_text_n(const std::string& text, const std::vector<double>& weights, int min_chars,
                  std::vector<std::string>& parts) {
    parts.clear();
    if (weights.size() < 2) return false;
    double remaining = 0.0;
    for (double w : weights) remaining += std::max(0.001, w);

    std::string rest = text;
    for (std::size_t k = 0; k + 1 < weights.size(); ++k) {
        const double ratio = std::max(0.001, weights[k]) / remaining;
        std::string left;
        std::string right;
        if (!split_text_at(rest, ratio, min_chars, left, right)) return false;
        parts.push_back(std::move(left));
        rest = std::move(right);
        remaining -= std::max(0.001, weights[k]);
    }
    if (visible_length(rest) < min_chars) return false;
    parts.push_back(std::move(rest));
    return true;
}

/// 把多说话人行拆成多条事件：优先用译文轨的时间边界，找不到就按字符比例切。
void split_multi_speaker(std::vector<Cue>& primary, std::vector<Cue>& secondary) {
    std::vector<Cue> out;
    out.reserve(primary.size());
    std::vector<bool> used(secondary.size(), false);

    for (auto& p : primary) {
        const auto segs = speaker_segments(p.text);
        if (segs.size() < 2) {
            out.push_back(p);
            continue;
        }

        std::vector<int> hits;
        for (std::size_t i = 0; i < secondary.size(); ++i) {
            if (used[i] || secondary[i].role == CueRole::annotation) continue;
            const std::int64_t ov =
                overlap_ms(p.start, p.end, secondary[i].start, secondary[i].end);
            if (ov * 2 >= (secondary[i].end - secondary[i].start)) {
                hits.push_back(static_cast<int>(i));
            }
        }
        std::sort(hits.begin(), hits.end(), [&](int a, int b) {
            return secondary[static_cast<std::size_t>(a)].start <
                   secondary[static_cast<std::size_t>(b)].start;
        });

        std::vector<std::string> zh;
        std::vector<std::pair<std::int64_t, std::int64_t>> zh_time;
        if (hits.size() == segs.size()) {
            for (int h : hits) {
                zh.push_back(secondary[static_cast<std::size_t>(h)].text);
                zh_time.push_back({secondary[static_cast<std::size_t>(h)].start,
                                   secondary[static_cast<std::size_t>(h)].end});
                used[static_cast<std::size_t>(h)] = true;
            }
        } else if (hits.size() == 1) {
            auto zsegs = speaker_segments(secondary[static_cast<std::size_t>(hits[0])].text);
            if (zsegs.size() == segs.size()) {
                zh = std::move(zsegs);
                for (auto& segment : zh) {
                    if (!segment.empty() && segment.front() == '-') {
                        segment = trim(segment.substr(1));
                    }
                }
            } else {
                // A single ordinary translation cue covers the whole
                // simultaneous dialogue; retain it on each speaker event.
                zh.assign(segs.size(), secondary[static_cast<std::size_t>(hits[0])].text);
            }
            used[static_cast<std::size_t>(hits[0])] = true;
        }

        // 时间边界：有对应译文就用译文的时间窗口，否则按字符数比例切。
        std::vector<std::int64_t> bounds(segs.size() + 1, p.start);
        bounds.back() = p.end;
        if (zh_time.size() == segs.size()) {
            for (std::size_t k = 0; k < segs.size(); ++k) {
                bounds[k] = std::clamp(zh_time[k].first, p.start, p.end);
                bounds[k + 1] = std::clamp(zh_time[k].second, p.start, p.end);
            }
        } else {
            for (std::size_t k = 0; k < segs.size(); ++k) {
                bounds[k + 1] =
                    p.start + (p.end - p.start) * static_cast<std::int64_t>(k + 1) / segs.size();
            }
        }
        for (std::size_t k = 0; k < segs.size(); ++k) {
            if (k + 1 < bounds.size() && bounds[k + 1] <= bounds[k]) {
                bounds[k + 1] = std::min(p.end, bounds[k] + 400);
            }
        }

        for (std::size_t k = 0; k < segs.size(); ++k) {
            Cue np;
            np.text = segs[k];
            np.speaker = take_dash_speaker_tag(np.text);
            if (np.speaker.empty() && !np.text.empty() && np.text.front() == '-') {
                np.text = trim(np.text.substr(1));
            }
            np.role = classify_cue(np.text);
            np.start = bounds[k];
            np.end = std::max(bounds[k] + 200, bounds[k + 1]);
            if (!zh.empty()) np.translations.push_back({np.start, zh[k]});
            out.push_back(std::move(np));
        }
    }

    std::vector<Cue> kept;
    kept.reserve(secondary.size());
    for (std::size_t i = 0; i < secondary.size(); ++i) {
        if (!used[i]) kept.push_back(std::move(secondary[i]));
    }
    primary = std::move(out);
    secondary = std::move(kept);
}

} // namespace

const char* role_name(CueRole role) {
    switch (role) {
        case CueRole::sdh: return "sdh";
        case CueRole::lyric: return "lyric";
        case CueRole::interjection: return "interjection";
        case CueRole::annotation: return "annotation";
        case CueRole::dialogue: break;
    }
    return "dialogue";
}

int visible_length(const std::string& text) {
    int n = 0;
    bool in_tag = false;
    for_each_cp(text, [&](std::size_t, std::size_t, unsigned cp) {
        if (cp == '{') {
            in_tag = true;
            return;
        }
        if (cp == '}') {
            in_tag = false;
            return;
        }
        if (in_tag) return;
        if (cp == '\n' || cp == '\r' || cp == '\t' || cp == ' ') return;
        ++n;
    });
    return n;
}

CueRole classify_cue(const std::string& text) {
    if (text.find("\xE2\x99\xAA") != std::string::npos) return CueRole::lyric; // ♪
    const std::string trimmed = trim(text);
    if (!trimmed.empty() && trimmed.front() == '[' &&
        trimmed.find(']') == std::string::npos) {
        return CueRole::sdh;
    }

    // 逐行判断是否全是 [音效]
    bool all_brackets = true;
    int bracket_lines = 0;
    std::size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const std::size_t line_end = text.find('\n', line_begin);
        std::string line = trim(text.substr(line_begin, line_end == std::string::npos
                                                          ? std::string::npos
                                                          : line_end - line_begin));
        if (!line.empty()) {
            while (!line.empty() && (line.front() == '-' || line.front() == ' ')) line.erase(0, 1);
            line = trim(line);
            if (!line.empty()) {
                const bool single_bracket_annotation =
                    starts_with(line, "[") && ends_with(line, "]") &&
                    line.find('[', 1) == std::string::npos &&
                    line.find(']', 1) == line.size() - 1;
                if (single_bracket_annotation) {
                    ++bracket_lines;
                } else {
                    all_brackets = false;
                }
            }
        }
        if (line_end == std::string::npos) break;
        line_begin = line_end + 1;
    }
    if (all_brackets && bracket_lines > 0) return CueRole::sdh;

    const std::string t = strip_outer_dialogue_dashes(trimmed);
    const bool wrapped_ascii = starts_with(t, "(") && ends_with(t, ")");
    const bool wrapped_full = starts_with(t, "\xEF\xBC\x88") && ends_with(t, "\xEF\xBC\x89"); // （）
    if (wrapped_ascii || wrapped_full) return CueRole::annotation;

    std::string body = normalize_stutter(t);
    while (!body.empty() && (body.front() == '-' || body.front() == ' ')) body.erase(0, 1);
    body = trim(body);
    int words = 0;
    bool in_word = false;
    for (char c : body) {
        const bool sep = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!sep && !in_word) {
            ++words;
            in_word = true;
        } else if (sep) {
            in_word = false;
        }
    }
    // 只把「纯语气词」当感叹词（Ah / Oh / Huh …），别把 `here.`、`No.` 这种实词也算进来。
    if (words <= 2 && visible_length(body) <= 14 && body.find('[') == std::string::npos) {
        std::string token;
        for (char c : body) {
            if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
                token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        static const char* kInterjections[] = {
            "ah",  "aah", "oh",  "ooh", "huh", "ugh",  "ow",   "ouch", "hey",  "hmm",
            "hm",  "uh",  "um",  "er",  "eh",  "gah",  "guh",  "eugh", "oop",  "blegh",
            "whoa", "hah", "heh", "ha", "hoo", "wah",  "yay",  "phew", "whew", "yo",
            "mm",  "mmm", "hmph", "tsk", "ahem", "hooray", "woo", "wow", "oof", "aagh",
            "augh", "ack", "pfft", "what", "oog", "blagh"};
        for (const char* word : kInterjections) {
            if (token == word) return CueRole::interjection;
        }
        std::string word;
        bool all_short_interjections = true;
        bool saw_word = false;
        for (char c : body) {
            if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
                word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            } else if (!word.empty()) {
                saw_word = true;
                bool known = false;
                for (const char* candidate : kInterjections) {
                    if (word == candidate) {
                        known = true;
                        break;
                    }
                }
                all_short_interjections = all_short_interjections && known;
                word.clear();
            }
        }
        if (!word.empty()) {
            saw_word = true;
            bool known = false;
            for (const char* candidate : kInterjections) {
                if (word == candidate) {
                    known = true;
                    break;
                }
            }
            all_short_interjections = all_short_interjections && known;
        }
        if (all_short_interjections && saw_word) return CueRole::interjection;
        bool repeated_vocal = false;
        if (token.size() >= 4) {
            const char first = token.front();
            repeated_vocal = (first == 'a' || first == 'e' || first == 'i' ||
                               first == 'o' || first == 'u') &&
                             std::count(token.begin(), token.end(), first) >= 3;
        }
        if (repeated_vocal ||
            (token.size() >= 6 && token.front() == 'a' && token.back() == 'h')) {
            return CueRole::interjection;
        }
    }
    return CueRole::dialogue;
}

bool load_glossary(const std::string& path, std::map<std::string, std::string>& out,
                   std::string& error) {
    std::ifstream f(infra::utf8_path(path), std::ios::binary);
    if (!f) {
        error = "无法打开术语表：" + path;
        return false;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        const auto tab = t.find('\t');
        if (tab == std::string::npos) {
            error = "术语表第 " + std::to_string(line_no) + " 行缺少 TAB：" + t;
            return false;
        }
        out[glossary_key(trim(t.substr(0, tab)))] = trim(t.substr(tab + 1));
    }
    return true;
}

bool reconcile(const model::Track& primary, const model::Track& secondary,
               const ReconcileOptions& options, model::Track& out, ReconcileStats& stats,
               std::vector<ReconcileRow>& review, std::string& error) {
    stats = ReconcileStats{};
    review.clear();
    out = model::Track{};
    out.type = "transcribe";
    out.language = options.primary_language;

    std::map<std::string, std::string> glossary;
    if (!options.glossary_path.empty()) {
        if (!load_glossary(options.glossary_path, glossary, error)) return false;
    }

    std::vector<Cue> P;
    P.reserve(primary.sentences.size());
    for (const auto& s : primary.sentences) {
        if (s.comment || trim(s.text).empty()) continue;
        Cue c;
        c.start = s.start_ms;
        c.end = s.end_ms > s.start_ms ? s.end_ms : s.start_ms + kMinDurationMs;
        c.text = strip_ass_text(s.text);
        c.text = strip_switch_prefix(c.text);
        if (pseudo_blank(c.text)) continue;
        c.role = classify_cue(c.text);
        if (c.role != CueRole::sdh) {
            c.text = strip_inline_sdh(c.text);
            c.role = classify_cue(c.text);
        }
        P.push_back(std::move(c));
    }
    std::vector<Cue> S;
    S.reserve(secondary.sentences.size());
    for (const auto& s : secondary.sentences) {
        if (s.comment || trim(s.text).empty()) continue;
        Cue c;
        c.start = s.start_ms;
        c.end = s.end_ms > s.start_ms ? s.end_ms : s.start_ms + kMinDurationMs;
        c.text = strip_ass_text(s.text);
        c.text = strip_switch_prefix(c.text);
        if (pseudo_blank(c.text)) continue;
        c.role = classify_cue(c.text);
        if (c.role != CueRole::sdh) {
            c.text = strip_inline_sdh(c.text);
            c.role = classify_cue(c.text);
        }
        S.push_back(std::move(c));
    }
    stats.primary_cues = static_cast<int>(P.size());
    stats.secondary_cues = static_cast<int>(S.size());

    // 一行多说话人（-A -B）先拆成多条事件，否则两种语言会被挤在同一行里。
    split_multi_speaker(P, S);
    if (P.empty()) {
        error = "主轨没有可用字幕";
        return false;
    }
    const auto cue_order = [](const Cue& a, const Cue& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.end < b.end;
    };
    for (auto& cue : P) {
        cue.text = normalize_stutter(strip_outer_dialogue_dashes(cue.text));
        if (pseudo_blank(cue.text)) cue.dead = true;
        cue.role = classify_cue(cue.text);
    }
    for (auto& cue : S) {
        cue.text = normalize_stutter(strip_outer_dialogue_dashes(cue.text));
        if (pseudo_blank(cue.text)) cue.dead = true;
        cue.role = classify_cue(cue.text);
    }
    P.erase(std::remove_if(P.begin(), P.end(), [](const Cue& cue) { return cue.dead; }), P.end());
    S.erase(std::remove_if(S.begin(), S.end(), [](const Cue& cue) { return cue.dead; }), S.end());
    std::stable_sort(P.begin(), P.end(), cue_order);
    std::stable_sort(S.begin(), S.end(), cue_order);

    // DP globally chooses monotonic cue groups; only these selected groups can claim cues.
    int rejected_alignments = 0;
    double confidence_sum = 0.0;
    const auto groups = align_cues(P, S, options, rejected_alignments, confidence_sum);
    stats.alignment_groups = static_cast<int>(groups.size());
    stats.alignment_rejected = rejected_alignments;
    stats.alignment_confidence =
        groups.empty() ? 0.0 : confidence_sum / static_cast<double>(groups.size());
    for (const auto& group : groups) {
        if (group.primary_count == 1) {
            Cue& p = P[group.primary];
            for (std::size_t k = 0; k < group.secondary_count; ++k) {
                Cue& s = S[group.secondary + k];
                p.translations.push_back({s.start, s.text});
                s.claimed = true;
            }
            continue;
        }

        if (group.secondary_count == 1) {
            Cue& s = S[group.secondary];
            std::vector<std::size_t> cands;
            std::vector<double> weights;
            cands.reserve(group.primary_count);
            weights.reserve(group.primary_count);
            for (std::size_t k = 0; k < group.primary_count; ++k) {
                const std::size_t index = group.primary + k;
                cands.push_back(index);
                weights.push_back(static_cast<double>(
                    std::max<std::int64_t>(P[index].end - P[index].start, 1)));
            }

            std::vector<std::string> parts;
            if (split_text_n(s.text, weights, options.min_split_chars, parts) &&
                parts.size() == cands.size()) {
                for (std::size_t k = 0; k < cands.size(); ++k) {
                    Cue& p = P[cands[k]];
                    p.translations.push_back({s.start, std::move(parts[k])});
                }
                s.claimed = true;
                ++stats.split_secondary;
                continue;
            }

            // A single translation that cannot be split is intentionally kept
            // on the first primary cue only. Copying it to every fragment can
            // attach the previous sentence's translation to a new short line.
            ++stats.split_failed;
            review.push_back(
                {"split-failed", s.start, s.end, P[cands.front()].text,
                 s.text + "（切不开，仅归首条；其余留空）"});
            P[cands.front()].translations.push_back({s.start, s.text});
            s.claimed = true;
            continue;
        }

        std::size_t last_primary = group.primary;
        for (std::size_t k = 0; k < group.secondary_count; ++k) {
            Cue& s = S[group.secondary + k];
            std::size_t best = P.size();
            std::int64_t best_overlap = 0;
            for (std::size_t i = last_primary; i < group.primary + group.primary_count; ++i) {
                const std::int64_t overlap = overlap_ms(P[i].start, P[i].end, s.start, s.end);
                if (overlap > best_overlap) {
                    best = i;
                    best_overlap = overlap;
                }
            }
            if (best == P.size()) continue;
            const std::int64_t shorter_duration =
                std::max<std::int64_t>(1, std::min(P[best].end - P[best].start, s.end - s.start));
            if (best_overlap * 100 < shorter_duration * 45) continue;
            P[best].translations.push_back({s.start, s.text});
            s.claimed = true;
            last_primary = best;
        }
    }

    for (auto& s : S) {
        if (s.claimed || s.role == CueRole::annotation) continue;
        ++stats.unlinked_secondary;
        const Cue* sound_effect = nullptr;
        std::int64_t best_overlap = 0;
        for (const auto& p : P) {
            if (p.role != CueRole::sdh) continue;
            const auto overlap = overlap_ms(p.start, p.end, s.start, s.end);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                sound_effect = &p;
            }
        }
        if (sound_effect != nullptr) {
            review.push_back({"unlinked", s.start, s.end, sound_effect->text,
                              s.text + "（只对上了音效行）"});
        } else {
            review.push_back({"unlinked", s.start, s.end, std::string(), s.text});
        }
    }

    // ---- 3) 组装事件 ----
    struct Event {
        std::int64_t start = 0;
        std::int64_t end = 0;
        std::string text;
        std::string translation;
        std::string style;
        std::string role;
        std::string speaker;
    };
    std::vector<Event> events;
    events.reserve(P.size() + S.size());

    for (auto& p : P) {
        if (p.dead) continue;
        if (p.speaker.empty()) p.speaker = take_speaker_tag(p.text);
        Event ev;
        ev.start = p.start;
        ev.end = p.end;
        ev.text = p.text;
        ev.style = style_for(p.role);
        ev.role = role_name(p.role);
        ev.speaker = p.speaker;
        const std::string speaker_lower = [&]() {
            std::string value = ev.speaker;
            for (char& c : value)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return value;
        }();
        for (const char* part : {"voice", "voices", "echo", "doorbell", "rings", "music",
                                 "sound", "noise", "wailing", "yelps"}) {
            if (speaker_lower.find(part) != std::string::npos) {
                ev.speaker.clear();
                break;
            }
        }

        if (!p.translations.empty()) {
            auto parts = p.translations;
            std::stable_sort(parts.begin(), parts.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) ev.translation += ' ';
                ev.translation += parts[i].second;
            }
            ++stats.paired;
            if (parts.size() >= 2) ++stats.merged_secondary;
        } else {
            const std::string& policy = policy_for(p.role, options);
            if (policy == "drop") {
                ++stats.dropped;
                review.push_back({"dropped", p.start, p.end, p.text, ""});
                continue;
            }
            if (policy == "glossary") {
                const auto it = glossary.find(glossary_key(p.text));
                if (it != glossary.end()) {
                    ev.translation = it->second;
                    ++stats.glossed;
                } else {
                    review.push_back({"glossary-miss", p.start, p.end, p.text, ""});
                    ++stats.kept;
                }
            } else {
                ++stats.kept;
            }
        }

        const double dur = static_cast<double>(std::max<std::int64_t>(ev.end - ev.start, 1)) / 1000.0;
        if (!ev.translation.empty() &&
            visible_length(ev.translation) / dur > options.max_cps_zh) {
            ++stats.cps_over_zh;
            review.push_back({"cps-zh", ev.start, ev.end, ev.text, ev.translation});
        }
        if (visible_length(ev.text) / dur > options.max_cps_en) {
            ++stats.cps_over_en;
        }
        events.push_back(std::move(ev));
    }

    if (options.keep_annotations) {
        for (const auto& s : S) {
            if (s.role != CueRole::annotation) continue;
            Event ev;
            ev.start = s.start;
            ev.end = s.end;
            ev.text = s.text;
            ev.style = "chi-annot";
            ev.role = "annotation";
            const double dur =
                static_cast<double>(std::max<std::int64_t>(ev.end - ev.start, 1)) / 1000.0;
            if (visible_length(ev.text) / dur > options.max_cps_zh) ++stats.cps_over_zh;
            events.push_back(std::move(ev));
            ++stats.secondary_only;
        }
    }

    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.end < b.end;
    });

    // A boundary-only interjection must not inherit the neighboring dialogue's
    // translation. Keep repeated translations for real dialogue, but clear a
    // duplicate on an adjacent interjection rather than injecting wrong text.
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i].translation.empty() || events[i - 1].translation.empty()) continue;
        if (events[i].translation != events[i - 1].translation) continue;
        if (events[i].start > events[i - 1].end || events[i - 1].start > events[i].end) continue;
        if (events[i].role == "interjection") {
            events[i].translation.clear();
        } else if (events[i - 1].role == "interjection") {
            events[i - 1].translation.clear();
        }
    }

    out.sentences.reserve(events.size());
    for (auto& ev : events) {
        model::Sentence s;
        s.start_ms = ev.start;
        s.end_ms = ev.end;
        s.text = std::move(ev.text);
        if (!ev.translation.empty()) s.translated_text = std::move(ev.translation);
        s.style = std::move(ev.style);
        s.role = std::move(ev.role);
        if (!ev.speaker.empty()) s.speaker = std::move(ev.speaker);
        out.sentences.push_back(std::move(s));
    }

    std::stable_sort(review.begin(), review.end(),
                     [](const ReconcileRow& a, const ReconcileRow& b) {
                         return a.start_ms < b.start_ms;
                     });
    return true;
}

} // namespace iris::subtitle
