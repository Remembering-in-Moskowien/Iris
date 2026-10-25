#include "iris/controller/commands.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/text/cleanup.hpp"
#include "iris/text/rules.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

int run_correct(const std::string& ir_path, const CorrectCommandOptions& options) {
    if (!fs::exists(infra::utf8_path(ir_path))) {
        std::cerr << _("correct: IR not found: ", "correct: IR 文件不存在：") << ir_path << '\n';
        return 1;
    }

    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        std::cerr << _("correct: cannot parse IR: ", "correct: IR 解析失败：") << e.what() << '\n';
        return 1;
    }

    text::RuleSet rules;
    text::Glossary glossary;
    bool has_rules = false;
    if (!options.rules_path.empty()) {
        std::string error;
        if (!text::load_rules(options.rules_path, rules, error)) {
            std::cerr << "correct: " << error << '\n';
            return 1;
        }
        has_rules = true;
    }
    if (!options.glossary_path.empty()) {
        std::string error;
        if (!text::load_glossary(options.glossary_path, glossary, error)) {
            std::cerr << "correct: " << error << '\n';
            return 1;
        }
        has_rules = true;
    }

    // 规则先于清理：此时行首 "Speaker:" 还是 ASCII 冒号，正则不会在 UTF-8 中间切断。
    text::RuleApplyStats applied;
    if (has_rules) {
        applied = text::apply_rules_to_document(doc, rules, glossary, options.samples);
    }

    const auto cleanup = text::correct_document(doc);

    std::cout << _("correct: sentences in ", "correct: 处理句子 ") << cleanup.sentences_in
              << _(", dropped ", "，删除 ") << cleanup.dropped
              << _(", whitespace fixes ", "，空白修正 ") << cleanup.whitespace_fixed
              << _(", punctuation fixes ", "，标点修正 ") << cleanup.punctuation_fixed
              << _(", timing fixes ", "，时间码修正 ") << cleanup.timing_fixed
              << _(", overlaps resolved ", "，重叠消解 ") << cleanup.overlap_fixed << '\n';
    if (has_rules) {
        std::cout << _("correct: rule hits ", "correct: 规则命中 ") << applied.rule_hits
                  << _(", glossary hits ", "，术语替换 ") << applied.glossary_hits
                  << _(", lines changed ", "，改动行数 ") << applied.changed
                  << _(", raw synced ", "，raw 同步 ") << (cleanup.raw_synced + applied.raw_synced)
                  << _(", raw locked ", "，raw 锁定 ") << (cleanup.raw_locked + applied.raw_locked)
                  << '\n';
    }

    for (const auto& [before, after] : applied.samples) {
        std::cout << "  - " << before << '\n'
                  << "  + " << after << '\n';
    }
    if (applied.changed > static_cast<int>(applied.samples.size()))
        std::cout << _("  ... more changes not shown", "  ……其余改动未逐条显示") << '\n';

    const std::string target = options.out_path.empty() ? ir_path : options.out_path;
    if (options.dry_run) {
        std::cout << _("correct: dry run, nothing written\n", "correct: 预演模式，未写入文件\n");
        return 0;
    }
    try {
        doc.save(target);
    } catch (const std::exception& e) {
        std::cerr << "correct: " << e.what() << '\n';
        return 1;
    }
    spdlog::info("correct: IR → {}", target);
    return 0;
}

} // namespace iris::controller
