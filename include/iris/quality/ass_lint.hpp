#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iris::quality {

// ---------------------------------------------------------------------------
// ASS/SSA 只读体检（lint）。
//
// 与 subtitle::parse_file 的区别：这里要保留「成品信息」——样式表、角色样式、
// 原始覆盖标签、层、Comment 行，才能检查语言/样式错配、卡拉 OK 标签、字体、
// 重叠与歌词段。解析结果不写回文件，纯审计。
// ---------------------------------------------------------------------------

/// 样式表里的一行（只保留体检需要的列）。
struct LintStyle {
    std::string name;
    std::string font;
    int size = 0;
    int alignment = 0;
    int margin_l = 0;
    int margin_r = 0;
    int margin_v = 0;
};

/// 一条事件（Dialogue / Comment）。
struct LintEvent {
    int line_no = 0;
    int layer = 0;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string style;
    std::string name;
    std::string effect;
    int margin_l = 0;
    int margin_r = 0;
    int margin_v = 0;
    std::string raw;   ///< 原始文本（含 {\...} 标签）
    std::string text;  ///< 去掉标签、\N/\h 换成空格、去首尾空白
    bool comment = false;
    int karaoke_cs = 0;     ///< 行内 \k/\K/\kf/\ko 的时长之和（百分秒）
    bool has_karaoke = false;
};

/// 一个 ASS 文件的解析结果。
struct LintFile {
    std::string path;
    std::string title;
    int play_res_x = 0;
    int play_res_y = 0;
    bool has_bom = false;
    bool crlf = true;
    bool wrapstyle_typo = false; ///< 脚本信息里出现 "WarpStyle"（拼写错误）
    std::vector<LintStyle> styles;
    std::vector<LintEvent> events;
};

/// 一条问题（带文件内行号，方便直接跳到 Aegisub）。
struct LintIssue {
    int line = 0;
    std::string code;     ///< 稳定的机器可读码，如 karaoke-under
    std::string severity; ///< error | warning | info
    std::string message;  ///< 人读说明（中文）
};

/// 待补歌词的候选段（启发式：长时长 + 低语速的连续行）。
struct LintLyricBlock {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    int lines = 0;
    bool has_karaoke = false;
    std::string first_text;
};

/// 单个文件的体检结果。
struct LintFileResult {
    std::string path;
    std::string parse_error;
    bool parsed = false;
    int events = 0;      ///< 全部事件行（含 Comment）
    int dialogues = 0;
    int karaoke_lines = 0;
    int lyric_blocks = 0;
    int lyric_pending = 0; ///< 无 \K 的歌词候选段
    double max_cps = 0.0;  ///< 全文件最高语速
    int errors = 0;
    int warnings = 0;
    int issues_total = 0;
    std::vector<LintIssue> issues;               ///< 已按严重度排序，最多 max_issues_per_file 条
    std::vector<std::pair<std::string, int>> code_counts; ///< 码 → 条数
    std::vector<LintLyricBlock> lyric_samples;   ///< 最多 20 段，供报告展示
};

/// 体检选项（均有保守默认值）。
struct LintOptions {
    double zh_max_cps = 8.0;   ///< 中文行语速上限（字/秒）
    double en_max_cps = 25.0;  ///< 拉丁行语速上限（字符/秒）
    int max_cjk_chars = 20;    ///< 中文行字数上限
    int max_issues_per_file = 60;
    bool check_fonts = true;
    std::vector<std::string> font_dirs; ///< 额外字体目录（--fonts）
};

/// 全量结果。
struct LintReport {
    std::vector<LintFileResult> files;
    int files_checked = 0;
    int files_failed = 0;
    int events = 0;
    int dialogues = 0;
    int karaoke_lines = 0;
    int lyric_blocks = 0;
    int lyric_pending = 0;
    int errors = 0;
    int warnings = 0;
    std::vector<std::pair<std::string, int>> code_counts;
};

/// 解析 ASS/SSA 文本（保留样式与原始标签）。
bool parse_ass_document(const std::string& content, LintFile& out, std::string& error);

/// 读文件后解析（自动识别 BOM）。
bool load_ass_file(const std::string& path, LintFile& out, std::string& error);

/// 建立一个字体索引：目录下的字体文件名规范化（小写、只留字母数字）。
std::vector<std::string> collect_fonts(const std::vector<std::string>& dirs);

/// 体检单个已解析文件。font_index 为空时跳过字体检查。
LintFileResult lint_ass(const LintFile& file, const LintOptions& options,
                        const std::vector<std::string>& font_index);

/// 展开文件/目录并逐个体检（目录递归，只取 .ass/.ssa）。结果按路径排序。
LintReport lint_paths(const std::vector<std::string>& inputs, const LintOptions& options);

/// 自包含 HTML 报告。
std::string lint_html(const LintReport& report, const LintOptions& options);

} // namespace iris::quality
