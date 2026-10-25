#include "iris/subtitle/ass_script.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "iris/subtitle/parser.hpp"

namespace iris::subtitle {

namespace {

constexpr const char* kStyleFormat =
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding";

constexpr const char* kEventFormat =
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text";

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

bool iequals(const std::string& a, const std::string& b) {
    return lower_ascii(a) == lower_ascii(b);
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::vector<std::string> split_fields(const std::string& body, std::size_t max_fields) {
    std::vector<std::string> out;
    std::string cur;
    std::size_t field = 0;
    for (char c : body) {
        if (c == ',' && field + 1 < max_fields) {
            out.push_back(cur);
            cur.clear();
            ++field;
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string format_time(std::int64_t ms) {
    if (ms < 0) ms = 0;
    const std::int64_t cs = (ms + 5) / 10; // 四舍五入到百分秒
    const std::int64_t h = cs / 360000;
    const std::int64_t m = (cs / 6000) % 60;
    const std::int64_t s = (cs / 100) % 60;
    const std::int64_t frac = cs % 100;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld", static_cast<long long>(h),
                  static_cast<long long>(m), static_cast<long long>(s),
                  static_cast<long long>(frac));
    return buf;
}

std::string format_number(double v) {
    if (v == static_cast<double>(static_cast<long long>(v)) && v > -1e12 && v < 1e12) {
        return std::to_string(static_cast<long long>(v));
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::filesystem::path from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

} // namespace

std::string AssStyle::to_line() const {
    std::ostringstream os;
    os << "Style: " << name << ',' << font << ',' << format_number(size) << ',' << primary << ','
       << secondary << ',' << outline << ',' << back << ',' << bold << ',' << italic << ','
       << underline << ',' << strikeout << ',' << format_number(scale_x) << ','
       << format_number(scale_y) << ',' << format_number(spacing) << ',' << format_number(angle)
       << ',' << border_style << ',' << format_number(outline_width) << ','
       << format_number(shadow) << ',' << alignment << ',' << margin_l << ',' << margin_r << ','
       << margin_v << ',' << encoding;
    return os.str();
}

nlohmann::json AssStyle::to_json() const {
    return {{"name", name},
            {"font", font},
            {"size", size},
            {"primary", primary},
            {"secondary", secondary},
            {"outline", outline},
            {"back", back},
            {"bold", bold},
            {"italic", italic},
            {"underline", underline},
            {"strikeout", strikeout},
            {"scaleX", scale_x},
            {"scaleY", scale_y},
            {"spacing", spacing},
            {"angle", angle},
            {"borderStyle", border_style},
            {"outlineWidth", outline_width},
            {"shadow", shadow},
            {"alignment", alignment},
            {"marginL", margin_l},
            {"marginR", margin_r},
            {"marginV", margin_v},
            {"encoding", encoding}};
}

AssStyle AssStyle::from_json(const nlohmann::json& j) {
    AssStyle s;
    s.name = j.value("name", s.name);
    s.font = j.value("font", s.font);
    s.size = j.value("size", s.size);
    s.primary = j.value("primary", s.primary);
    s.secondary = j.value("secondary", s.secondary);
    s.outline = j.value("outline", s.outline);
    s.back = j.value("back", s.back);
    s.bold = j.value("bold", s.bold);
    s.italic = j.value("italic", s.italic);
    s.underline = j.value("underline", s.underline);
    s.strikeout = j.value("strikeout", s.strikeout);
    s.scale_x = j.value("scaleX", s.scale_x);
    s.scale_y = j.value("scaleY", s.scale_y);
    s.spacing = j.value("spacing", s.spacing);
    s.angle = j.value("angle", s.angle);
    s.border_style = j.value("borderStyle", s.border_style);
    s.outline_width = j.value("outlineWidth", s.outline_width);
    s.shadow = j.value("shadow", s.shadow);
    s.alignment = j.value("alignment", s.alignment);
    s.margin_l = j.value("marginL", s.margin_l);
    s.margin_r = j.value("marginR", s.margin_r);
    s.margin_v = j.value("marginV", s.margin_v);
    s.encoding = j.value("encoding", s.encoding);
    return s;
}

std::string AssScript::info_value(const std::string& key, const std::string& fallback) const {
    for (const auto& [k, v] : info) {
        if (iequals(k, key)) return v;
    }
    return fallback;
}

void AssScript::set_info(const std::string& key, const std::string& value) {
    for (auto& [k, v] : info) {
        if (iequals(k, key)) {
            v = value;
            return;
        }
    }
    info.emplace_back(key, value);
}

int AssScript::play_res_x() const { return std::atoi(info_value("PlayResX", "1920").c_str()); }
int AssScript::play_res_y() const { return std::atoi(info_value("PlayResY", "1080").c_str()); }

const AssStyle* AssScript::find_style(const std::string& name) const {
    for (const auto& s : styles) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::string strip_ass_text(const std::string& raw) { return clean_ass_text(raw); }

std::string escape_ass_text(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '{' || c == '}') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

int style_language_rank(const std::string& style_name) {
    const std::string s = lower_ascii(style_name);
    if (starts_with(s, "chi") || starts_with(s, "zh") || starts_with(s, "cn")) return 0;
    if (s == "en" || starts_with(s, "eng") || starts_with(s, "en-") || starts_with(s, "en_"))
        return 1;
    return 2;
}

std::string swap_style_language(const std::string& style_name) {
    const std::string s = lower_ascii(style_name);
    auto suffix = [&](std::size_t prefix_len) { return style_name.substr(prefix_len); };
    if (starts_with(s, "chi-")) return "eng-" + suffix(4);
    if (starts_with(s, "eng-")) return "chi-" + suffix(4);
    if (s == "chi" || s == "zh" || s == "cn") return "eng";
    if (s == "eng" || s == "en") return "chi";
    return {};
}

bool is_chinese_language(const std::string& language) {
    const std::string s = lower_ascii(language);
    return starts_with(s, "zh") || s == "chi" || s == "chs" || s == "cht" || s == "cmn";
}

bool parse_ass_script(const std::string& content, AssScript& out, std::string& error) {
    out = AssScript{};
    std::size_t pos = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        out.has_bom = true;
        pos = 3;
    }
    out.crlf = content.find("\r\n") != std::string::npos;

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

    enum class Section { None, Info, Styles, Events, Ignore };
    Section section = Section::None;
    std::vector<std::string> style_cols;
    std::vector<std::string> event_cols;
    bool saw_events = false;

    for (const auto& raw : lines) {
        const std::string line = trim(raw);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            const std::string name = lower_ascii(line);
            if (name == "[script info]") section = Section::Info;
            else if (name == "[v4+ styles]" || name == "[v4 styles]") section = Section::Styles;
            else if (name == "[events]") section = Section::Events;
            else section = Section::Ignore;
            if (section == Section::Events) saw_events = true;
            continue;
        }

        if (section == Section::Info) {
            if (line.front() == ';' || line.front() == '!') continue;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            out.info.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
            continue;
        }

        if (section == Section::Styles) {
            if (starts_with(line, "Format:")) {
                style_cols = split_fields(line.substr(7), 64);
                for (auto& c : style_cols) c = lower_ascii(trim(c));
                continue;
            }
            if (!starts_with(line, "Style:")) continue;
            const auto f = split_fields(line.substr(6), 23);
            if (style_cols.empty() || f.size() < style_cols.size()) continue;

            AssStyle st;
            for (std::size_t i = 0; i < style_cols.size() && i < f.size(); ++i) {
                const std::string& col = style_cols[i];
                const std::string v = trim(f[i]);
                if (col == "name") st.name = v;
                else if (col == "fontname") st.font = v;
                else if (col == "fontsize") st.size = std::atof(v.c_str());
                else if (col == "primarycolour") st.primary = v;
                else if (col == "secondarycolour") st.secondary = v;
                else if (col == "outlinecolour") st.outline = v;
                else if (col == "backcolour") st.back = v;
                else if (col == "bold") st.bold = std::atoi(v.c_str());
                else if (col == "italic") st.italic = std::atoi(v.c_str());
                else if (col == "underline") st.underline = std::atoi(v.c_str());
                else if (col == "strikeout") st.strikeout = std::atoi(v.c_str());
                else if (col == "scalex") st.scale_x = std::atof(v.c_str());
                else if (col == "scaley") st.scale_y = std::atof(v.c_str());
                else if (col == "spacing") st.spacing = std::atof(v.c_str());
                else if (col == "angle") st.angle = std::atof(v.c_str());
                else if (col == "borderstyle") st.border_style = std::atoi(v.c_str());
                else if (col == "outline") st.outline_width = std::atof(v.c_str());
                else if (col == "shadow") st.shadow = std::atof(v.c_str());
                else if (col == "alignment") st.alignment = std::atoi(v.c_str());
                else if (col == "marginl") st.margin_l = std::atoi(v.c_str());
                else if (col == "marginr") st.margin_r = std::atoi(v.c_str());
                else if (col == "marginv") st.margin_v = std::atoi(v.c_str());
                else if (col == "encoding") st.encoding = std::atoi(v.c_str());
            }
            if (!st.name.empty()) out.styles.push_back(std::move(st));
            continue;
        }

        if (section == Section::Events) {
            if (starts_with(line, "Format:")) {
                event_cols = split_fields(line.substr(7), 64);
                for (auto& c : event_cols) c = lower_ascii(trim(c));
                continue;
            }
            const bool is_comment = starts_with(line, "Comment:");
            const bool is_dialogue = starts_with(line, "Dialogue:");
            if (!is_comment && !is_dialogue) continue;

            std::vector<std::string> cols = event_cols;
            if (cols.empty())
                cols = {"layer", "start", "end", "style", "name", "marginl", "marginr",
                        "marginv", "effect", "text"};
            const std::string body = line.substr(line.find(':') + 1);
            const auto f = split_fields(body, cols.size());
            if (f.size() < cols.size()) continue;

            AssEvent ev;
            ev.comment = is_comment;
            for (std::size_t i = 0; i < cols.size() && i < f.size(); ++i) {
                const std::string& col = cols[i];
                const std::string v = (col == "text") ? f[i] : trim(f[i]);
                if (col == "layer") ev.layer = std::atoi(v.c_str());
                else if (col == "start") ev.start_ms = parse_ass_time(v);
                else if (col == "end") ev.end_ms = parse_ass_time(v);
                else if (col == "style") ev.style = v;
                else if (col == "name") ev.actor = v;
                else if (col == "marginl") ev.margin_l = std::atoi(v.c_str());
                else if (col == "marginr") ev.margin_r = std::atoi(v.c_str());
                else if (col == "marginv") ev.margin_v = std::atoi(v.c_str());
                else if (col == "effect") ev.effect = v;
                else if (col == "text") ev.text = f[i];
            }
            out.events.push_back(std::move(ev));
            continue;
        }
    }

    if (!saw_events) {
        error = "没有找到 [Events] 段";
        return false;
    }
    if (out.events.empty()) {
        error = "[Events] 里没有 Dialogue/Comment 行";
        return false;
    }
    return true;
}

bool load_ass_script(const std::string& path, AssScript& out, std::string& error) {
    std::ifstream f(from_utf8(path), std::ios::binary);
    if (!f) {
        error = "无法打开文件：" + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_ass_script(ss.str(), out, error);
}

std::string render_ass_script(const AssScript& script, bool crlf, bool bom) {
    std::ostringstream os;
    const char* nl = crlf ? "\r\n" : "\n";
    if (bom) os << "\xEF\xBB\xBF";

    os << "[Script Info]" << nl;
    for (const auto& [k, v] : script.info) {
        if (k.empty()) continue;
        os << k << ": " << v << nl;
    }
    os << nl << "[V4+ Styles]" << nl << kStyleFormat << nl;
    for (const auto& s : script.styles) os << s.to_line() << nl;
    os << nl << "[Events]" << nl << kEventFormat << nl;
    for (const auto& e : script.events) {
        os << (e.comment ? "Comment: " : "Dialogue: ") << e.layer << ','
           << format_time(e.start_ms) << ',' << format_time(e.end_ms) << ',' << e.style << ','
           << e.actor << ',' << e.margin_l << ',' << e.margin_r << ',' << e.margin_v << ','
           << e.effect << ',' << e.text << nl;
    }
    return os.str();
}

} // namespace iris::subtitle
