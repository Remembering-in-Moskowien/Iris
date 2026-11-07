#include "iris/view/ass_builder.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "iris/subtitle/ass_script.hpp"
#include "iris/infra/paths.hpp"

namespace iris::view {

namespace fs = std::filesystem;

namespace {

using subtitle::AssEvent;
using subtitle::AssScript;
using subtitle::AssStyle;

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// 文本是否以中文为主（用于未定义样式时的回退判断）。
bool mostly_cjk(const std::string& text) {
    int cjk = 0;
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
            if (cp >= 0x4E00 && cp <= 0x9FFF) ++cjk;
        }
        i += len;
    }
    return cjk >= 2;
}

fs::path from_utf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool iequals(const std::string& a, const std::string& b) { return lower_ascii(a) == lower_ascii(b); }

void ensure_info(AssScript& script, const std::string& key, const std::string& value) {
    if (script.info_value(key).empty()) script.set_info(key, value);
}

/// 找基础样式：优先同名（chi / eng），否则按语言分类找第一个。
const AssStyle* find_base(const AssScript& script, bool chinese) {
    const char* preferred = chinese ? "chi" : "eng";
    for (const auto& s : script.styles) {
        if (iequals(s.name, preferred)) return &s;
    }
    const int want = chinese ? 0 : 1;
    for (const auto& s : script.styles) {
        if (subtitle::style_language_rank(s.name) == want) return &s;
    }
    return script.styles.empty() ? nullptr : &script.styles.front();
}

const AssStyle* find_style(const AssScript& script, const std::string& name) {
    for (const auto& s : script.styles) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

/// 模板里没有、但 IR 里引用了的样式：从 IR 样式表补进来。
void append_missing_styles(AssScript& script, const model::WorkflowState& state) {
    std::set<std::string> have;
    for (const auto& s : script.styles) have.insert(s.name);
    for (const auto& s : state.styles) {
        if (have.insert(s.name).second) script.styles.push_back(s);
    }
}

bool requests_tadc_theme(const model::IrDocument& doc) {
    const auto is_tadc_name = [](const std::string& style) {
        return style.find("Pomni") != std::string::npos ||
               style.find("Caine") != std::string::npos ||
               style.find("Ragatha") != std::string::npos ||
               style.find("Zooble") != std::string::npos ||
               style.find("Kinger") != std::string::npos ||
               style.find("Gangle") != std::string::npos ||
               style.find("Bubble") != std::string::npos ||
               style.find("Kaufmo") != std::string::npos;
    };
    for (const auto& style : doc.state.styles) {
        if (is_tadc_name(style.name)) return true;
    }
    for (const auto& track : doc.state.tracks) {
        for (const auto& sentence : track.sentences) {
            if (!sentence.style) continue;
            if (is_tadc_name(*sentence.style)) return true;
        }
    }
    return false;
}

std::string discover_style_sheet(const model::IrDocument& doc) {
    if (!requests_tadc_theme(doc)) return {};
    const std::vector<fs::path> candidates = {
        fs::path(infra::executable_dir()) / "themes" / "tadc.json",
        fs::current_path() / "themes" / "tadc.json",
        fs::current_path() / "docs" / "themes" / "tadc.json",
    };
    std::error_code ec;
    for (const auto& path : candidates) {
        if (fs::exists(path, ec)) return infra::path_to_utf8(path);
    }
    return {};
}

bool parse_template_json(const std::string& content, AssScript& out, std::string& error) {
    nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "模板不是合法 JSON 对象";
        return false;
    }
    out = AssScript{};
    if (j.contains("scriptInfo") && j["scriptInfo"].is_array()) {
        for (const auto& kv : j["scriptInfo"]) {
            if (kv.is_array() && kv.size() == 2)
                out.info.emplace_back(kv[0].get<std::string>(), kv[1].get<std::string>());
        }
    }
    if (j.contains("styles") && j["styles"].is_array()) {
        // 支持 "extends"：角色样式只写与基础样式不同的字段（典型是主色），
        // 免得一个 9 人剧组把 23 列样式表抄 18 遍。
        std::map<std::string, nlohmann::json> raw;
        std::vector<std::string> order;
        for (const auto& s : j["styles"]) {
            if (!s.is_object() || !s.contains("name") || !s["name"].is_string()) continue;
            const std::string name = s["name"].get<std::string>();
            if (raw.find(name) == raw.end()) order.push_back(name);
            raw[name] = s;
        }
        std::function<nlohmann::json(const std::string&, int)> resolve =
            [&](const std::string& name, int depth) -> nlohmann::json {
            const auto it = raw.find(name);
            if (it == raw.end() || depth > 8) return nlohmann::json::object();
            const nlohmann::json& self = it->second;
            if (!self.contains("extends") || !self["extends"].is_string()) return self;
            nlohmann::json base = resolve(self["extends"].get<std::string>(), depth + 1);
            if (!base.is_object()) return self;
            for (const auto& [key, value] : self.items()) base[key] = value;
            return base;
        };
        for (const auto& name : order) {
            out.styles.push_back(AssStyle::from_json(resolve(name, 0)));
        }
    }
    if (out.styles.empty()) {
        error = "模板里没有 styles";
        return false;
    }
    return true;
}

