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

/// 多模态 RoPE（mRoPE）：把 head_dim/2 个旋转对按 sections 切成多段，
/// 每段用各自的 position 分量（Qwen3-TTS 的 mrope_section = [24,20,20]）。
/// sections 之和必须等于 head_dim/2；否则退化为单段。
void rope_mrope(float* vec, int n_heads, int head_dim, const int pos[3], const int sections[3],
                float theta);

/// 单头 RMSNorm（在原地对每个 head 的 head_dim 维做归一）。
void rmsnorm_heads(float* vec, int n_heads, int head_dim, const float* weight, float eps);

} // namespace iris::tts
