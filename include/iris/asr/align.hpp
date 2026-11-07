#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::asr {

// ---------------------------------------------------------------------------
// 强制对齐（forced alignment）：把 IR 里每句文本对到音频，写 Sentence.words。
//
// 走 CrispASR 的 crispasr_align_words_abi()：
//   - 模型文件名含 "forced-aligner" / "qwen3-fa" / "qwen3-forced" 时用
//     Qwen3-ForcedAligner（0.6B，词级/字级时间戳）；
//   - 其它文件名按 canary-ctc / wav2vec2 路由。
// 对齐器上下文在运行库里按模型路径缓存，所以逐句调用不会反复加载模型。
// ---------------------------------------------------------------------------

struct AlignOptions {
    std::string model;     ///< 对齐模型 GGUF（必填，除非 --dry-run）
    std::string language;  ///< 语言（信息性，便于日志）
    int threads = 0;       ///< 0 = 自动（min(8, 核数)）
    double pad_seconds = 0.2; ///< 每句前后各留一点音频，避免切掉首尾音
    bool dry_run = false;  ///< 只解码 + 报区间，不跑模型
    bool apply_sentence_timings = false; ///< 用有效词级时间覆盖句子起止时间
};

struct AlignStats {
    int sentences = 0;
    int aligned = 0;
    int failed = 0;
    int tokens = 0;
    double audio_seconds = 0.0;
};

struct AudioRange {
    double start = 0.0;
    double end = 0.0;
};

/// 媒体 → 16 kHz 单声道 PCM（ffmpeg 子进程 + WAV 读取）。
bool decode_media_16k(const std::string& media, std::vector<float>& pcm, std::string& error);

/// 单句在音频里的取样区间（秒）；会把 pad 夹到 [0, total]。
AudioRange sentence_range(std::int64_t start_ms, std::int64_t end_ms, std::int64_t total_ms,
                          double pad_seconds);

/// 用有效词级时间更新句子边界；无有效跨度时保持原值并返回 false。
bool apply_word_timings_to_sentence(model::Sentence& sentence, std::int64_t total_ms);

/// 给一个轨道逐句对齐（写 sentence.words）；失败返回 false 并给 error。
bool align_track(model::Track& track, const std::vector<float>& pcm, int sample_rate,
                 const AlignOptions& options, AlignStats& stats, std::string& error);

} // namespace iris::asr