void scale_script(AssScript& script, double scale) {
    if (scale <= 0 || scale == 1.0) return;
    for (auto& s : script.styles) {
        s.size *= scale;
        s.spacing *= scale;
        s.outline_width *= scale;
        s.shadow *= scale;
        s.margin_l = static_cast<int>(std::lround(s.margin_l * scale));
        s.margin_r = static_cast<int>(std::lround(s.margin_r * scale));
        s.margin_v = static_cast<int>(std::lround(s.margin_v * scale));
    }
    script.set_info("PlayResX", std::to_string(static_cast<int>(std::lround(script.play_res_x() * scale))));
    script.set_info("PlayResY", std::to_string(static_cast<int>(std::lround(script.play_res_y() * scale))));
}

} // namespace

AssScript default_ass_template() {
    AssScript s;
    s.info = {{"Title", "Default ASS file"},
              {"ScriptType", "v4.00+"},
              {"WrapStyle", "0"},
              {"PlayResX", "1920"},
              {"PlayResY", "1080"},
              {"ScaledBorderAndShadow", "yes"}};

    struct Role {
        const char* name;
        const char* primary;
        const char* secondary;
        const char* outline;
    };
    const Role roles[] = {
        {"Chorus", "&H0044DE4C", "&H00565E6D", "&H00115813"},
        {"Twilight", "&H00D399D0", "&H00565E6D", "&H007B3B29"},
        {"Rarity", "&H00F5E6E6", "&H00363655", "&H00AA5361"},
        {"Rainbow", "&H00FCE5AB", "&H00363655", "&H00BB9C52"},
        {"Pinkie", "&H00DFC2F6", "&H00363655", "&H007F2CCD"},
        {"Applejack", "&H0053B4FF", "&H00363655", "&H000D54E6"},
        {"Fluttershy", "&H00B5F3F5", "&H00363655", "&H00B27EE7"},
    };

    auto base = [&](const char* name, const char* font, double size, int margin_v) {
        AssStyle st;
        st.name = name;
        st.font = font;
        st.size = size;
        st.primary = "&H00FFFFFF";
        st.secondary = "&H00FFFFFF";
        st.outline = "&H37000000";
        st.back = "&H370E0807";
        st.bold = -1;
        st.outline_width = 3.3;
        st.shadow = 2.5;
        st.alignment = 2;
        st.margin_l = 9;
        st.margin_r = 9;
        st.margin_v = margin_v;
        s.styles.push_back(st);
    };
    base("chi", "Dream Han Sans CN W27", 81, 28);
    base("eng", "Source Sans 3 Black", 84, 100);

    for (const auto& r : roles) {
        AssStyle c;
        c.name = std::string("chi-") + r.name;
        c.font = "Dream Han Sans CN W27";
        c.size = 81;
        c.primary = r.primary;
        c.secondary = r.secondary;
        c.outline = r.outline;
        c.back = "&H37000000";
        c.bold = -1;
        c.outline_width = 4.5;
        c.shadow = 2.5;
        c.alignment = 2;
        c.margin_l = 9;
        c.margin_r = 9;
        c.margin_v = 28;
        s.styles.push_back(c);

        AssStyle e = c;
        e.name = std::string("eng-") + r.name;
        e.font = "Source Sans 3 Black";
        e.size = 84;
        e.secondary = "&H00363655";
        e.shadow = 3.0;
        e.margin_v = 880;
        s.styles.push_back(e);
    }

    return s;
}

