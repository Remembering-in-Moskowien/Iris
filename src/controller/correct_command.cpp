#include "iris/controller/commands.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/text/cleanup.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

int run_correct(const std::string& ir_path, const std::string& out_path) {
    if (!fs::exists(ir_path)) {
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

    const auto report = text::correct_document(doc);

    const std::string target = out_path.empty() ? ir_path : out_path;
    doc.save(target);

    std::cout << _("correct: sentences in ", "correct: 处理句子 ") << report.sentences_in
              << _(", dropped ", "，删除 ") << report.dropped
              << _(", whitespace fixes ", "，空白修正 ") << report.whitespace_fixed
              << _(", punctuation fixes ", "，标点修正 ") << report.punctuation_fixed
              << _(", timing fixes ", "，时间码修正 ") << report.timing_fixed
              << _(", overlaps resolved ", "，重叠消解 ") << report.overlap_fixed << '\n';
    spdlog::info("correct: IR → {}", target);
    return 0;
}

} // namespace iris::controller
