#pragma once

#include <cstddef>
#include <cstdint>

namespace iris::tts {

// ---------------------------------------------------------------------------
// 自研运行时的数值内核（行主序 float32、单线程、无依赖）。
// 只实现 Qwen3-TTS 前向真正用到的算子，每个都有单测锁行为。
// ---------------------------------------------------------------------------

/// y[rows] = W[rows × cols] · x[cols]（W 行连续，正好匹配 GGUF 的 [cols, rows] 布局）。
void matvec(const float* w, std::size_t rows, std::size_t cols, const float* x, float* y);

/// dst += src（逐元素）。
void add_inplace(float* dst, const float* src, std::size_t n);

/// RMSNorm：out = x / sqrt(mean(x²) + eps) * weight。
void rmsnorm(float* out, const float* x, const float* weight, std::size_t n, float eps);

/// SiLU（逐元素）：x * sigmoid(x)。
void silu_inplace(float* x, std::size_t n);

/// 数值稳定的 softmax（原地）。
void softmax_inplace(float* x, std::size_t n);

/// 交错式 RoPE（rope_interleaved=1）：对每个 head 的 (2i, 2i+1) 成对旋转。
/// vec 布局为 n_heads × head_dim，pos 为该 token 的位置编号。
void rope_interleaved(float* vec, int n_heads, int head_dim, int pos, float theta);

} // namespace iris::tts