bool load_ass_template(const std::string& path, AssScript& out, std::string& error) {
    const std::string ext = lower_ascii(fs::path(from_utf8(path)).extension().string());
    std::ifstream f(from_utf8(path), std::ios::binary);
    if (!f) {
        error = "无法打开模板：" + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();
    if (ext == ".ass" || ext == ".ssa") return subtitle::parse_ass_script(content, out, error);
    if (ext == ".json") return parse_template_json(content, out, error);
    error = "模板只支持 .json / .ass / .ssa：" + path;
    return false;
}

bool build_ass(const model::IrDocument& doc, const AssBuildOptions& options, AssBuildResult& out,
               std::string& error) {
    const model::Track* track = nullptr;
    for (const auto& t : doc.state.tracks) {
        if (t.type == "transcribe") {
            track = &t;
            break;
        }
    }
    if (track == nullptr && !doc.state.tracks.empty()) track = &doc.state.tracks.front();
    if (track == nullptr) {
        error = "IR 里没有可用轨道";
        return false;
    }

    AssScript script;
    const std::string style_path = !options.style_sheet_path.empty()
                                       ? options.style_sheet_path
                                       : options.template_path.empty()
                                           ? discover_style_sheet(doc)
                                           : options.template_path;
    if (!style_path.empty()) {
        if (!load_ass_template(style_path, script, error)) return false;
        script.events.clear(); // 模板只取 Script Info + 样式
    } else if (!doc.state.styles.empty()) {
        script.info = doc.state.script_info;
        script.styles = doc.state.styles;
    } else {
        script = default_ass_template();
    }

    ensure_info(script, "ScriptType", "v4.00+");
    ensure_info(script, "WrapStyle", "0");
    ensure_info(script, "ScaledBorderAndShadow", "yes");
    if (script.play_res_x() <= 0) script.set_info("PlayResX", "1920");
    if (script.play_res_y() <= 0) script.set_info("PlayResY", "1080");
    if (!options.title.empty()) script.set_info("Title", options.title);

    if (!style_path.empty()) append_missing_styles(script, doc.state);
    scale_script(script, options.scale);

    const std::string language = !track->language.empty() ? track->language : doc.config.language;
    const bool source_is_chinese = subtitle::is_chinese_language(language);
    const AssStyle* base_source = find_base(script, source_is_chinese);
    const AssStyle* base_other = find_base(script, !source_is_chinese);
    if (base_source == nullptr) {
        error = "样式表是空的";
        return false;
    }

    const std::int64_t offset =
        options.shift_ms + (options.use_ir_offset ? doc.state.timeline_offset_ms : 0);

    out = AssBuildResult{};
    for (const auto& s : track->sentences) {
        if (s.comment && !options.keep_comments) continue;
        const bool has_raw = s.raw.has_value();
        const std::string text = has_raw ? *s.raw : subtitle::escape_ass_text(s.text);
        if (!s.comment && text.empty() && !has_raw) {
            ++out.stats.skipped_empty;
            continue;
        }

        AssEvent ev;
        ev.layer = s.layer;
        ev.start_ms = s.start_ms + offset;
        ev.end_ms = s.end_ms + offset;
        ev.comment = s.comment;
        ev.actor = s.speaker.value_or("");
        ev.style = (s.style && !s.style->empty()) ? *s.style : base_source->name;
        ev.text = text;
        if (find_style(script, ev.style) == nullptr) {
            // 未定义样式：按文本语言回退（中文 → chi，其余 → eng），比一律回退到轨道语言更稳。
            const AssStyle* fallback =
                mostly_cjk(s.text) ? find_base(script, true) : find_base(script, false);
            if (fallback == nullptr) fallback = base_source;
            out.warnings.push_back("样式未定义：" + ev.style + "（回退到 " + fallback->name + "）");
            ev.style = fallback->name;
        }

        if (options.bilingual && !s.comment && s.translated_text && !s.translated_text->empty()) {
            AssEvent tr = ev;
            std::string swapped = subtitle::swap_style_language(ev.style);
            if (swapped.empty() || find_style(script, swapped) == nullptr) {
                swapped = base_other != nullptr ? base_other->name : ev.style;
            }
            tr.style = swapped;
            tr.text = (s.translated_raw && !s.translated_raw->empty())
                          ? *s.translated_raw
                          : subtitle::escape_ass_text(*s.translated_text);
            ++out.stats.translated;
            // MLP 约定：中文在上（先），英文在下（后）；按语言 rank 排序。
            const int rank_primary = subtitle::style_language_rank(ev.style);
            const int rank_trans = subtitle::style_language_rank(tr.style);
            if (rank_trans < rank_primary) {
                script.events.push_back(std::move(tr));
                script.events.push_back(std::move(ev));
            } else {
                script.events.push_back(std::move(ev));
                script.events.push_back(std::move(tr));
            }
            out.stats.lines += 2;
        } else {
            if (s.comment) ++out.stats.comments;
            script.events.push_back(std::move(ev));
            ++out.stats.lines;
        }
    }

    out.text = subtitle::render_ass_script(script, options.crlf, options.bom);
    return true;
}

} // namespace iris::view
