#include "iris/quality/ass_lint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "iris/infra/i18n.hpp"
#include "iris/subtitle/parser.hpp"
#include "iris/text/cleanup.hpp"

namespace iris::quality {

namespace fs = std::filesystem;

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

/// 字体名/文件名规范化：小写、只留字母数字（"Dream Han Sans CN W27" ↔ "DreamHanSansCN-W27.ttf"）。
std::string normalize_font(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out.push_back(static_cast<char>(std::tolower(u)));
    }
    return out;
}

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

struct TextCounts {
    int chars = 0; ///< 非空白字符数
    int cjk = 0;
    int latin = 0; ///< ASCII 字母
};

/// 非法 UTF-8 字节替换成 '?'（源字幕可能混入非 UTF-8 字节）。
std::string utf8_sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        const std::size_t len = utf8_len(b);
        bool ok = len >= 2 && i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
        }
        if (!ok) {
            out.push_back('?');
            ++i;
            continue;
        }
        out.append(s, i, len);
        i += len;
    }
    return out;
}

/// 按 UTF-8 字符边界截断（避免把汉字劈成半个字节，产出非法 UTF-8）。
std::string utf8_head(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end) + "…";
}

std::string safe_head(const std::string& s, std::size_t max_bytes) {
    return utf8_head(utf8_sanitize(s), max_bytes);
}

TextCounts count_text(const std::string& s) {
    TextCounts c;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const std::size_t len = utf8_len(b);
        if (len == 1 && std::isspace(b)) {
            ++i;
            continue;
        }
        ++c.chars;
        if (len == 1) {
            if (std::isalpha(b)) ++c.latin;
            ++i;
            continue;
        }
        const std::uint32_t cp = utf8_decode(s, i, len);
        if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0xF900 && cp <= 0xFAFF)) {
            ++c.cjk;
        }
        i += len;
    }
    return c;
}

enum class StyleLang { Cjk, Latin, Other };

StyleLang style_lang(const std::string& style) {
    const std::string s = lower_ascii(style);
    if (starts_with(s, "chi") || starts_with(s, "zh") || starts_with(s, "cn")) return StyleLang::Cjk;
    if (s == "en" || starts_with(s, "eng") || starts_with(s, "en-") || starts_with(s, "en_"))
        return StyleLang::Latin;
    return StyleLang::Other;
}

/// 扫描原始文本里的 \k/\K/\kf/\ko 时长（百分秒），返回总和。
int karaoke_sum_cs(const std::string& raw) {
    int sum = 0;
    for (std::size_t i = 0; i + 1 < raw.size(); ++i) {
        if (raw[i] != '\\') continue;
        const char c = raw[i + 1];
        if (c != 'k' && c != 'K') continue;
        std::size_t j = i + 2;
        if (j < raw.size()) {
            const char m = raw[j];
            if (m == 'f' || m == 'F' || m == 'o' || m == 'O') ++j;
        }
        int value = 0;
        bool any = false;
        while (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j]))) {
            value = value * 10 + (raw[j] - '0');
            ++j;
            any = true;
        }
        if (any) {
            sum += value;
            i = j - 1; // 跳过已消费的数字
        }
    }
    return sum;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(trim(cur));
    return out;
}

/// 路径 → UTF-8（Windows 上 path::string() 走本地代码页，中文文件名会乱码/抛异常）。
std::string path_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

/// UTF-8 → path（不用 C++20 已弃用的 std::filesystem::u8path）。
fs::path from_utf8(const std::string& s) {
    const auto u8 = std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return fs::path(u8);
}

} // namespace

