#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>

#include "iris/emotion/emotion.hpp"
#include "iris/tts/indextts.hpp"

namespace iris::controller {

int run_dub(const std::string& input, const std::string& model_dir,
            const std::string& onnx_emotion, const std::string& ref_wav,
            const std::string& out_wav) {
    // dub 线路：asr 转写 → emotion 情感分析 → tts 配音合成
    // 当前骨架：asr 已由 run_asr 完成（单独调用），此处串联 emotion + tts。
    std::string err;

    // 1. 情感分析（对输入音频）
    emotion::EmotionResult emo;
    if (!onnx_emotion.empty()) {
        if (!emotion::analyze_emotion(onnx_emotion, input, emo, err)) {
            std::cerr << "dub: 情感分析失败：" << err << "\n";
            return 1;
        }
        std::cout << "dub: A/D/V = " << emo.arousal << "/" << emo.dominance << "/"
                  << emo.valence << "\n";
    }

    // 2. TTS 合成（文本由上层 IR 传入，此处骨架占位）
    std::cout << "dub: 串联完成（asr → emotion → tts）；完整编排待 TTS 引擎就绪\n";
    (void)model_dir; (void)ref_wav; (void)out_wav;
    return 0;
}

} // namespace iris::controller
