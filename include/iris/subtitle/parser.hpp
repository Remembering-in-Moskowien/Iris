#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iris::subtitle {

// ---------------------------------------------------------------------------
// 字幕文件解析（SRT / ASS / VTT）→ 统一的 cue 列表。
// 对应 C# 的 ConvertParseOperator / CombineParseOperator。
// ---------------------------------------------------------------------------

/// 一条字幕 cue（毫秒时间码 + 纯文本）。
struct Cue {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text;
    std::string translation; ///< 双语合并时填（来自第二条轨道）
};

/// 按扩展名分派（.srt / .ass / .ssa / .vtt）。
bool parse_file(const std::string& path, std::vector<Cue>& out, std::string& error);

/// SRT：`hh:mm:ss,mmm --> hh:mm:ss,mmm`，支持多行文本与 CRLF。
bool parse_srt(const std::string& content, std::vector<Cue>& out, std::string& error);

/// ASS/SSA：只取 [Events] 里的 Dialogue 行（Start/End/Text 三个字段）。
/// 会去掉 `{\...}` 覆盖标签、把 `\N`/`\n` 转成空格。
bool parse_ass(const std::string& content, std::vector<Cue>& out, std::string& error);

/// WebVTT：`hh:mm:ss.mmm --> hh:mm:ss.mmm`（小时可省略）。
bool parse_vtt(const std::string& content, std::vector<Cue>& out, std::string& error);

/// 解析 ASS 的时间码 `h:mm:ss.cc`（百分秒）→ 毫秒。
std::int64_t parse_ass_time(const std::string& text);

/// 清理 ASS 文本：去覆盖标签、换行符合并、压缩空白。
std::string clean_ass_text(const std::string& text);

} // namespace iris::subtitle
