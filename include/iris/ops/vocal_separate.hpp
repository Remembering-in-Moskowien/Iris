#pragma once

#include <string>
#include <utility>

#include "iris/asr/separate.hpp"
#include "iris/core/operator.hpp"

namespace iris::ops {

/// 对应 C# 的 VocalSeparation：把输入媒体里的人生轨分离出来，供后续阶段使用。
///
/// 输入：`config.inputFilePath`（原始媒体）。
/// 输出：`state.vocalsPath`（16 kHz 单声道 WAV）+ `isVocalsSeparated`；
///       `AudioPreprocess` 会自动优先吃人声轨（对应 C# 的 VocalsPath ?? ConvertedAudioPath）。
/// 依赖 ffmpeg_convert —— 放在转换之后、预处理之前。
class VocalSeparate : public core::Operator {
public:
    explicit VocalSeparate(asr::SeparateOptions options) : opts_(std::move(options)) {}

    [[nodiscard]] std::string name() const override { return "vocal_separate"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"ffmpeg_convert"};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    asr::SeparateOptions opts_;
};

} // namespace iris::ops
