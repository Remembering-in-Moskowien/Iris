#pragma once

#include "daisy/core/operator.hpp"

namespace daisy::ops {

/// 对应 C# SentenceSplit：按标点/长度切分句子（rule 策略）。
class SentenceSplit : public core::Operator {
public:
    [[nodiscard]] std::string name() const override { return "sentence_split"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"transcribe"};
    }
    bool execute(core::PipelineContext& ctx) override;
};

} // namespace daisy::ops
