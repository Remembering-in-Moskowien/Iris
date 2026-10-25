#include "iris/controller/commands.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "iris/infra/i18n.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/subtitle/parser.hpp"
#include "iris/text/reference_diff.hpp"
#include "iris/ui/console.hpp"
#include <nlohmann/json.hpp>

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

std::string mmss(std::int64_t ms) {
    const auto total = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld", (long long)(total / 60),
                  (long long)(total % 60));
    return buf;
}

} // namespace

int run_diff(const std::string& ir_path, const std::string& reference_path,
             const std::string& out_html, double min_overlap, bool cjk_only) {
    if (!fs::exists(infra::utf8_path(ir_path))) {
        std::cerr << _("diff: IR not found: ", "diff: IR 文件不存在：") << ir_path << '\n';
        return 1;
    }
    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        std::cerr << "diff: " << e.what() << '\n';
        return 1;
    }

    std::vector<subtitle::Cue> reference;
    std::string error;
    if (!subtitle::parse_file(reference_path, reference, error)) {
        std::cerr << "diff: " << error << '\n';
        return 1;
    }

    const auto report = text::diff_reference(doc, reference, min_overlap, 200, cjk_only);
    std::cout << _("diff: sentences ", "diff: 当前句数 ") << report.sentences
              << _(", reference cues ", "，参考条数 ") << report.reference_cues
              << _(", matched ", "，配对 ") << report.matched
              << _(", uncovered ", "，参考有我们缺 ") << report.missing_in_ours
              << _(", no reference ", "，我们有参考缺 ") << report.missing_in_reference
              << _(", length anomalies ", "，长度异常 ") << report.length_mismatch << '\n';

    int shown = 0;
    for (const auto& p : report.issues) {
        if (shown++ >= 15) break;
        std::cout << "  [" << p.kind << "] " << mmss(p.start_ms) << "–" << mmss(p.end_ms) << "  "
                  << (p.ours.empty() ? "<missing>" : p.ours) << "  ||  "
                  << (p.reference.empty() ? "<missing>" : p.reference) << '\n';
    }
    if (static_cast<int>(report.issues.size()) > 15)
        std::cout << _("  ... more in the HTML report", "  ……其余见 HTML 报告") << '\n';

    const std::string target =
        out_html.empty() ? ir_path + ".diff.html" : out_html;
    std::ofstream f(infra::utf8_path(target), std::ios::binary);
    if (!f) {
        std::cerr << _("diff: cannot write report: ", "diff: 无法写入报告：") << target << '\n';
        return 1;
    }
    f << text::ref_diff_html(
        report, infra::path_to_utf8(infra::utf8_path(ir_path).filename()) + " ↔ " +
                    infra::path_to_utf8(infra::utf8_path(reference_path).filename()));
    std::cout << _("diff: report written: ", "diff: 报告已写入：") + target << '\n';

    nlohmann::json out({{"ok", true},
                        {"command", "diff"},
                        {"ir", ir_path},
                        {"reference", reference_path},
                        {"report", target},
                        {"sentences", report.sentences},
                        {"referenceCues", report.reference_cues},
                        {"matched", report.matched},
                        {"uncovered", report.missing_in_ours},
                        {"noReference", report.missing_in_reference},
                        {"lengthAnomalies", report.length_mismatch}});
    ui::json_out(out.dump(2));

    return (report.missing_in_ours + report.missing_in_reference) > 0 ? 2 : 0;
}

} // namespace iris::controller