bool parse_ass_document(const std::string& content, LintFile& out, std::string& error) {
    out = LintFile{};
    std::size_t pos = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        out.has_bom = true;
        pos = 3;
    }
    out.crlf = content.find("\r\n") != std::string::npos;

    // 分行（保留行号：文件行号从 1 开始）
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = pos; i < content.size(); ++i) {
        const char c = content[i];
        if (c == '\r') continue;
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);

    enum class Section { None, ScriptInfo, Styles, Events, Fonts };
    Section section = Section::None;
    std::vector<std::string> style_cols;
    std::vector<std::string> event_cols;
    bool saw_dialogue = false;

    for (std::size_t idx = 0; idx < lines.size(); ++idx) {
        const int line_no = static_cast<int>(idx) + 1;
        const std::string raw = lines[idx];
        const std::string line = trim(raw);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            const std::string name = lower_ascii(line);
            if (name == "[script info]") section = Section::ScriptInfo;
            else if (name == "[v4+ styles]" || name == "[v4 styles]" || name == "[v4 styles+]")
                section = Section::Styles;
            else if (name == "[events]") section = Section::Events;
            else if (name == "[fonts]" || name == "[graphics]") section = Section::Fonts;
            else section = Section::None;
            continue;
        }

        if (section == Section::ScriptInfo) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = trim(line.substr(0, colon));
            const std::string value = trim(line.substr(colon + 1));
            const std::string lk = lower_ascii(key);
            if (lk == "playresx") out.play_res_x = std::atoi(value.c_str());
            else if (lk == "playresy") out.play_res_y = std::atoi(value.c_str());
            else if (lk == "title") out.title = value;
            else if (lk == "warpstyle") out.wrapstyle_typo = true;
            continue;
        }

        if (section == Section::Styles) {
            if (starts_with(line, "Format:")) {
                style_cols = split_csv(line.substr(7));
                for (auto& c : style_cols) c = lower_ascii(c);
                continue;
            }
            if (!starts_with(line, "Style:")) continue;
            const auto fields = split_csv(line.substr(6));
            if (fields.empty()) continue;
            LintStyle st;
            for (std::size_t i = 0; i < fields.size() && i < style_cols.size(); ++i) {
                const std::string& col = style_cols[i];
                const std::string& v = fields[i];
                if (col == "name") st.name = v;
                else if (col == "fontname") st.font = v;
                else if (col == "fontsize") st.size = std::atoi(v.c_str());
                else if (col == "alignment") st.alignment = std::atoi(v.c_str());
                else if (col == "marginl") st.margin_l = std::atoi(v.c_str());
                else if (col == "marginr") st.margin_r = std::atoi(v.c_str());
                else if (col == "marginv") st.margin_v = std::atoi(v.c_str());
            }
            if (!st.name.empty()) out.styles.push_back(std::move(st));
            continue;
        }

        if (section == Section::Events) {
            if (starts_with(line, "Format:")) {
                event_cols = split_csv(line.substr(7));
                for (auto& c : event_cols) c = lower_ascii(c);
                continue;
            }
            const bool is_comment = starts_with(line, "Comment:");
            const bool is_dialogue = starts_with(line, "Dialogue:");
            if (!is_comment && !is_dialogue) continue;
            if (is_dialogue) saw_dialogue = true;
            const std::string body = line.substr(line.find(':') + 1);

            std::vector<std::string> cols = event_cols;
            if (cols.empty())
                cols = {"layer", "start", "end", "style", "name", "marginl", "marginr", "marginv",
                        "effect", "text"};

            std::vector<std::string> fields;
            std::string cur_field;
            int field = 0;
            const int max_fields = static_cast<int>(cols.size()) - 1;
            for (char c : body) {
                if (c == ',' && field < max_fields) {
                    fields.push_back(cur_field);
                    cur_field.clear();
                    ++field;
                } else {
                    cur_field.push_back(c);
                }
            }
            fields.push_back(cur_field);
            if (fields.size() < cols.size()) continue;

            LintEvent ev;
            ev.line_no = line_no;
            ev.comment = is_comment;
            bool got_text = false;
            for (std::size_t i = 0; i < cols.size(); ++i) {
                const std::string& col = cols[i];
                const std::string& v = fields[i];
                if (col == "layer") ev.layer = std::atoi(v.c_str());
                else if (col == "start") ev.start_ms = subtitle::parse_ass_time(trim(v));
                else if (col == "end") ev.end_ms = subtitle::parse_ass_time(trim(v));
                else if (col == "style") ev.style = trim(v);
                else if (col == "name") ev.name = trim(v);
                else if (col == "marginl") ev.margin_l = std::atoi(v.c_str());
                else if (col == "marginr") ev.margin_r = std::atoi(v.c_str());
                else if (col == "marginv") ev.margin_v = std::atoi(v.c_str());
                else if (col == "effect") ev.effect = trim(v);
                else if (col == "text") {
                    ev.raw = v;
                    got_text = true;
                }
            }
            if (!got_text && !fields.empty()) ev.raw = fields.back();
            ev.text = subtitle::clean_ass_text(ev.raw);
            ev.karaoke_cs = karaoke_sum_cs(ev.raw);
            ev.has_karaoke = ev.karaoke_cs > 0;
            out.events.push_back(std::move(ev));
            continue;
        }
    }

    if (!saw_dialogue) {
        error = "没有找到 [Events] 里的 Dialogue 行";
        return false;
    }
    return true;
}

