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
    WhisperGgml,      ///< whisper（ggml，CrispASR whisper 后端）
    Qwen3AsrGgml,     ///< qwen3-asr（ggml，CrispASR qwen3 后端）
    WhisperCt2,       ///< whisper（transformer，CTranslate2）
    WhisperOnnx,      ///< whisper（onnx，onnxruntime）
    SenseVoiceOnnx,   ///< SenseVoice（FunASR，onnxruntime，CTC）
    ParaformerOnnx,   ///< Paraformer（FunASR，onnxruntime，CTC）
};

/// 将 CLI --engine 字符串解析为后端；未知引擎返回 false。
bool parse_asr_backend(const std::string& engine, AsrBackend& out);

/// 该后端在当前构建中是否可用（对应运行时 DLL 是否就绪）。
/// 由 cmake/dependencies.cmake 探测 tools/runtimes 后注入 IRIS_HAS_CRISPASR / IRIS_HAS_CT2。
bool asr_backend_available(AsrBackend backend);

/// 已注册的内置 ASR 后端（按注册顺序；供 plugin list / 诊断使用）。
struct AsrBackendInfo {
    std::string id;                   ///< 规范 ID（CLI --engine 的首选值）
    std::vector<std::string> aliases; ///< 兼容别名
    AsrBackend backend;               ///< 类型安全的内部枚举
    bool available = false;           ///< 本次构建是否带对应运行时
};

/// 枚举静态注册的 ASR 后端。返回副本，调用方可自由排序/过滤。
std::vector<AsrBackendInfo> registered_asr_backends();

/// 进程内 ASR 引擎（Controller 层经 ops::Transcribe 使用）。
///
/// whisper(ggml) / qwen3-asr(ggml) 由 CrispASR DLL（ggml 运行时）推理；
/// whisper(transformer) 由 CTranslate2 DLL（ct2 转换模型目录）推理；
/// whisper(onnx) 由 onnxruntime DLL（whisperX/Xenova onnx 模型目录）推理。
/// 音频统一由 iris 自建 read_wav_16k_mono 读取为 16 kHz 单声道 PCM 后送入推理。
class AsrEngine : public core::IAsrProvider {
public:
    /// n_threads：0 = 自动（硬件并发），>0 = 固定线程数。
    AsrEngine(AsrBackend backend, std::string model_path, int n_threads = 0)
        : backend_(backend), model_path_(std::move(model_path)), n_threads_(n_threads) {}

    [[nodiscard]] std::string name() const override;
    bool transcribe(const std::string& audio_path, const std::string& language,
                    std::vector<model::Sentence>& out, std::string& error) override;

    /// 构造时实际使用的线程数（0 表示由后端决定）。
    [[nodiscard]] int n_threads() const { return n_threads_; }

private:
    AsrBackend backend_;
    std::string model_path_;
    int n_threads_ = 0;
};

} // namespace iris::asr
