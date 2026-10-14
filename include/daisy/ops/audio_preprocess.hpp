#pragma once

#include "daisy/core/operator.hpp"

namespace daisy::ops {

/// 对应 C# AudioPreprocess：去噪/归一化（通过 ffmpeg 滤镜）。
class AudioPreprocess : public core::Operator {
public:
    [[nodiscard]] std::string name() const override { return "audio_preprocess"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"ffmpeg_convert"};
    }
    bool execute(core::PipelineContext& ctx) override;
};

} // namespace daisy::ops
