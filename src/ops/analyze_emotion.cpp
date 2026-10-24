#include "iris/ops/analyze_emotion.hpp"

#include <string>

#include "iris/emotion/emotion.hpp"
#include "iris/infra/log.hpp"

namespace iris::ops {

bool AnalyzeEmotion::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    const auto& st = ctx.doc->state;

    // 情感模型吃 16 kHz 单声道 WAV —— 正是 audio_preprocess 的产物。
    const std::string* audio = st.preprocessed_audio_path
                                   ? &*st.preprocessed_audio_path
                                   : (st.converted_audio_path ? &*st.converted_audio_path : nullptr);
    if (!audio) {
        spdlog::warn("analyze_emotion: 没有可分析的音频产物，跳过");
        return true;
    }

    emotion::EmotionResult result;
    std::string error;
    if (!emotion::analyze_emotion(onnx_path_, *audio, result, error)) {
        spdlog::error("analyze_emotion: {}", error);
        return false;
    }

    spdlog::info("analyze_emotion: {} ({:.1f}%)", result.label, result.score * 100.0f);
    ctx.record_provenance(name(), onnx_path_);
    return true;
}

} // namespace iris::ops
