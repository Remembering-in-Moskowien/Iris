#pragma once

#include <string>
#include <vector>

#include "iris/asr/diarize.hpp"
#include "iris/core/operator.hpp"

namespace iris::ops {

/// 对应 C# 的 Diarization：给每一句（以及词）贴说话人标签。
///
/// 输入：IR 的 `transcribe` 轨道 + `state.preprocessedAudioPath`（16 kHz 单声道）。
/// 输出：`Sentence.speaker` / `Word.speaker`（`SPEAKER_0`…）；
///       其它轨道（如 translation）按时间中点共享同一套说话人标签。
/// 依赖 sentence_split —— 必须在最终分句之后跑，否则标签会跟着句子被切走。
class Diarize : public core::Operator {
public:
    explicit Diarize(asr::DiarizeOptions options) : opts_(std::move(options)) {}

    [[nodiscard]] std::string name() const override { return "diarize"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"sentence_split"};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    asr::DiarizeOptions opts_;
};

} // namespace iris::ops