bool load_ass_file(const std::string& path, LintFile& out, std::string& error) {
    std::ifstream f(from_utf8(path), std::ios::binary);
    if (!f) {
        error = "无法打开文件：" + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();
    if (!parse_ass_document(content, out, error)) return false;
    out.path = path;
    return true;
}

std::vector<std::string> collect_fonts(const std::vector<std::string>& dirs) {
    std::vector<std::string> out;
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
             end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = lower_ascii(it->path().extension().string());
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc" && ext != ".otc") continue;
            out.push_back(normalize_font(it->path().stem().string()));
        }
        if (ec) continue;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

LintFileResult lint_ass(const LintFile& file, const LintOptions& options,
                        const std::vector<std::string>& font_index) {
    LintFileResult r;
    r.path = file.path;
    r.parsed = true;
    r.events = static_cast<int>(file.events.size());

    std::map<std::string, const LintStyle*> styles;
    for (const auto& s : file.styles) styles[s.name] = &s;

    std::set<std::string> font_set(font_index.begin(), font_index.end());
    std::set<std::string> warned_fonts;
    std::map<std::string, int> codes;

    auto add = [&](int line, const std::string& code, const std::string& sev, std::string msg) {
        ++codes[code];
        ++r.issues_total;
        if (sev == "error") ++r.errors;
        else if (sev == "warning") ++r.warnings;
        if (static_cast<int>(r.issues.size()) < options.max_issues_per_file)
            r.issues.push_back({line, code, sev, std::move(msg)});
    };

    struct Valid {
        int idx = 0;
        std::int64_t start = 0;
        std::int64_t end = 0;
        bool karaoke = false;
    };
    std::vector<Valid> timed;

    for (std::size_t i = 0; i < file.events.size(); ++i) {
        const auto& ev = file.events[i];
        if (ev.comment) continue;
        ++r.dialogues;

        if (ev.start_ms < 0 || ev.end_ms < 0 || ev.end_ms <= ev.start_ms) {
            add(ev.line_no, "bad-time", "error",
                "时间码无效或结束早于开始（" + std::to_string(ev.start_ms) + " → " +
                    std::to_string(ev.end_ms) + " ms）");
            continue;
        }
        timed.push_back({static_cast<int>(i), ev.start_ms, ev.end_ms, ev.has_karaoke});

        if (text::count_control_chars(ev.text) > 0) {
            add(ev.line_no, "control-char", "warning",
                "含不可见控制字符（bidi 嵌入符/零宽空格），会造成字距异常或渲染错位");
        }

        const auto it = styles.find(ev.style);
        if (it == styles.end()) {
            add(ev.line_no, "style-undefined", "error", "样式未定义：" + ev.style);
        } else if (options.check_fonts && !font_set.empty()) {
            const std::string key = normalize_font(it->second->font);
            if (!key.empty() && font_set.find(key) == font_set.end() &&
                warned_fonts.insert(key).second) {
                add(ev.line_no, "font-missing", "warning",
                    "字体未在字体目录找到：" + it->second->font +
                        "（按文件名匹配，可能误报）");
            }
        }

        const TextCounts counts = count_text(ev.text);
        const StyleLang lang = style_lang(ev.style);

        if (lang == StyleLang::Cjk) {
            const bool mostly_latin =
                (counts.cjk == 0 && counts.latin >= 3) ||
                (counts.latin >= 8 && counts.latin >= 4 * std::max(counts.cjk, 1));
            if (mostly_latin)
                add(ev.line_no, "lang-style-mismatch", "warning",
                    "chi 样式里是英文：" + safe_head(ev.text, 60));
        } else if (lang == StyleLang::Latin) {
            const bool mostly_cjk =
                (counts.cjk > 0 && counts.latin == 0) ||
                (counts.cjk >= 4 && counts.cjk >= 2 * std::max(counts.latin, 1));
            if (mostly_cjk)
                add(ev.line_no, "lang-style-mismatch", "warning",
                    "eng 样式里是中文：" + safe_head(ev.text, 60));
        }

        const double dur_s = static_cast<double>(ev.end_ms - ev.start_ms) / 1000.0;
        const double cps = dur_s > 0 ? static_cast<double>(counts.chars) / dur_s : 0.0;
        if (cps > r.max_cps) r.max_cps = cps;

        if (ev.text.empty()) {
            if (ev.has_karaoke)
                add(ev.line_no, "karaoke-empty", "warning", "有 \\K 标签但没有文本");
            else
                add(ev.line_no, "empty-text", "warning", "空字幕行");
        }

        if (ev.has_karaoke) {
            ++r.karaoke_lines;
            const std::int64_t filled = static_cast<std::int64_t>(ev.karaoke_cs) * 10;
            const std::int64_t dur = ev.end_ms - ev.start_ms;
            const std::int64_t diff = dur - filled;
            if (diff > 100) {
                add(ev.line_no, "karaoke-under", "warning",
                    "歌词打轴比行时长短 " + std::to_string(diff) + " ms（末尾留白）");
            } else if (diff < -30) {
                add(ev.line_no, "karaoke-over", "error",
                    "歌词打轴超出 " + std::to_string(-diff) + " ms");
            }
        }

        const bool cjk_line = lang == StyleLang::Cjk || (lang == StyleLang::Other && counts.cjk > 0);
        if (cjk_line) {
            if (cps > options.zh_max_cps)
                add(ev.line_no, "cps-high", "warning",
                    "语速 " + std::to_string(static_cast<int>(cps + 0.5)) + " 字/秒（上限 " +
                        std::to_string(static_cast<int>(options.zh_max_cps)) + "）");
            if (counts.cjk > options.max_cjk_chars)
                add(ev.line_no, "line-long", "warning",
                    "中文行 " + std::to_string(counts.cjk) + " 字（上限 " +
                        std::to_string(options.max_cjk_chars) + "）");
        } else if (lang == StyleLang::Latin) {
            if (cps > options.en_max_cps)
                add(ev.line_no, "cps-high", "warning",
                    "语速 " + std::to_string(static_cast<int>(cps + 0.5)) + " 字符/秒（上限 " +
                        std::to_string(static_cast<int>(options.en_max_cps)) + "）");
        }
    }

    // 重叠：按开始时间排序，比较相邻事件；含 \K 的歌词行允许重叠。
    std::sort(timed.begin(), timed.end(), [](const Valid& a, const Valid& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.idx < b.idx;
    });
    for (std::size_t i = 1; i < timed.size(); ++i) {
        const auto& prev = timed[i - 1];
        const auto& cur = timed[i];
        if (prev.karaoke || cur.karaoke) continue;
        // 双语成品里 chi / eng 是同一时码的两条轨，重叠是设计，不算问题。
        // 未知样式（如 Default / Default-L2）按样式名区分轨道。
        const auto& a = file.events[prev.idx];
        const auto& b = file.events[cur.idx];
        const StyleLang la = style_lang(a.style);
        const StyleLang lb = style_lang(b.style);
        if (la == StyleLang::Other || lb == StyleLang::Other) {
            if (a.style != b.style) continue;
        } else if (la != lb) {
            continue;
        }
        if (cur.start < prev.end - 50) {
            add(b.line_no, "overlap", "warning",
                "与上一行重叠 " + std::to_string(prev.end - cur.start) + " ms");
        }
    }

    // 相邻重复：同文本、间隔 < 800 ms。
    for (std::size_t i = 1; i < timed.size(); ++i) {
        const auto& a = file.events[timed[i - 1].idx];
        const auto& b = file.events[timed[i].idx];
        if (a.text.empty() || a.text != b.text) continue;
        if (b.start_ms - a.start_ms < 800)
            add(b.line_no, "duplicate", "info", "与上一行文本相同：" + safe_head(b.text, 40));
    }

    // 歌词候选段：长时间 + 低语速的连续中文行（启发式，需人工确认）。
    std::vector<const LintEvent*> chi;
    for (const auto& ev : file.events) {
        if (ev.comment || ev.start_ms < 0 || ev.end_ms <= ev.start_ms) continue;
        if (style_lang(ev.style) != StyleLang::Cjk) continue;
        if (ev.text.empty()) continue;
        chi.push_back(&ev);
    }
    std::sort(chi.begin(), chi.end(),
              [](const LintEvent* a, const LintEvent* b) { return a->start_ms < b->start_ms; });

    std::vector<const LintEvent*> block;
    int pending = 0;
    auto close_block = [&]() {
        if (block.size() < 4) {
            block.clear();
            return;
        }
        ++r.lyric_blocks;
        bool karaoke = false;
        for (const auto* e : block) karaoke = karaoke || e->has_karaoke;
        if (!karaoke) {
            ++r.lyric_pending;
            ++pending;
            if (r.lyric_samples.size() < 20) {
                LintLyricBlock b;
                b.start_ms = block.front()->start_ms;
                b.end_ms = block.back()->end_ms;
                b.lines = static_cast<int>(block.size());
                b.has_karaoke = false;
                b.first_text = safe_head(block.front()->text, 48);
                r.lyric_samples.push_back(std::move(b));
            }
        }
        block.clear();
    };

    for (const auto* e : chi) {
        const double dur = static_cast<double>(e->end_ms - e->start_ms) / 1000.0;
        const TextCounts c = count_text(e->text);
        const double cps = dur > 0 ? static_cast<double>(c.chars) / dur : 0.0;
        const bool candidate = dur >= 3.2 && cps <= 3.0;
        if (!candidate) {
            close_block();
            continue;
        }
        if (!block.empty() && e->start_ms - block.back()->end_ms > 1200) close_block();
        block.push_back(e);
    }
    close_block();

    if (pending > 0) {
        for (const auto& b : r.lyric_samples) {
            char buf[64];
            const auto s = b.start_ms / 1000;
            const auto e = b.end_ms / 1000;
            std::snprintf(buf, sizeof(buf), "%02lld:%02lld–%02lld:%02lld", (long long)(s / 60),
                          (long long)(s % 60), (long long)(e / 60), (long long)(e % 60));
            add(0, "lyric-candidate", "info",
                std::string("疑似歌词段 ") + buf + "（" + std::to_string(b.lines) +
                    " 行，未打 \\K）：" + b.first_text);
        }
    }

    // 定义了角色样式却没有 \K：典型「歌曲还没打轴」。
    int role_styles = 0;
    for (const auto& s : file.styles) {
        const std::string n = lower_ascii(s.name);
        if ((starts_with(n, "chi-") || starts_with(n, "eng-") || starts_with(n, "zh-")) &&
            n.size() > 4 && n.substr(n.size() - 3) != "-up")
            ++role_styles;
    }
    if (role_styles >= 3 && r.karaoke_lines == 0 && r.lyric_pending > 0)
        add(0, "song-styles-unused", "info",
            "定义了 " + std::to_string(role_styles) + " 个角色样式，但整片没有 \\K（歌曲疑似未打轴）");

    if (file.wrapstyle_typo)
        add(0, "wrapstyle-typo", "warning", "脚本信息里是 WarpStyle（应为 WrapStyle）");

    std::stable_sort(r.issues.begin(), r.issues.end(), [](const LintIssue& a, const LintIssue& b) {
        auto rank = [](const std::string& s) { return s == "error" ? 0 : s == "warning" ? 1 : 2; };
        if (rank(a.severity) != rank(b.severity)) return rank(a.severity) < rank(b.severity);
        return a.line < b.line;
    });

    r.code_counts.assign(codes.begin(), codes.end());
    std::sort(r.code_counts.begin(), r.code_counts.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return r;
}

namespace {

std::string system_font_dir_join() { return {}; }

std::vector<std::string> default_font_dirs() {
    std::vector<std::string> dirs;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    dirs.push_back(std::string(windir != nullptr ? windir : "C:\\Windows") + "\\Fonts");
    const char* local = std::getenv("LOCALAPPDATA");
    if (local != nullptr)
        dirs.push_back(std::string(local) + "\\Microsoft\\Windows\\Fonts");
#else
    const char* home = std::getenv("HOME");
    dirs.push_back("/usr/share/fonts");
    dirs.push_back("/usr/local/share/fonts");
    if (home != nullptr) dirs.push_back(std::string(home) + "/.fonts");
#endif
    return dirs;
}

} // namespace

LintReport lint_paths(const std::vector<std::string>& inputs, const LintOptions& options) {
    LintReport report;
    std::set<std::string> files;

    std::error_code ec;
    for (const auto& input : inputs) {
        if (fs::is_directory(from_utf8(input), ec)) {
            for (fs::recursive_directory_iterator it(from_utf8(input),
                                                     fs::directory_options::skip_permission_denied,
                                                     ec),
                 end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = lower_ascii(it->path().extension().string());
                if (ext == ".ass" || ext == ".ssa") files.insert(path_utf8(it->path()));
            }
        } else {
            files.insert(input);
        }
    }

    std::vector<std::string> font_dirs = default_font_dirs();
    font_dirs.insert(font_dirs.end(), options.font_dirs.begin(), options.font_dirs.end());
    const auto fonts = options.check_fonts ? collect_fonts(font_dirs) : std::vector<std::string>{};

    std::map<std::string, int> codes;
    for (const auto& path : files) {
        ++report.files_checked;
        LintFile file;
        std::string error;
        LintFileResult r;
        if (!load_ass_file(path, file, error)) {
            ++report.files_failed;
            r.path = path;
            r.parse_error = error;
            report.files.push_back(std::move(r));
            continue;
        }
        r = lint_ass(file, options, fonts);
        report.events += r.events;
        report.dialogues += r.dialogues;
        report.karaoke_lines += r.karaoke_lines;
        report.lyric_blocks += r.lyric_blocks;
        report.lyric_pending += r.lyric_pending;
        report.errors += r.errors;
        report.warnings += r.warnings;
        for (const auto& [code, count] : r.code_counts) codes[code] += count;
        report.files.push_back(std::move(r));
    }

    report.code_counts.assign(codes.begin(), codes.end());
    std::sort(report.code_counts.begin(), report.code_counts.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return report;
}

namespace {

std::string escape_html(const std::string& raw) {
    const std::string s = utf8_sanitize(raw);
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

} // namespace

std::string lint_html(const LintReport& report, const LintOptions& options) {
    std::ostringstream os;
    os << "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n<meta charset=\"utf-8\">\n";
    os << "<title>iris lint — 字幕文件体检</title>\n<style>\n"
          "body{font-family:-apple-system,'Segoe UI',Roboto,'Noto Sans SC',sans-serif;"
          "margin:2rem auto;max-width:1200px;padding:0 1rem;color:#1f2328;background:#fff}\n"
          "h1{font-size:1.5rem;margin:0 0 .25rem}h2{font-size:1.15rem;margin:2rem 0 .5rem}\n"
          ".sub{color:#57606a;margin:0 0 1.5rem}\n"
          ".cards{display:flex;flex-wrap:wrap;gap:.75rem;margin:1rem 0}\n"
          ".card{border:1px solid #d0d7de;border-radius:10px;padding:.6rem .9rem;min-width:120px}\n"
          ".card .k{color:#57606a;font-size:.78rem}.card .v{font-size:1.5rem;font-weight:600}\n"
          "table{border-collapse:collapse;width:100%;font-size:.88rem;margin:.5rem 0 1rem}\n"
          "th,td{border:1px solid #d0d7de;padding:.35rem .5rem;text-align:left;vertical-align:top}\n"
          "th{background:#f6f8fa;font-weight:600}\n"
          ".error{color:#cf222e;font-weight:600}.warning{color:#9a6700;font-weight:600}\n"
          ".info{color:#0969da}\n.mono{font-family:ui-monospace,Consolas,monospace}\n"
          "details{border:1px solid #d0d7de;border-radius:8px;margin:.5rem 0;padding:.4rem .6rem}\n"
          "summary{cursor:pointer;font-weight:600}\n.bad{background:#fff5f5}\n"
          "</style>\n</head>\n<body>\n";
    os << "<h1>iris lint · 字幕文件体检</h1>\n";
    os << "<p class=\"sub\">只读审计：" << report.files_checked << " 个文件，"
       << report.events << " 条事件（" << report.dialogues << " 条对白），"
       << report.karaoke_lines << " 行已有歌词打轴。阈值：中文 " << options.zh_max_cps
       << " 字/秒、英文 " << options.en_max_cps << " 字符/秒、中文行 ≤ "
       << options.max_cjk_chars << " 字。</p>\n";

    os << "<div class=\"cards\">\n";
    auto card = [&](const std::string& k, const std::string& v, const char* cls = "") {
        os << "<div class=\"card\"><div class=\"k\">" << escape_html(k) << "</div><div class=\"v "
           << cls << "\">" << escape_html(v) << "</div></div>\n";
    };
    card("文件", std::to_string(report.files_checked));
    card("对白条数", std::to_string(report.dialogues));
    card("歌词已打轴", std::to_string(report.karaoke_lines));
    card("歌词待补（候选段）", std::to_string(report.lyric_pending));
    card("错误", std::to_string(report.errors), "error");
    card("警告", std::to_string(report.warnings), "warning");
    if (report.files_failed > 0) card("解析失败", std::to_string(report.files_failed), "error");
    os << "</div>\n";

    if (!report.code_counts.empty()) {
        os << "<h2>问题类型</h2>\n<table><tr><th>代码</th><th>条数</th><th>含义</th></tr>\n";
        const std::map<std::string, std::string> meaning = {
            {"style-undefined", "事件引用了不存在的样式"},
            {"font-missing", "样式字体不在字体目录（按文件名匹配）"},
            {"lang-style-mismatch", "中文样式里是英文，或英文样式里是中文"},
            {"karaoke-under", "歌词打轴短于行时长（末尾留白）"},
            {"karaoke-over", "歌词打轴超出时长"},
            {"karaoke-empty", "有 \\K 但没有文本"},
            {"cps-high", "语速超阈值"},
            {"line-long", "行长超阈值"},
            {"overlap", "对白时间重叠（歌词行豁免）"},
            {"duplicate", "相邻行文本重复"},
            {"empty-text", "空字幕行"},
            {"lyric-candidate", "疑似歌词段但未打轴"},
            {"song-styles-unused", "定义了角色样式却没有 \\K"},
            {"wrapstyle-typo", "WarpStyle 拼写错误"},
            {"bad-time", "时间码无效"},
            {"control-char", "含不可见控制字符（bidi / 零宽空格）"}};
        for (const auto& [code, count] : report.code_counts) {
            auto it = meaning.find(code);
            os << "<tr><td class=\"mono\">" << escape_html(code) << "</td><td>" << count
               << "</td><td>" << escape_html(it == meaning.end() ? std::string() : it->second)
               << "</td></tr>\n";
        }
        os << "</table>\n";
    }

    // 待补歌词清单（工作清单）
    os << "<h2>歌词待补清单</h2>\n";
    bool any_pending = false;
    os << "<table><tr><th>文件</th><th>时间</th><th>行数</th><th>首行</th></tr>\n";
    for (const auto& f : report.files) {
        for (const auto& b : f.lyric_samples) {
            any_pending = true;
            os << "<tr><td class=\"mono\">"
               << escape_html(path_utf8(from_utf8(f.path).filename()))
               << "</td><td class=\"mono\">" << mmss(b.start_ms) << "–" << mmss(b.end_ms)
               << "</td><td>" << b.lines << "</td><td>" << escape_html(b.first_text)
               << "</td></tr>\n";
        }
    }
    if (!any_pending) os << "<tr><td colspan=\"4\">没有发现待补歌词段。</td></tr>\n";
    os << "</table>\n";

    os << "<h2>文件明细</h2>\n";
    for (const auto& f : report.files) {
        const std::string name = path_utf8(from_utf8(f.path).filename());
        const char* cls = f.errors > 0 ? " class=\"bad\"" : "";
        os << "<details" << cls << "><summary>" << escape_html(name) << " — 事件 " << f.events
           << " · 歌词 " << f.karaoke_lines << " · 候选段 " << f.lyric_pending << " · 错误 "
           << f.errors << " · 警告 " << f.warnings << "</summary>\n";
        if (!f.parse_error.empty()) {
            os << "<p class=\"error\">解析失败：" << escape_html(f.parse_error) << "</p>\n";
            os << "</details>\n";
            continue;
        }
        if (f.issues.empty()) {
            os << "<p>没有发现问题。</p>\n</details>\n";
            continue;
        }
        os << "<table><tr><th>行</th><th>级别</th><th>代码</th><th>说明</th></tr>\n";
        for (const auto& i : f.issues) {
            os << "<tr><td class=\"mono\">" << i.line << "</td><td class=\"" << i.severity
               << "\">" << i.severity << "</td><td class=\"mono\">" << escape_html(i.code)
               << "</td><td>" << escape_html(i.message) << "</td></tr>\n";
        }
        os << "</table>\n";
        if (f.issues_total > static_cast<int>(f.issues.size()))
            os << "<p class=\"sub\">另有 " << (f.issues_total - static_cast<int>(f.issues.size()))
               << " 条同类问题未逐条列出（每个文件最多 " << options.max_issues_per_file
               << " 条）。</p>\n";
        os << "</details>\n";
    }

    os << "</body>\n</html>\n";
    return os.str();
}

} // namespace iris::quality
