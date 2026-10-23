#pragma once

#include <string>

namespace iris::emotion {

/// 情感分析结果（6 类分类）。
struct EmotionResult {
    std::string label;       // SAD / ANGRY / DISGUST / FEAR / HAPPY / NEUTRAL
    float score = 0.0f;      // softmax 置信度
    float scores[6] = {};    // 各类置信度（按 id2label 顺序）
};

/// 语音情感分类（wav2vec2-base, DunnBC22 微调, int8 ONNX, ~95MB）。
/// 输入 16kHz 单声道 WAV；输出 6 类情绪 softmax。
/// 运行时引用 tools/runtimes/onnx 打包的 onnxruntime DLL。
bool analyze_emotion(const std::string& onnx_path,
                     const std::string& wav_path,
                     EmotionResult& out,
                     std::string& error);

} // namespace iris::emotion
