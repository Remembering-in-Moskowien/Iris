#pragma once

#include <memory>

#include "iris/core/operator.hpp"
#include "iris/core/provider.hpp"
#include "iris/text/punctuate.hpp"

namespace iris::ops {

/// 对应 C# Transcribe：通过 ASR 提供者转写音频为句子。
class Transcribe : public core::Operator {
public:
    explicit Transcribe(std::shared_ptr<core::IAsrProvider> provider,
                        std::shared_ptr<text::Punctuator> punctuator = nullptr)
        : provider_(std::move(provider)), punctuator_(std::move(punctuator)) {}

    [[nodiscard]] std::string name() const override { return "transcribe"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"audio_preprocess"};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    std::shared_ptr<core::IAsrProvider> provider_;
    std::shared_ptr<text::Punctuator> punctuator_; ///< 可选：给识别结果补标点
};

} // namespace iris::ops
