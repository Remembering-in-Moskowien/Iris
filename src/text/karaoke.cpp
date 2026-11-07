#include "iris/text/karaoke.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace iris::text {

namespace {

std::size_t utf8_len(unsigned char c) {
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

std::uint32_t utf8_decode(const std::string& s, std::size_t i, std::size_t len) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (len == 1) return b0;
    std::uint32_t cp = (len == 2) ? (b0 & 0x1F) : (len == 3) ? (b0 & 0x0F) : (b0 & 0x07);
    for (std::size_t k = 1; k < len && i + k < s.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return cp;
}

bool is_cjk_cp(std::uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0xAC00 && cp <= 0xD7AF);
}

bool is_latin_word_cp(std::uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

bool blank_text(const std::string& s) {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool is_vowel(char c) {
    switch (std::tolower(static_cast<unsigned char>(c))) {
        case 'a': case 'e': case 'i': case 'o': case 'u': case 'y': return true;
        default: return false;
    }
}

/// 英文词音节数（够用的估计）：元音组计数 + 词尾不发音 e；中文按字计。
int source_weight(const std::string& text) {
    int cjk = 0;
    std::string latin;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if ((b & 0xE0) == 0xC0) len = 2;
        else if ((b & 0xF0) == 0xE0) len = 3;
        else if ((b & 0xF8) == 0xF0) len = 4;
        if (len == 3 && i + 2 < text.size()) {
            const unsigned cp = ((b & 0x0F) << 12) |
                                ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                                (static_cast<unsigned char>(text[i + 2]) & 0x3F);
            if (cp >= 0x4E00 && cp <= 0x9FFF) {
                ++cjk;
                i += len;
                continue;
            }
        }
        if (std::isalpha(b)) latin.push_back(static_cast<char>(b));
        i += len;
    }
    if (cjk > 0) return cjk;

    int groups = 0;
    bool in_vowel = false;
    for (char c : latin) {
        const bool v = is_vowel(c);
        if (v && !in_vowel) ++groups;
        in_vowel = v;
    }
    if (groups > 1 && !latin.empty() &&
        std::tolower(static_cast<unsigned char>(latin.back())) == 'e') {
        const char prev = latin.size() >= 2 ? latin[latin.size() - 2] : ' ';
        if (!is_vowel(prev) && prev != 'l') --groups; // time / once：词尾 e 不发音
    }
    return std::max(1, groups);
}

} // namespace

std::vector<KaraokeSegment> parse_karaoke_segments(const std::string& raw) {
    std::vector<KaraokeSegment> segs;
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '{') {
            const auto end = raw.find('}', i);
            if (end == std::string::npos) {
                if (!segs.empty()) segs.back().text += raw.substr(i);
                break;
            }
            const std::string block = raw.substr(i + 1, end - i - 1);
            std::vector<int> durations;
            for (std::size_t j = 0; j + 1 < block.size(); ++j) {
                if (block[j] != '\\') continue;
                const char c = block[j + 1];
                if (c != 'k' && c != 'K') continue;
                std::size_t p = j + 2;
                if (p < block.size()) {
                    const char m = block[p];
                    if (m == 'f' || m == 'F' || m == 'o' || m == 'O') ++p;
                }
                int value = 0;
                bool any = false;
                while (p < block.size() && std::isdigit(static_cast<unsigned char>(block[p]))) {
                    value = value * 10 + (block[p] - '0');
                    ++p;
                    any = true;
                }
                if (any) durations.push_back(value);
                j = p - 1;
            }
            for (int d : durations) segs.push_back({d, std::string()});
            i = end + 1;
            continue;
        }
        if (!segs.empty()) segs.back().text.push_back(raw[i]);
        ++i;
    }
    return segs;
}

bool has_karaoke(const std::string& raw) {
    return raw.find("\\K") != std::string::npos || raw.find("\\k") != std::string::npos;
}

