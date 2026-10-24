#pragma once

#include <string>

namespace iris::asr {

// ---------------------------------------------------------------------------
// 人声分离（source separation）：把混音拆成 stems，默认只取 vocals。
//
// 走 CrispASR 的 `crispasr_session_separate`（HTDemucs，44.1 kHz 立体声输入，
// 输出 drums/bass/other/vocals 四路）。媒体解码/重采样仍交给 ffmpeg，
// 分离本身在进程内完成。
// ---------------------------------------------------------------------------

struct SeparateOptions {
    std::string model;      ///< htdemucs GGUF
    std::string stem = "vocals"; ///< 要取出的 stem（vocals/drums/bass/other）
    int n_threads = 0;
    int chunk_seconds = 16; ///< 分块长度（重叠 1 s）；更小块=更多并行、更多边界拼接
};

/// 分离指定 stem，输出 16 kHz 单声道 WAV（下游 ASR/TTS 统一吃这个格式）。
/// input 可以是任意媒体（内部先转 44.1 kHz 立体声）。
bool separate_stem(const std::string& input_media, const SeparateOptions& options,
                   const std::string& out_wav16k, std::string& error);

/// 在 ~/.cache/crispasr 里找默认的 htdemucs 模型；找不到返回空串。
std::string default_separation_model();

} // namespace iris::asr
