#pragma once

#include <string>
#include <vector>

#include "iris/core/operator.hpp"
#include "iris/mt/translator.hpp"

namespace iris::ops {

/// 对应 C# Translate：把 IR 里的每句原文翻成目标语言，写入一条 "translation" 轨道。
///
/// 输入：IR state.tracks 中的 "transcribe" 轨道（每句 text）。
/// 输出：新轨道 { type: "translation", language: <target> }，
///       每句保留原文 text，译文写入 translatedText（dub 会优先用它配音）。
/// 依赖 sentence_split（同一管线内跟在转写之后）。
class Translate : public core::Operator {
public:
    struct Options {
        mt::TranslateOptions mt;
        bool after_asr = true; ///< 上游是否挂了 asr 链路（决定 DAG 依赖）
    };

    explicit Translate(Options opts) : opts_(std::move(opts)) {}

    [[nodiscard]] std::string name() const override { return "translate"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return opts_.after_asr ? std::vector<std::string>{"sentence_split"}
                               : std::vector<std::string>{};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    Options opts_;
};

} // namespace iris::ops