std::vector<std::string> lyric_tokens(const std::string& text) {
    std::vector<std::string> tokens;
    bool last_word = false;
    for (std::size_t i = 0; i < text.size();) {
        const auto b = static_cast<unsigned char>(text[i]);
        const std::size_t len = utf8_len(b);
        const std::string ch = text.substr(i, len);
        const std::uint32_t cp = utf8_decode(text, i, len);
        i += len;

        if (cp == ' ' || cp == '\t') {
            if (!tokens.empty()) tokens.back() += ch;
            last_word = false;
            continue;
        }
        if (is_latin_word_cp(cp)) {
            if (last_word && !tokens.empty()) {
                tokens.back() += ch;
            } else {
                tokens.push_back(ch);
                last_word = true;
            }
            continue;
        }
        if (cp == '\'' || cp == '-') {
            if (last_word && !tokens.empty()) {
                tokens.back() += ch;
                continue;
            }
        }
        if (is_cjk_cp(cp)) {
            tokens.push_back(ch);
            last_word = false;
            continue;
        }
        // 标点/其它字符：并入前一个 token，没有则单独成 token。
        if (!tokens.empty()) tokens.back() += ch;
        else tokens.push_back(ch);
        last_word = false;
    }
    return tokens;
}

bool project_translation(const std::string& source_raw, const std::string& target_text,
                         int target_cs, KaraokeResult& out, std::string* error) {
    out = KaraokeResult{};
    const auto segs = parse_karaoke_segments(source_raw);
    if (segs.empty()) {
        if (error) *error = "源行没有 \\K 标签";
        return false;
    }

    std::size_t first = 0;
    while (first < segs.size() && blank_text(segs[first].text)) ++first;
    if (first == segs.size()) {
        if (error) *error = "源行 \\K 段都没有文本";
        return false;
    }
    std::size_t last = segs.size() - 1;
    while (last > first && blank_text(segs[last].text)) --last;

    int lead = 0;
    for (std::size_t i = 0; i < first; ++i) lead += segs[i].cs;
    int hold = 0;
    for (std::size_t i = last + 1; i < segs.size(); ++i) hold += segs[i].cs;

    const auto tokens = lyric_tokens(target_text);
    const int n = static_cast<int>(tokens.size());
    if (n <= 0) {
        if (error) *error = "译文为空";
        return false;
    }

    int source_total = lead + hold;
    std::vector<int> ds;
    ds.reserve(last - first + 1);
    for (std::size_t i = first; i <= last; ++i) {
        source_total += segs[i].cs;
        ds.push_back(segs[i].cs);
    }
    if (source_total <= 0) {
        if (error) *error = "源行总时长为 0";
        return false;
    }

    // 目标行时长一般等于源行；不等时按比例缩放引导/留白/文本段。
    const double scale =
        (target_cs > 0 && target_cs != source_total)
            ? static_cast<double>(target_cs) / static_cast<double>(source_total)
            : 1.0;
    lead = static_cast<int>(std::lround(lead * scale));
    hold = static_cast<int>(std::lround(hold * scale));
    int total_text = 0;
    for (int& d : ds) {
        d = static_cast<int>(std::lround(d * scale));
        total_text += d;
    }
    if (target_cs > 0) {
        const int want = std::max(0, target_cs - lead - hold);
        ds.back() = std::max(0, ds.back() + (want - total_text));
        total_text = want;
    }
    if (total_text <= 0) {
        if (error) *error = "文本段时长为 0";
        return false;
    }

    // 按「源词的音节」分配中文字符：Once(1) / upon(2) / a(1) / time(1) 各拿对应字数，
    // 词的时长原样留给它，因此长音节（"time"）对应的字会被拉长，短词也不会被摊平。
    const int k = static_cast<int>(ds.size());
    std::vector<int> weights(static_cast<std::size_t>(k), 1);
    double weight_sum = 0.0;
    {
        int idx = 0;
        for (std::size_t i = first; i <= last; ++i, ++idx)
            weights[static_cast<std::size_t>(idx)] = std::max(1, source_weight(segs[i].text));
        for (int j = 0; j < k; ++j) weight_sum += weights[static_cast<std::size_t>(j)];
    }

    std::vector<double> quota(static_cast<std::size_t>(k), 0.0);
    std::vector<int> m(static_cast<std::size_t>(k), 0);
    int assigned = 0;
    for (int j = 0; j < k; ++j) {
        quota[static_cast<std::size_t>(j)] =
            static_cast<double>(n) * weights[static_cast<std::size_t>(j)] / weight_sum;
        m[static_cast<std::size_t>(j)] =
            static_cast<int>(std::floor(quota[static_cast<std::size_t>(j)]));
        assigned += m[static_cast<std::size_t>(j)];
    }
    if (n >= k) {
        for (int j = 0; j < k; ++j) {
            if (m[static_cast<std::size_t>(j)] == 0) {
                m[static_cast<std::size_t>(j)] = 1;
                ++assigned;
            }
        }
    }
    while (assigned < n) {
        int best = 0;
        double best_gap = -1.0;
        for (int j = 0; j < k; ++j) {
            const double gap = quota[static_cast<std::size_t>(j)] -
                               static_cast<double>(m[static_cast<std::size_t>(j)]);
            if (gap > best_gap) {
                best_gap = gap;
                best = j;
            }
        }
        ++m[static_cast<std::size_t>(best)];
        ++assigned;
    }
    while (assigned > n) {
        int best = -1;
        double best_gap = 1e9;
        for (int j = 0; j < k; ++j) {
            if (m[static_cast<std::size_t>(j)] <= 1) continue;
            const double gap = quota[static_cast<std::size_t>(j)] -
                               static_cast<double>(m[static_cast<std::size_t>(j)]);
            if (gap < best_gap) {
                best_gap = gap;
                best = j;
            }
        }
        if (best < 0) break;
        --m[static_cast<std::size_t>(best)];
        --assigned;
    }

    std::vector<int> durs(static_cast<std::size_t>(n), 0);
    int token_index = 0;
    int carry = 0;
    for (int j = 0; j < k; ++j) {
        const int words = m[static_cast<std::size_t>(j)];
        int d = ds[static_cast<std::size_t>(j)] + carry;
        if (words <= 0) {
            carry = d; // 该词没分到字：时长并给下一个有字的词
            continue;
        }
        carry = 0;
        const int base = d / words;
        const int rem = d % words;
        for (int q = 0; q < words; ++q)
            durs[static_cast<std::size_t>(token_index++)] = base + (q < rem ? 1 : 0);
    }
    if (carry > 0 && token_index > 0) durs[static_cast<std::size_t>(token_index) - 1] += carry;

    std::string raw;
    raw.reserve(target_text.size() + static_cast<std::size_t>(n) * 8);
    if (lead > 0) raw += "{\\K" + std::to_string(lead) + "}";
    for (int i = 0; i < n; ++i) {
        raw += "{\\K" + std::to_string(durs[static_cast<std::size_t>(i)]) + "}";
        raw += tokens[static_cast<std::size_t>(i)];
    }
    if (hold > 0) raw += "{\\K" + std::to_string(hold) + "}";

    out.raw = std::move(raw);
    out.tokens = n;
    out.total_cs = lead + total_text + hold;
    return true;
}

