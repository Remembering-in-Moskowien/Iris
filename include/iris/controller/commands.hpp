#pragma once

#include <string>

#include "iris/asr/diarize.hpp"
#include "iris/asr/separate.hpp"

namespace iris::controller {

/// 版本号（Model/IR generator.version 与 --version 共用）。
/// CI 通过 IRIS_BUILD_NUMBER 注入构建号（release tag = v{run_number}）；本地默认 0.1.0。
#ifndef IRIS_VERSION_STRING
#define IRIS_VERSION_STRING "0.1.0"
#endif
inline constexpr const char* kVersion = IRIS_VERSION_STRING;

// MVC Controller 层：每个子命令一个处理器，负责编排 Model 与 Service（ops），
// 并把结果交给 View（渲染/控制台）。返回进程退出码（0 成功，非 0 失败）。

/// asr：媒体 → IR（ffmpeg_convert → audio_preprocess → transcribe → sentence_split）。
/// diarize 非空时，在分句之后追加说话人分割阶段。
int run_asr(const std::string& input, const std::string& language, const std::string& engine,
            const std::string& model, const std::string& output,
            const asr::DiarizeOptions* diarize = nullptr, int n_threads = 0,
            const asr::SeparateOptions* separate = nullptr,
            const std::string& punc_model = {});

/// build：IR → ASS/SRT/TXT（View 层渲染）。
int run_build(const std::string& ir_path, const std::string& format, const std::string& output);

/// validate：校验 IR 文件。
int run_validate(const std::string& ir_path);

/// doctor：环境诊断（外部媒体工具 + 内嵌 ASR/TTS 后端可用性）。
int run_doctor();

/// models list：列出已注册提供者。
int run_models_list();

/// vsf：视频硬字幕帧定位（自实现，ROI 由用户传精准绝对坐标，无外部 VSF 依赖）。
int run_vsf(const std::string& input, const std::string& output,
             int roi_x, int roi_y, int roi_w, int roi_h, double sample_fps);

/// tts 命令参数（文本 → 语音；引擎无关）。
struct TtsOptions {
    std::string engine = "qwen3-tts";
    std::string model;  ///< qwen3-tts: talker GGUF；indextts: ONNX 模型目录
    std::string codec;  ///< qwen3-tts 必需的 tokenizer/codec GGUF
    std::string text;
    std::string ref_wav;
    std::string ref_text;
    std::string language;
    std::string out_wav = "tts.wav";
    int steps = 0;
    int n_threads = 0; ///< 0 = 自动（min(8, 核数)）
    float cfg_scale = 0.0f;
};

/// tts：文本 → 语音（qwen3-tts / indextts）。
int run_tts(const TtsOptions& opts);

/// tts --inspect：用自研 GGUF 解析器打印模型结构（元数据 + 张量分组），不合成。
int run_tts_inspect(const std::string& model_path);

/// dub 命令参数（媒体/IR → 转写/情感 → 逐句 TTS → 配音音轨）。
struct DubOptions {
    std::string input;
    std::string engine = "qwen3-tts";
    std::string model;
    std::string codec;
    std::string ref_wav;
    std::string ref_text;
    std::string language = "en";
    std::string asr_engine = "whisper";
    std::string asr_model;
    std::string emotion_model;
    std::string out_wav = "dub.wav";
    std::string ir_out; ///< 非空则把本次 IR 另存一份
    int steps = 0;
    int n_threads = 0; ///< 0 = 自动（min(8, 核数)）
    float cfg_scale = 0.0f;
    bool fit_timing = true; ///< 按原句时隙对齐时长（--no-fit 关闭）
    int gap_ms = 200;       ///< 句间最小停顿
    float max_speed = 1.2f; ///< 时长压缩的变速上限
};

/// dub：配音线路串联（asr → emotion → tts）。
int run_dub(const DubOptions& opts);

/// translate 命令参数（IR/媒体 → 逐句翻译 → 新增 translation 轨道）。
struct TranslateCommandOptions {
    std::string input;                         ///< IR（*.json）或媒体文件
    std::string model;                         ///< 翻译模型 GGUF
    std::string engine = "auto";               ///< auto | m2m100 | opus-mt | wmt21 | madlad
    std::string from = "en";
    std::string to = "zh";
    std::string out_ir;                        ///< 非空则把结果 IR 存到该路径
    std::string asr_engine = "whisper";        ///< 媒体输入时使用
    std::string asr_model;
    int beam_size = 5;
    int max_tokens = 256;
};

/// translate：机器翻译（非 LLM：OPUS-MT/Marian、M2M100）。
int run_translate(const TranslateCommandOptions& opts);

/// correct：IR 文本/时间码校正（空白、标点、空句、反序与重叠时间码）。
int run_correct(const std::string& ir_path, const std::string& out_path);

/// emotion：语音情感分析（A/D/V 三维，Wav2Small ONNX）。
int run_emotion(const std::string& onnx_path, const std::string& wav_path);

} // namespace iris::controller
