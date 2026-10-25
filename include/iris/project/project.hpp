#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/quality/ass_lint.hpp"

namespace iris::project {

// ---------------------------------------------------------------------------
// 项目化：把「一个目录里的全部字幕」当作一个批次。
//   scan：只读台账（事件数 / 歌词覆盖率 / 待补段 / 错误警告 / 状态）
//   run ：convert → 规则术语 → correct → karaoke → build → lint，可增量重跑
// ---------------------------------------------------------------------------

struct ScanOptions {
    bool recursive = true;
    quality::LintOptions lint;
};

struct Entry {
    std::string input;
    std::string output;
    std::int64_t size = 0;
    std::int64_t modified = 0; ///< 文件时间（计数，用于增量判断）
    int events = 0;
    int karaoke = 0;
    int pending = 0;
    int errors = 0;
    int warnings = 0;
    std::string status; ///< ok | needs-karaoke | error | failed | skipped
    std::string message;
};

struct Report {
    std::string root;
    std::string error; ///< 全局错误（规则文件读不了等）
    std::vector<Entry> entries;
    int files = 0;
    int ok = 0;
    int needs_karaoke = 0;
    int errors = 0;
    int skipped = 0;
    int failed = 0;

    [[nodiscard]] nlohmann::json to_json() const;
    static Report from_json(const nlohmann::json& j);
};

struct RunOptions {
    std::string out_dir = "iris-out";
    std::string template_path;
    std::string rules_path;
    std::string glossary_path;
    bool fill_even = false;
    bool force = false;
    bool keep_ir = true;
    quality::LintOptions lint;
};

/// 展开文件/目录（目录递归，只取 .ass/.ssa），排序去重。
std::vector<std::string> collect_files(const std::vector<std::string>& inputs, bool recursive);

/// 只读扫描。
Report scan(const std::vector<std::string>& inputs, const ScanOptions& options);

/// 批量流水线；manifest_path 非空时读写增量台账。
Report run(const std::vector<std::string>& inputs, const RunOptions& options,
           const std::string& manifest_path);

/// 自包含 HTML 台账。
std::string to_html(const Report& report, const std::string& title);

} // namespace iris::project