std::string karaoke_from_timed_tokens(const std::vector<TimedToken>& tokens, double line_start_s,
                                      int total_cs) {
    if (tokens.empty() || total_cs <= 0) return {};

    std::vector<int> starts;
    starts.reserve(tokens.size());
    int last = 0;
    for (const auto& t : tokens) {
        int cs = static_cast<int>(std::lround((t.start - line_start_s) * 100.0));
        cs = std::clamp(cs, last, total_cs);
        starts.push_back(cs);
        last = cs;
    }

    std::string raw;
    if (starts.front() > 0) raw += "{\\K" + std::to_string(starts.front()) + "}";
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const int next = (i + 1 < tokens.size()) ? starts[i + 1] : total_cs;
        const int d = std::max(0, next - starts[i]);
        raw += "{\\K" + std::to_string(d) + "}";
        raw += tokens[i].text;
    }
    return raw.empty() ? std::string() : raw;
}

KaraokeResult distribute_even(const std::string& text, int total_cs) {
    KaraokeResult out;
    const auto tokens = lyric_tokens(text);
    if (tokens.empty() || total_cs <= 0) return out;
    const int n = static_cast<int>(tokens.size());
    const int base = total_cs / n;
    const int rem = total_cs % n;
    std::string raw;
    for (int i = 0; i < n; ++i) {
        const int d = base + (i < rem ? 1 : 0);
        raw += "{\\K" + std::to_string(d) + "}";
        raw += tokens[static_cast<std::size_t>(i)];
    }
    out.raw = std::move(raw);
    out.tokens = n;
    out.total_cs = total_cs;
    return out;
}

