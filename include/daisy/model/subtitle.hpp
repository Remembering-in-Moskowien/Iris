#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "daisy/model/time.hpp"

namespace daisy::model {

/// 字幕输出格式。
enum class SubtitleFormat { Ass, Srt, Txt };

/// ASS 样式（对应 C# 版 ASS 样式模型）。
struct AssStyle {
    std::string name = "Default";
    std::string font_name = "Arial";
    int font_size = 32;
    /// ARGB 色彩，ASS 中写作 &HAABBGGRR。
    std::uint32_t primary_colour = 0x00FFFFFF;
    std::uint32_t outline_colour = 0x00000000;
    int bold = 0;
    int outline = 2;
    int shadow = 1;
    int margin_l = 10;
    int margin_r = 10;
    int margin_v = 10;

    /// 渲染为 ASS 样式行（Format 各列顺序固定）。
    [[nodiscard]] std::string to_ass() const;
};

/// 一行字幕（时间轴 + 文本）。
struct SubtitleLine {
    TimeCode start;
    TimeCode end;
    std::string text;
    std::string style = "Default";
};

/// 字幕文档：样式表 + 行集合，可渲染为 ASS / SRT。
struct SubtitleDocument {
    std::string title;
    SubtitleFormat format = SubtitleFormat::Ass;
    std::vector<AssStyle> styles{AssStyle{}};
    std::vector<SubtitleLine> lines;

    /// 渲染为完整 ASS v4.00+ 脚本。
    [[nodiscard]] std::string render_ass() const;
    /// 渲染为 SRT（带序号）。
    [[nodiscard]] std::string render_srt() const;
};

} // namespace daisy::model
