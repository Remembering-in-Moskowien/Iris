#pragma once

#include <string>
#include <vector>

#include <regex>

#include "iris/model/ir_document.hpp"

namespace iris::text {

// ---------------------------------------------------------------------------
// 文本规则（把 To-do List 里的手工正则固化成文件）+ 术语表。
//
// 规则文件：每行 `pattern => replacement`
//   - `#` 开头为注释，空行忽略；
//   - pattern 是 ECMAScript 正则（支持 $1 反向引用与 lookahead；**不支持 lookbehind**，
//     用 lookahead 等价写法，见 docs/rules/mlp.rules）；
//   - pattern 也可以是内置宏：@trim / @collapse-spaces / @drop-brackets /
//     @drop-speaker-prefix / @drop-music-note / @space-before-punct / @space-after-punct。
//
// 术语表：每行 `from<TAB>to`（也接受 `from => to`），大小写不敏感的字面替换。
// ---------------------------------------------------------------------------

struct Rule {
    std::string source;      ///< 原始 pattern（宏名或正则）
    std::string replacement; ///< 替换文本
    std::regex regex;        ///< 编译后的正则
};

struct RuleSet {
    std::vector<Rule> rules;
};

struct GlossaryEntry {
    std::string from;
    std::string to;
    std::regex regex;
};

struct Glossary {
    std::vector<GlossaryEntry> entries;
};

/// 读规则文件；失败时 error 带行号。
bool load_rules(const std::string& path, RuleSet& out, std::string& error);

/// 读术语表。
bool load_glossary(const std::string& path, Glossary& out, std::string& error);

/// 命中次数（规则 / 术语表分开统计）。
struct ApplyHits {
    int rules = 0;
    int glossary = 0;
};

/// 对单行文本依次应用术语表与规则。
ApplyHits apply_rules(const RuleSet& rules, const Glossary& glossary, std::string& text);

struct RuleApplyStats {
    int sentences = 0;
    int changed = 0;
    int rule_hits = 0;
    int glossary_hits = 0;
    int raw_synced = 0; ///< 纯文本 raw 已同步
    int raw_locked = 0; ///< 含 \K 等标签，raw 未改动
    std::vector<std::pair<std::string, std::string>> samples; ///< 前 N 条 old → new
};

/// 对 IR 所有轨道应用规则；max_samples 控制保留的样例数。
RuleApplyStats apply_rules_to_document(model::IrDocument& doc, const RuleSet& rules,
                                       const Glossary& glossary, int max_samples = 20);

} // namespace iris::text
