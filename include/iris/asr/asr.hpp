#pragma once

#include <string>
#include <vector>

#include "iris/core/provider.hpp"

namespace iris::asr {

/// ASR 推理后端（模型名 → 运行时 DLL，推理编排代码由 Iris 自写，进程内完成）：
///   - whisper（ggml）       → CrispASR DLL（whisper 后端，tools/runtimes/crispasr）
///   - qwen3-asr（ggml）     → CrispASR DLL（qwen3 后端，tools/runtimes/crispasr）
///   - whisper（transformer）→ CTranslate2 DLL（tools/runtimes/ct2）
///   - whisper（onnx）       → onnxruntime DLL（tools/runtimes/onnx，GPU 可行）
enum class AsrBackend {
    WhisperGgml,   ///< whisper（ggml，CrispASR whisper 后端）
    Qwen3AsrGgml,  ///< qwen3-asr（ggml，CrispASR qwen3 后端）
    WhisperCt2,    ///< whisper（transformer，CTranslate2）
    WhisperOnnx,   ///< whisper（onnx，onnxruntime）
};

/// 将 CLI --engine 字符串解析为后端；未知引擎返回 false。
bool parse_asr_backend(const std::string& engine, AsrBackend& out);

/// 该后端在当前构建中是否可用（对应运行时 DLL 是否就绪）。
/// 由 cmake/dependencies.cmake 探测 tools/runtimes 后注入 IRIS_HAS_CRISPASR / IRIS_HAS_CT2。
bool asr_backend_available(AsrBackend backend);

/// 进程内 ASR 引擎（Controller 层经 ops::Transcribe 使用）。
///
/// whisper(ggml) / qwen3-asr(ggml) 由 CrispASR DLL（ggml 运行时）推理；
/// whisper(transformer) 由 CTranslate2 DLL（ct2 转换模型目录）推理；
/// whisper(onnx) 由 onnxruntime DLL（whisperX/Xenova onnx 模型目录）推理。
/// 音频统一由 iris 自建 read_wav_16k_mono 读取为 16 kHz 单声道 PCM 后送入推理。
class AsrEngine : public core::IAsrProvider {
public:
    AsrEngine(AsrBackend backend, std::string model_path)
        : backend_(backend), model_path_(std::move(model_path)) {}

    [[nodiscard]] std::string name() const override;
    bool transcribe(const std::string& audio_path, const std::string& language,
                    std::vector<model::Sentence>& out, std::string& error) override;

private:
    AsrBackend backend_;
    std::string model_path_;
};

} // namespace iris::asr
