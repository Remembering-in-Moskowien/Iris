#include "iris/controller/commands.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "iris/infra/i18n.hpp"
#include "iris/quality/ass_lint.hpp"
#include "iris/ui/console.hpp"
#include <nlohmann/json.hpp>

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

fs::path from_utf8(const std::string& s) {
    const auto u8 = std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return fs::path(u8);
}

std::string basename_utf8(const std::string& path) {
    const auto u8 = from_utf8(path).filename().u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string mmss(std::int64_t ms) {
    const auto total = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld", (long long)(total / 60),
                  (long long)(total % 60));
    return buf;
}

} // namespace

int run_lint(const std::vector<std::string>& inputs, const quality::LintOptions& options,
             const std::string& out_html) {
    ui::banner("iris lint", _("read-only subtitle checkup", "字幕文件体检（只读）"));

    const auto report = quality::lint_paths(inputs, options);

    if (report.files_checked == 0) {
        ui::failure(_("no .ass/.ssa files found", "没有找到 .ass / .ssa 文件"));
        return 1;
    }

    // 概览
    std::vector<std::vector<std::string>> summary_rows;
    summary_rows.push_back({_("files", "文件"), std::to_string(report.files_checked) +
                                                  (report.files_failed > 0
                                                       ? _(" (failed: ", "（解析失败 ")
                                                             + std::to_string(report.files_failed) +
                                                             "）"
                                                       : std::string()),
                            _("events", "事件"), std::to_string(report.events)});
    summary_rows.push_back({_("dialogues", "对白"), std::to_string(report.dialogues),
                            _("karaoke lines", "歌词已打轴"),
                            std::to_string(report.karaoke_lines)});
    summary_rows.push_back({_("lyric blocks", "歌词候选段"), std::to_string(report.lyric_blocks),
                            _("pending (no \\K)", "待补（无 \\K）"),
                            std::to_string(report.lyric_pending)});
    ui::table({_("metric", "指标"), _("value", "值"), _("metric", "指标"), _("value", "值")},
              summary_rows);

    // 问题类型
    if (!report.code_counts.empty()) {
        ui::section(_("issue types", "问题类型"));
        std::vector<std::vector<std::string>> rows;
        int shown = 0;
        for (const auto& [code, count] : report.code_counts) {
            if (shown++ >= 14) break;
            rows.push_back({code, std::to_string(count)});
        }
        ui::table({_("code", "代码"), _("count", "条数")}, rows);
    }

    // 错误逐条（最多 20 条）
    {
        int printed = 0;
        bool header = false;
        for (const auto& f : report.files) {
            if (f.errors == 0) continue;
            if (!header) {
                ui::section(_("errors", "错误"));
                header = true;
            }
            for (const auto& issue : f.issues) {
                if (issue.severity != "error") continue;
                if (printed++ >= 20) break;
                std::cout << "  " << basename_utf8(f.path) << ':' << issue.line << "  ["
                          << issue.code << "] " << issue.message << '\n';
            }
            if (printed >= 20) break;
        }
        if (printed >= 20)
            ui::note(_("more errors in the HTML report", "更多错误见 HTML 报告"));
    }

    // 歌词待补清单
    if (report.lyric_pending > 0) {
        ui::section(_("lyrics to time", "歌词待补（候选段）"));
        std::vector<std::vector<std::string>> rows;
        int shown = 0;
        for (const auto& f : report.files) {
            for (const auto& b : f.lyric_samples) {
                if (shown++ >= 20) break;
                rows.push_back({basename_utf8(f.path), mmss(b.start_ms) + "–" + mmss(b.end_ms),
                                std::to_string(b.lines), b.first_text});
            }
            if (shown >= 20) break;
        }
        ui::table({_("file", "文件"), _("time", "时间"), _("lines", "行数"), _("first line", "首行")},
                  rows);
        if (report.lyric_pending > 20)
            ui::note(std::to_string(report.lyric_pending - 20) +
                     _(" more blocks in the HTML report", " 段未列出，见 HTML 报告"));
    }

    // HTML 报告
    const std::string target = out_html.empty() ? "iris-lint.html" : out_html;
    {
        std::ofstream f(from_utf8(target), std::ios::binary);
        if (!f) {
            ui::failure(_("cannot write report: ", "无法写入报告：") + target);
            return 1;
        }
        f << quality::lint_html(report, options);
    }
    ui::step(_("report written: ", "报告已写入：") + target);

    std::cout << '\n';
    if (report.errors > 0) {
        ui::failure(std::to_string(report.errors) + _(" error(s), ", " 处错误，") +
                    std::to_string(report.warnings) + _(" warning(s)", " 处警告"));
    } else if (report.warnings > 0) {
        ui::warning(std::to_string(report.warnings) + _(" warning(s), no errors", " 处警告，无错误"));
    } else {
        ui::success(_("no issues found", "没有发现问题"));
    }

    // JSON 摘要
    {
        nlohmann::json files = nlohmann::json::array();
        for (const auto& f : report.files) {
            nlohmann::json codes = nlohmann::json::object();
            for (const auto& [code, count] : f.code_counts) codes[code] = count;
            nlohmann::json samples = nlohmann::json::array();
            for (const auto& b : f.lyric_samples) {
                samples.push_back({{"startMs", b.start_ms},
                                   {"endMs", b.end_ms},
                                   {"lines", b.lines},
                                   {"firstText", b.first_text}});
            }
            files.push_back({{"path", f.path},
                             {"events", f.events},
                             {"karaokeLines", f.karaoke_lines},
                             {"lyricPending", f.lyric_pending},
                             {"errors", f.errors},
                             {"warnings", f.warnings},
                             {"codes", codes},
                             {"lyricSamples", samples},
                             {"parseError", f.parse_error}});
        }
        ui::json_out(nlohmann::json({{"ok", report.errors == 0},
                                     {"command", "lint"},
                                     {"report", target},
                                     {"files", report.files_checked},
                                     {"failed", report.files_failed},
                                     {"events", report.events},
                                     {"dialogues", report.dialogues},
                                     {"karaokeLines", report.karaoke_lines},
                                     {"lyricBlocks", report.lyric_blocks},
                                     {"lyricPending", report.lyric_pending},
                                     {"errors", report.errors},
                                     {"warnings", report.warnings},
                                     {"details", files}})
                         .dump(2));
    }

    return report.errors == 0 ? 0 : 2;
}

} // namespace iris::controller
