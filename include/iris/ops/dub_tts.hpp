#pragma once

#include <string>
#include <utility>
#include <vector>

#include "iris/core/operator.hpp"
#include "iris/tts/tts.hpp"

namespace iris::ops {

/// 对应 C# Dub 的合成段：把 IR 里的句子逐条 TTS，并按起始时间码拼成一条配音音轨。
///
/// 输入：IR state.tracks 中的句子（优先 translatedText，回退 text）。
/// 输出：单条 WAV（state.dubbedAudioPath），采样率取 TTS 引擎原生采样率。
///
/// 时长对齐（默认开启）：合成结果先裁掉首尾静音，再压到原句时隙内
/// （自研 WSOLA 变速不变调，最多 max_speed 倍）；压到上限仍放不下的部分，
/// 让下一句顺延，保证句间始终留有 gap_ms 的停顿，且不截断语音。
class DubTts : public core::Operator {
public:
    struct Options {
        tts::TtsRequest request;   ///< 引擎、模型、音色、语言等（text 由算子逐句填充）
        std::string output_path;   ///< 空 = <work_dir>/dub.wav
        bool after_asr = true;     ///< 上游是否挂了 asr 链路（决定 DAG 依赖）
        bool fit_timing = true;    ///< 按原句时间码对齐时长
        int gap_ms = 200;          ///< 句间最小停顿（ms）
        float max_speed = 1.2f;    ///< 时长压缩的变速上限
    };

    explicit DubTts(Options opts) : opts_(std::move(opts)) {}

    [[nodiscard]] std::string name() const override { return "dub_tts"; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return opts_.after_asr ? std::vector<std::string>{"sentence_split"}
                               : std::vector<std::string>{};
    }
    bool execute(core::PipelineContext& ctx) override;

private:
    Options opts_;
};

} // namespace iris::ops
