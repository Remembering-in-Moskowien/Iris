#pragma once

#include "daisy/core/operator.hpp"

namespace daisy::ops {

/// 对应 C# build 命令：把 IR 中的句子渲染为 ASS/SRT/TXT。
class SubtitleBuild : public core::Operator {
public:
    [[nodiscard]] std::string name() const override { return "build"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return {"sentence_split"};
    }
    bool execute(core::PipelineContext& ctx) override;
};

} // namespace daisy::ops
