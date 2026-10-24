#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::asr {

// ---------------------------------------------------------------------------
// 说话人分割（speaker diarization）。
//
// 走 CrispASR 的 diarize ABI（`crispasr_diarize_segments_abi`）：调用方给一段
// 时间片段网格，库里给每个片段标 speaker id。算法由 method 决定：
//   4 = FoxNose（WeSpeaker 嵌入 + 谱聚类，推荐；需 embedder GGUF）
//   5 = Sortformer（NeMo 端到端，需 sortformer GGUF）
//   3 = pyannote 分割（实验性：只做分割，跨窗口没有全局说话人一致性）
// ---------------------------------------------------------------------------

/// 一个已标注的时间片段（毫秒）。
struct SpeakerSpan {
    std::int64_t t0_ms = 0;
    std::int64_t t1_ms = 0;
    int speaker = -1;
};

struct DiarizeOptions {
    int method = 4;
    std::string model;    ///< method 3/5 的模型 GGUF（pyannote / sortformer）
    std::string embedder; ///< method 4 的说话人嵌入 GGUF（WeSpeaker resnet34）
    int min_speakers = 1;
    int max_speakers = 8;
    int num_speakers = 0; ///< >0 时固定人数
    int n_threads = 0;
    int grid_ms = 1000; ///< 分割网格粒度（把长句切成细片交给聚类算法）
};

/// 给每句按**时间中点**贴 speaker（`SPEAKER_0`、`SPEAKER_1`…）；句子里有词级信息时一并贴。
/// 音频必须是 16 kHz 单声道 WAV（audio_preprocess 的产物）。
/// 成功后 out_n_speakers 为识别到的说话人数。
bool diarize_sentences(const std::string& wav16k_path, const DiarizeOptions& options,
                       std::vector<model::Sentence>& sentences, int& out_n_speakers,
                       std::string& error);

/// 纯逻辑：取 t_ms 落在的片段的 speaker（-1 = 未命中）。便于单测。
int speaker_at(std::int64_t t_ms, const std::vector<SpeakerSpan>& spans);

/// speaker id → 展示名（SPEAKER_0 …）。
std::string speaker_label(int id);

/// 在 `~/.cache/crispasr` 里找默认的说话人嵌入模型（WeSpeaker / TitaNet），
/// 找不到返回空串（由调用方提示用户传 --speaker-model）。
std::string default_embedder_model();

} // namespace iris::asr
