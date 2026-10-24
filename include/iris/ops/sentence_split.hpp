#pragma once

#include <memory>

#include "iris/core/operator.hpp"
#include "iris/text/sat.hpp"

namespace iris::ops {

/// 对应 C# SentenceSplit：把长句切成字幕句。
///
/// 两种策略（对应 C# 的 RuleBasedSplitStrategy 与 SaTSplitStrategy）：
///   - rule：按标点 + 长度硬切（默认，无模型依赖）；
///   - sat ：Segment Any Text 模型（XLM-R + ONNX）预测句界，失败时自动退回 rule。
class SentenceSplit : public core::Operator {
public:
    SentenceSplit() = default;
    explicit SentenceSplit(std::shared_ptr<text::SatSegmenter> sat) : sat_(std::move(sat)) {}

    [[nodiscard]] std::string name() const override { return "sentence_split"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"transcribe"};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    std::shared_ptr<text::SatSegmenter> sat_;
};

} // namespace iris::ops
