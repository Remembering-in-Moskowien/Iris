#pragma once

#include <memory>

#include "iris/core/operator.hpp"
#include "iris/core/provider.hpp"

namespace iris::ops {

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

} // namespace iris::ops
