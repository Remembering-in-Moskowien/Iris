#include "iris/controller/commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/quality/report.hpp"
#include "iris/ui/console.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

int run_quality(const std::string& ir_path, const std::string& out_html) {
    if (!fs::exists(ir_path)) {
        ui::failure(_("IR not found: ", "IR 文件不存在：") + ir_path);
        return 1;
    }

    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        ui::failure(_("cannot parse IR: ", "IR 解析失败：") + e.what());
        return 1;
    }

    const auto report = quality::analyze(doc);

    ui::banner("iris quality", _("subtitle quality report", "字幕质量报告"));
    for (const auto& t : report.tracks) {
        ui::section(t.type + (t.language.empty() ? "" : "  " + t.language));
        std::vector<std::vector<std::string>> rows;
        rows.push_back({_("cues", "条目"), std::to_string(t.cues), _("coverage", "覆盖率"),
                        std::to_string(static_cast<int>(t.coverage * 100)) + "%"});
        rows.push_back({_("chars min/avg/max", "字数 最短/平均/最长"),
                        std::to_string(t.min_chars) + " / " +
                            std::to_string(static_cast<int>(t.avg_chars)) + " / " +
                            std::to_string(t.max_chars),
                        _("CPS avg/max", "语速 平均/最高"),
                        std::to_string(static_cast<int>(t.avg_cps)) + " / " +
                            std::to_string(static_cast<int>(t.max_cps))});
        rows.push_back({_("overlaps", "重叠"), std::to_string(t.overlaps),
                        _("long gaps", "长空白"), std::to_string(t.long_gaps)});
        rows.push_back({_("duplicates", "重复"), std::to_string(t.duplicates),
                        _("empty cues", "空条目"), std::to_string(t.empty_cues)});
        ui::table({_("metric", "指标"), _("value", "值"), _("metric", "指标"), _("value", "值")},
                  rows);
        if (!t.issues.empty()) {
            ui::note(std::to_string(t.issues.size()) +
                     _(" issue(s) — see the HTML report", " 处问题 —— 详见 HTML 报告"));
        }
    }

    std::cout << '\n';
    if (report.errors > 0) {
        ui::failure(std::to_string(report.errors) + _(" error(s), ", " 处错误，") +
                    std::to_string(report.warnings) + _(" warning(s)", " 处警告"));
    } else if (report.warnings > 0) {
        ui::warning(std::to_string(report.warnings) + _(" warning(s), no errors", " 处警告，无错误"));
    } else {
        ui::success(_("no issues found", "没有发现问题"));
    }

    const std::string target = out_html.empty() ? ir_path + ".quality.html" : out_html;
    std::ofstream f(target, std::ios::binary);
    if (!f) {
        ui::failure(_("cannot write report: ", "无法写入报告：") + target);
        return 1;
    }
    f << quality::to_html(doc, report);
    f.close();
    ui::step(_("report written: ", "报告已写入：") + target);

    // 有 error 时返回 2，方便脚本把质量检查挂进流水线
    return report.errors == 0 ? 0 : 2;
}

} // namespace iris::controller
