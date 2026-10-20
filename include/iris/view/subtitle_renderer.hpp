#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "iris/core/operator.hpp"
#include "iris/model/subtitle.hpp"

namespace iris::view {

/// 输出格式（MVC View 层支持的渲染目标）。
enum class SubtitleFormat { Ass, Srt, Txt };

/// 将格式名（ass|srt|txt）解析为 SubtitleFormat；未知返回 false。
bool parse_subtitle_format(const std::string& format, SubtitleFormat& out);

/// 渲染字幕文档为指定格式文本（View 层唯一渲染入口）。
/// txt 为纯文本行（每行一条字幕文本）。
std::string render_subtitles(const model::SubtitleDocument& doc, SubtitleFormat format);

/// 对应 C# build 命令：把 IR 中的句子渲染为 ASS/SRT/TXT（管线操作符形态）。
class SubtitleRenderer : public core::Operator {
public:
    [[nodiscard]] std::string name() const override { return "build"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"sentence_split"};
    }
    bool execute(core::PipelineContext& ctx) override;
};

} // namespace iris::view
