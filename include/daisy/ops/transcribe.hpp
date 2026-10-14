#pragma once

#include <memory>

#include "daisy/core/operator.hpp"
#include "daisy/core/provider.hpp"

namespace daisy::ops {

/// 对应 C# Transcribe：通过 ASR 提供者转写音频为句子。
class Transcribe : public core::Operator {
public:
    explicit Transcribe(std::shared_ptr<core::IAsrProvider> provider)
        : provider_(std::move(provider)) {}

    [[nodiscard]] std::string name() const override { return "transcribe"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"audio_preprocess"};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    std::shared_ptr<core::IAsrProvider> provider_;
};

} // namespace daisy::ops
