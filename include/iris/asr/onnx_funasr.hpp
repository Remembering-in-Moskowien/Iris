#pragma once

#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::asr {

// ---------------------------------------------------------------------------
// FunASR 系 ONNX 转录后端（SenseVoice / Paraformer）。
//
// 两者都是**非自回归 CTC**，前处理完全相同：
//   16 kHz PCM → kaldi fbank 80 维（povey 窗/预加重 0.97）→ LFR(7,6) 拼 560 维
//   → CMVN（模型 metadata 自带 neg_mean / inv_stddev）→ 一次前向 → CTC 贪心
//
// 与 whisper-onnx 一样，推理在 onnxruntime 进程内完成，不依赖外部可执行文件。
// ---------------------------------------------------------------------------

enum class FunasrModel { SenseVoice, Paraformer };

struct FunasrOptions {
    std::string model_dir; ///< 含 model.int8.onnx / model.onnx + tokens.txt
    std::string language;  ///< SenseVoice 语言：auto/zh/en/yue/ja/ko（Paraformer 仅中文）
    int n_threads = 0;     ///< 0 = infra::resolve_threads()
};

/// 转写 16 kHz 单声道 WAV。成功时 out 里放一句覆盖整段音频的 Sentence
/// （细节分句交给后续 SentenceSplit）。
bool transcribe_funasr(FunasrModel kind, const FunasrOptions& options, const std::string& wav16k_path,
                       std::vector<model::Sentence>& out, std::string& error);

// ---- 便于单测的纯函数 ------------------------------------------------------
namespace detail {

/// kaldi 风格 fbank：80 mel、25 ms/10 ms、povey 窗、预加重 0.97、snip_edges=false。
/// 返回行主序 [帧数 × 80]。
std::vector<float> kaldi_fbank80(const std::vector<float>& pcm16k);

/// LFR：m 帧拼一帧、步进 n 帧（不足 m 时用首/尾帧补齐），返回 [帧数 × (80*m)]。
std::vector<float> lfr(const std::vector<float>& fbank, int m, int n);

/// 逐维 CMVN：out = (in + neg_mean) * inv_stddev（维度必须匹配，空则原样返回）。
void apply_cmvn(std::vector<float>& features, const std::vector<float>& neg_mean,
                const std::vector<float>& inv_stddev);

/// CTC 贪心解码：去掉 blank(0) 与连续重复，拼成文本。
/// strip_tags=true 时丢弃 `<|...|>` 这类 SenseVoice 控制标签；否则按 Paraformer 处理 `@@`。
std::string ctc_greedy_text(const std::vector<int>& token_ids,
                            const std::vector<std::string>& id_to_token, bool strip_tags);

} // namespace detail

} // namespace iris::asr