KaraokeResult distribute_even_weighted(const std::string& text, int total_cs) {
    KaraokeResult out;
    const auto tokens = lyric_tokens(text);
    if (tokens.empty() || total_cs <= 0) return out;
    const int n = static_cast<int>(tokens.size());

    // 英文按音节、中文按字；词内再按权重分时长，避免 "a" 和 "beautiful" 一样长。
    std::vector<double> weights(static_cast<std::size_t>(n), 1.0);
    double weight_sum = 0.0;
    for (int i = 0; i < n; ++i) {
        weights[static_cast<std::size_t>(i)] =
            static_cast<double>(std::max(1, source_weight(tokens[static_cast<std::size_t>(i)])));
        weight_sum += weights[static_cast<std::size_t>(i)];
    }

    std::vector<int> durs(static_cast<std::size_t>(n), 1);
    int assigned = n;
    if (n > total_cs) {
        // 极短行：只能按顺序给前 total_cs 个 token 各 1cs
        durs.assign(static_cast<std::size_t>(n), 0);
        for (int i = 0; i < total_cs; ++i) durs[static_cast<std::size_t>(i)] = 1;
        assigned = total_cs;
    } else {
        for (int i = 0; i < n; ++i) {
            const double quota = static_cast<double>(total_cs) *
                                 weights[static_cast<std::size_t>(i)] / weight_sum;
            durs[static_cast<std::size_t>(i)] = std::max(1, static_cast<int>(std::floor(quota)));
            assigned += durs[static_cast<std::size_t>(i)] - 1;
        }
    }
    while (assigned < total_cs) {
        int best = 0;
        double best_gap = -1.0;
        for (int i = 0; i < n; ++i) {
            const double quota = static_cast<double>(total_cs) *
                                 weights[static_cast<std::size_t>(i)] / weight_sum;
            const double gap = quota - static_cast<double>(durs[static_cast<std::size_t>(i)]);
            if (gap > best_gap) {
                best_gap = gap;
                best = i;
            }
        }
        ++durs[static_cast<std::size_t>(best)];
        ++assigned;
    }
    while (assigned > total_cs) {
        int best = -1;
        int best_dur = 0;
        for (int i = 0; i < n; ++i) {
            if (durs[static_cast<std::size_t>(i)] > best_dur) {
                best_dur = durs[static_cast<std::size_t>(i)];
                best = i;
            }
        }
        if (best < 0 || best_dur <= 1) break;
        --durs[static_cast<std::size_t>(best)];
        --assigned;
    }

    std::string raw;
    raw.reserve(text.size() + static_cast<std::size_t>(n) * 8);
    for (int i = 0; i < n; ++i) {
        raw += "{\\K" + std::to_string(durs[static_cast<std::size_t>(i)]) + "}";
        raw += tokens[static_cast<std::size_t>(i)];
    }
    out.raw = std::move(raw);
    out.tokens = n;
    out.total_cs = total_cs;
    return out;
}

} // namespace iris::text
