#include "iris/controller/commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/quality/report.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

int run_quality(const std::string& ir_path, const std::string& out_html) {
    if (!fs::exists(ir_path)) {
        std::cerr << _("quality: IR not found: ", "quality: IR 文件不存在：") << ir_path << '\n';
        return 1;
    }

    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        std::cerr << _("quality: cannot parse IR: ", "quality: IR 解析失败：") << e.what() << '\n';
        return 1;
    }

    const auto report = quality::analyze(doc);

    // 控制台摘要
    for (const auto& t : report.tracks) {
        std::cout << _("quality: track ", "quality: 轨道 ") << t.type
                  << _(" — cues ", " — 条目 ") << t.cues
                  << _(", coverage ", "，覆盖 ") << static_cast<int>(t.coverage * 100) << "%"
                  << _(", avg ", "，平均 ") << static_cast<int>(t.avg_chars) << _(" chars", " 字")
                  << _(", max CPS ", "，最高语速 ") << static_cast<int>(t.max_cps)
                  << _(", overlaps ", "，重叠 ") << t.overlaps
                  << _(", long gaps ", "，长空白 ") << t.long_gaps
                  << _(", duplicates ", "，重复 ") << t.duplicates << '\n';
    }
    std::cout << _("quality: errors ", "quality: 错误 ") << report.errors
              << _(", warnings ", "，警告 ") << report.warnings << '\n';

    const std::string target =
        out_html.empty() ? ir_path + ".quality.html" : out_html;
    std::ofstream f(target, std::ios::binary);
    if (!f) {
        std::cerr << _("quality: cannot write report: ", "quality: 无法写入报告：") << target << '\n';
        return 1;
    }
    f << quality::to_html(doc, report);
    f.close();
    spdlog::info("quality: 报告 → {}", target);

    // 有 error 时返回非零，方便脚本判档
    return report.errors == 0 ? 0 : 2;
}

} // namespace iris::controller
