#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace iris::subtitle {

// ---------------------------------------------------------------------------
// ASS/SSA 完整文档模型（保真：样式全字段、Script Info 顺序、事件原始文本）。
//
// 与 parser.hpp 的 Cue 列表不同：这里保留成品信息，用于
//   - convert：ASS → IR（样式 + 覆盖标签 + 层 + Comment）
//   - build  ：IR → ASS（模板/原样式回写，往返不丢东西）
//   - style  ：从成品反推模板 JSON
// ---------------------------------------------------------------------------

/// 一行样式（23 列）。
struct AssStyle {
    std::string name = "Default";
    std::string font = "Arial";
    double size = 32.0;
    std::string primary = "&H00FFFFFF";
    std::string secondary = "&H00FFFFFF";
    std::string outline = "&H00000000";
    std::string back = "&H00000000";
    int bold = 0;
    int italic = 0;
    int underline = 0;
    int strikeout = 0;
    double scale_x = 100.0;
    double scale_y = 100.0;
    double spacing = 0.0;
    double angle = 0.0;
    int border_style = 1;
    double outline_width = 2.0;
    double shadow = 1.0;
    int alignment = 2;
    int margin_l = 10;
    int margin_r = 10;
    int margin_v = 10;
    int encoding = 1;

    [[nodiscard]] std::string to_line() const;
    [[nodiscard]] nlohmann::json to_json() const;
    static AssStyle from_json(const nlohmann::json& j);
};

/// 一条事件（Dialogue / Comment）。text 保留原始覆盖标签。
struct AssEvent {
    int layer = 0;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string style = "Default";
    std::string actor;
    int margin_l = 0;
    int margin_r = 0;
    int margin_v = 0;
    std::string effect;
    std::string text;
    bool comment = false;
};

/// 一份 ASS 脚本：Script Info（有序键值）+ 样式表 + 事件表。
struct AssScript {
    std::vector<std::pair<std::string, std::string>> info; ///< 顺序保留
    std::vector<AssStyle> styles;
    std::vector<AssEvent> events;
    bool has_bom = false;
    bool crlf = true;

    [[nodiscard]] std::string info_value(const std::string& key,
                                         const std::string& fallback = {}) const;
    void set_info(const std::string& key, const std::string& value);
    [[nodiscard]] int play_res_x() const;
    [[nodiscard]] int play_res_y() const;
    [[nodiscard]] const AssStyle* find_style(const std::string& name) const;
};

/// 解析（失败返回 false，error 给原因）。
bool parse_ass_script(const std::string& content, AssScript& out, std::string& error);
bool load_ass_script(const std::string& path, AssScript& out, std::string& error);

/// 渲染（Canonical：固定列顺序、`h:mm:ss.cc`、CRLF 可配）。
std::string render_ass_script(const AssScript& script, bool crlf = true, bool bom = false);

/// 把纯文本里的 `{` `}` 转义成可安全放进 ASS 文本的形式。
std::string escape_ass_text(const std::string& text);

/// 去掉覆盖标签、`\N`/`\h` 变空格、去首尾空白（沿用 parser 的语义）。
std::string strip_ass_text(const std::string& raw);

/// 样式名的语言分类：0 = 中文（chi/zh），1 = 英文（eng/en），2 = 其他。
int style_language_rank(const std::string& style_name);

/// chi ↔ eng / chi-X ↔ eng-X；无法映射时返回空串。
std::string swap_style_language(const std::string& style_name);

/// 语言代码是否中文（zh / chi / chs / cmn）。
bool is_chinese_language(const std::string& language);

} // namespace iris::subtitle
