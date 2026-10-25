#include "iris/text/rules.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

#include "iris/infra/paths.hpp"

namespace iris::text {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string escape_literal(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (std::string(R"(\^$.|?*+()[]{}")").find(c) != std::string::npos) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

/// std::regex_replace 的替换串里 `$` 是元字符，字面替换要转义。
std::string escape_replacement(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '$') out += "$$";
        else out.push_back(c);
    }
    return out;
}

const std::map<std::string, std::pair<const char*, const char*>>& macros() {
    static const std::map<std::string, std::pair<const char*, const char*>> table = {
        {"@trim", {R"(^\s+|\s+$)", ""}},
        {"@collapse-spaces", {R"( {2,})", " "}},
        {"@drop-brackets", {R"(\[[^\]]*\])", ""}},
        // 只认 ASCII 冒号：字节级正则里把全角标点放进字符类会切坏 UTF-8。
        {"@drop-speaker-prefix", {R"(^[^ \t:]{1,24}:\s*)", ""}},
        {"@drop-music-note", {R"( ?♪ ?)", ""}},
        {"@space-before-punct", {R"( +(?=[,.!?;:]))", ""}},
        {"@space-after-punct", {R"(([,.;:!?])(?=[A-Za-z]))", "$1 "}},
        {"@ellipsis-space", {R"(\.\.\.(?=[A-Za-z]))", "... "}},
    };
    return table;
}

std::string escape_braces(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '{' || c == '}') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool has_cjk(const std::string& s) {
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        const auto b = static_cast<unsigned char>(s[i]);
        if ((b & 0xF0) != 0xE0) continue;
        const unsigned cp = ((b & 0x0F) << 12) |
                            ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                            (static_cast<unsigned char>(s[i + 2]) & 0x3F);
        if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
    }
    return false;
}

} // namespace

bool load_rules(const std::string& path, RuleSet& out, std::string& error) {
    out = RuleSet{};
    std::ifstream f(iris::infra::utf8_path(path), std::ios::binary);
    if (!f) {
        error = "无法打开规则文件：" + path;
        return false;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;

        const auto arrow = trimmed.find("=>");
        std::string pattern;
        std::string replacement;
        if (arrow == std::string::npos) {
            // 允许只写宏名（@trim），替换文本由宏表决定。
            if (trimmed.front() != '@') {
                error = "第 " + std::to_string(line_no) + " 行缺少 `=>`";
                return false;
            }
            pattern = trimmed;
        } else {
            pattern = trim(trimmed.substr(0, arrow));
            replacement = trim(trimmed.substr(arrow + 2));
        }
        if (pattern.empty()) {
            error = "第 " + std::to_string(line_no) + " 行 pattern 为空";
            return false;
        }

        Rule rule;
        rule.source = pattern;
        rule.replacement = replacement;
        try {
            if (pattern.front() == '@') {
                const auto it = macros().find(pattern);
                if (it == macros().end()) {
                    error = "第 " + std::to_string(line_no) + " 行未知宏：" + pattern;
                    return false;
                }
                rule.regex = std::regex(it->second.first, std::regex::ECMAScript);
                if (replacement.empty()) rule.replacement = it->second.second;
            } else {
                rule.regex = std::regex(pattern, std::regex::ECMAScript);
            }
        } catch (const std::regex_error& e) {
            error = "第 " + std::to_string(line_no) + " 行正则无效：" + pattern + "（" + e.what() + "）";
            return false;
        }
        out.rules.push_back(std::move(rule));
    }
    if (out.rules.empty()) {
        error = "规则文件里没有有效规则：" + path;
        return false;
    }
    return true;
}

bool load_glossary(const std::string& path, Glossary& out, std::string& error) {
    out = Glossary{};
    std::ifstream f(iris::infra::utf8_path(path), std::ios::binary);
    if (!f) {
        error = "无法打开术语表：" + path;
        return false;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(f, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;

        std::string from, to;
        const auto tab = trimmed.find('\t');
        const auto arrow = trimmed.find("=>");
        if (tab != std::string::npos) {
            from = trim(trimmed.substr(0, tab));
            to = trim(trimmed.substr(tab + 1));
        } else if (arrow != std::string::npos) {
            from = trim(trimmed.substr(0, arrow));
            to = trim(trimmed.substr(arrow + 2));
        } else {
            error = "第 " + std::to_string(line_no) + " 行需要 `<from><TAB><to>` 或 `<from> => <to>`";
            return false;
        }
        if (from.empty() || to.empty()) {
            error = "第 " + std::to_string(line_no) + " 行 from/to 为空";
            return false;
        }
        GlossaryEntry entry;
        entry.from = from;
        entry.to = to;
        // 只有纯 ASCII 术语才开 icase；中文按字节精确匹配（icase 会做字节折叠）。
        bool ascii_alpha = false;
        for (char c : from) {
            if (std::isalpha(static_cast<unsigned char>(c))) ascii_alpha = true;
        }
        try {
            entry.regex = std::regex(escape_literal(from),
                                     ascii_alpha ? std::regex::icase : std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            error = "第 " + std::to_string(line_no) + " 行术语无效：" + from + "（" + e.what() + "）";
            return false;
        }
        out.entries.push_back(std::move(entry));
    }
    if (out.entries.empty()) {
        error = "术语表里没有有效条目：" + path;
        return false;
    }
    return true;
}

ApplyHits apply_rules(const RuleSet& rules, const Glossary& glossary, std::string& text) {
    ApplyHits hits;
    // 术语表只作用于中文行：避免把英文台词里的 "Celestia" 替成中文。
    if (has_cjk(text)) {
        for (const auto& entry : glossary.entries) {
            const std::string replaced =
                std::regex_replace(text, entry.regex, escape_replacement(entry.to));
            if (replaced != text) {
                text = replaced;
                ++hits.glossary;
            }
        }
    }
    for (const auto& rule : rules.rules) {
        const std::string replaced = std::regex_replace(text, rule.regex, rule.replacement);
        if (replaced != text) {
            text = replaced;
            ++hits.rules;
        }
    }
    return hits;
}

RuleApplyStats apply_rules_to_document(model::IrDocument& doc, const RuleSet& rules,
                                       const Glossary& glossary, int max_samples) {
    RuleApplyStats stats;
    for (auto& track : doc.state.tracks) {
        for (auto& s : track.sentences) {
            if (s.comment) continue;
            ++stats.sentences;

            auto fix = [&](std::string& value, std::optional<std::string>& raw) {
                if (value.empty()) return;
                const std::string before = value;
                const ApplyHits hits = apply_rules(rules, glossary, value);
                if (value == before) return;
                ++stats.changed;
                stats.rule_hits += hits.rules;
                stats.glossary_hits += hits.glossary;
                if (raw && raw->find('{') != std::string::npos) {
                    ++stats.raw_locked;
                } else if (raw) {
                    *raw = escape_braces(value);
                    ++stats.raw_synced;
                }
                if (static_cast<int>(stats.samples.size()) < max_samples)
                    stats.samples.emplace_back(before, value);
            };

            fix(s.text, s.raw);
            if (s.translated_text) fix(*s.translated_text, s.translated_raw);
        }
    }
    return stats;
}

} // namespace iris::text
