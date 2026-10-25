#include "iris/controller/commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/paths.hpp"
#include "iris/project/project.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

void print_summary(const project::Report& report) {
    for (const auto& e : report.entries) {
        if (e.status == "needs-karaoke" || e.status == "error" || e.status == "failed") {
            std::cout << "  [" << e.status << "] "
                      << iris::infra::path_to_utf8(
                             iris::infra::utf8_path(e.input).filename())
                      << "  events=" << e.events << " karaoke=" << e.karaoke
                      << " pending=" << e.pending << " E=" << e.errors
                      << " W=" << e.warnings;
            if (!e.message.empty()) std::cout << "  " << e.message;
            std::cout << '\n';
        }
    }
    std::cout << _("project: files ", "project: 文件 ") << report.files
              << _(", ok ", "，完成 ") << report.ok
              << _(", needs karaoke ", "，待补歌词 ") << report.needs_karaoke
              << _(", errors ", "，有错误 ") << report.errors
              << _(", skipped ", "，增量跳过 ") << report.skipped
              << _(", failed ", "，失败 ") << report.failed << '\n';
}

} // namespace

int run_project_scan(const std::vector<std::string>& inputs, const project::ScanOptions& options,
                     const std::string& out_html, const std::string& out_json) {
    const auto report = project::scan(inputs, options);
    print_summary(report);

    if (!out_html.empty()) {
        std::ofstream f(iris::infra::utf8_path(out_html), std::ios::binary);
        if (f) f << project::to_html(report, "iris project · 字幕台账");
        else std::cerr << _("project: cannot write ", "project: 无法写入 ") << out_html << '\n';
    }
    if (!out_json.empty()) {
        std::ofstream f(iris::infra::utf8_path(out_json), std::ios::binary);
        if (f) f << report.to_json().dump(2) << '\n';
    }
    return report.failed > 0 ? 2 : 0;
}

int run_project_run(const std::vector<std::string>& inputs, const project::RunOptions& options,
                    const std::string& manifest_path, const std::string& out_html) {
    const std::string manifest =
        manifest_path.empty() ? (options.out_dir + "/manifest.json") : manifest_path;
    const auto report = project::run(inputs, options, manifest);
    if (!report.error.empty()) {
        std::cerr << "project: " << report.error << '\n';
        return 1;
    }
    print_summary(report);

    const std::string html = out_html.empty() ? (options.out_dir + "/project.html") : out_html;
    std::ofstream f(iris::infra::utf8_path(html), std::ios::binary);
    if (f) f << project::to_html(report, "iris project · 批量流水线");
    std::cout << _("project: manifest ", "project: 台账 ") << manifest
              << _(", report ", "，报告 ") << html << '\n';
    return report.failed > 0 ? 2 : 0;
}

} // namespace iris::controller
