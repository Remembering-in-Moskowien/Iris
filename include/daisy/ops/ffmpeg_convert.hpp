#pragma once

#include "daisy/core/operator.hpp"

namespace daisy::ops {

/// 对应 C# FFmpegConvert：调用 ffmpeg 将输入媒体转换为 16kHz 单声道 WAV。
class FfmpegConvert : public core::Operator {
public:
    [[nodiscard]] std::string name() const override { return "ffmpeg_convert"; }
    bool execute(core::PipelineContext& ctx) override;
};

} // namespace daisy::ops
