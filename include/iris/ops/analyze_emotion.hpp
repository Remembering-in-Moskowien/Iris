#pragma once

#include <string>

#include "iris/core/operator.hpp"

namespace iris::ops {

/// 语音情感分析（Wav2Small / wav2vec2 ONNX）：对整段语音给出 6 类情绪与置信度。
///
/// 现状：结果只写入日志与 IR provenance —— TTS 侧还没有情绪风格注入接口，
/// 因此本算子不改变后续合成的音色/风格，只作为 dub 链路的一环留痕。
class AnalyzeEmotion : public core::Operator {
public:
    explicit AnalyzeEmotion(std::string onnx_path) : onnx_path_(std::move(onnx_path)) {}

    [[nodiscard]] std::string name() const override { return "analyze_emotion"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"audio_preprocess"};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    std::string onnx_path_;
};

} // namespace iris::ops
