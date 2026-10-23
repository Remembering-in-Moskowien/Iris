#pragma once

#include <string>

namespace iris::emotion {

/// 情感分析结果（A/D/V 三维连续值，0~1）。
struct EmotionResult {
    float arousal = 0.0f;    // 唤醒度（平静↔激动）
    float dominance = 0.0f;  // 支配度（顺从↔主导）
    float valence = 0.0f;    // 效价（消极↔积极）
};

/// 语音情感分析（Wav2Small 风格 ONNX，72K 参数，60KB 量化）。
/// 输入 16kHz 单声道 WAV；输出 A/D/V 三维。
/// 运行时引用 tools/runtimes/onnx 打包的 onnxruntime DLL。
bool analyze_emotion(const std::string& onnx_path,
                     const std::string& wav_path,
                     EmotionResult& out,
                     std::string& error);

} // namespace iris::emotion
