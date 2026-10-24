#include "iris/tts/kernels.hpp"

#include <algorithm>
#include <cmath>

namespace iris::tts {

void matvec(const float* w, std::size_t rows, std::size_t cols, const float* x, float* y) {
    for (std::size_t r = 0; r < rows; ++r) {
        const float* row = w + r * cols;
        float sum = 0.0f;
        for (std::size_t c = 0; c < cols; ++c) sum += row[c] * x[c];
        y[r] = sum;
    }
}

void add_inplace(float* dst, const float* src, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) dst[i] += src[i];
}

void rmsnorm(float* out, const float* x, const float* weight, std::size_t n, float eps) {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += static_cast<double>(x[i]) * x[i];
    const float scale = 1.0f / std::sqrt(static_cast<float>(sum / static_cast<double>(n)) + eps);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = x[i] * scale * (weight ? weight[i] : 1.0f);
    }
}

void silu_inplace(float* x, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = x[i] / (1.0f + std::exp(-x[i]));
    }
}

void softmax_inplace(float* x, std::size_t n) {
    if (n == 0) return;
    const float mx = *std::max_element(x, x + n);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = std::exp(x[i] - mx);
        sum += x[i];
    }
    const float inv = sum > 0.0 ? static_cast<float>(1.0 / sum) : 0.0f;
    for (std::size_t i = 0; i < n; ++i) x[i] *= inv;
}

void rope_interleaved(float* vec, int n_heads, int head_dim, int pos, float theta) {
    if (n_heads <= 0 || head_dim < 2 || (head_dim % 2) != 0) return;
    const int half = head_dim / 2;
    for (int h = 0; h < n_heads; ++h) {
        float* v = vec + static_cast<std::size_t>(h) * static_cast<std::size_t>(head_dim);
        for (int i = 0; i < half; ++i) {
            // theta_i = pos / theta^(2i / head_dim)
            const float exponent = static_cast<float>(2 * i) / static_cast<float>(head_dim);
            const float angle = static_cast<float>(pos) / std::pow(theta, exponent);
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const float x0 = v[2 * i];
            const float x1 = v[2 * i + 1];
            v[2 * i] = x0 * c - x1 * s;
            v[2 * i + 1] = x0 * s + x1 * c;
        }
    }
}

void rope_mrope(float* vec, int n_heads, int head_dim, const int pos[3], const int sections[3],
                float theta) {
    if (n_heads <= 0 || head_dim < 2 || (head_dim % 2) != 0) return;
    const int half = head_dim / 2;

    // 每对 i 属于哪一路位置：按 sections 的累计边界切分。
    int stream_of[512];
    if (half > 512) return;
    const int total = sections[0] + sections[1] + sections[2];
    const bool split = (total == half);
    for (int i = 0; i < half; ++i) {
        if (!split) {
            stream_of[i] = 0; // 配置不一致时退化为单段（等价于普通 RoPE）
        } else if (i < sections[0]) {
            stream_of[i] = 0;
        } else if (i < sections[0] + sections[1]) {
            stream_of[i] = 1;
        } else {
            stream_of[i] = 2;
        }
    }

    for (int h = 0; h < n_heads; ++h) {
        float* v = vec + static_cast<std::size_t>(h) * static_cast<std::size_t>(head_dim);
        for (int i = 0; i < half; ++i) {
            const float exponent = static_cast<float>(2 * i) / static_cast<float>(head_dim);
            const float angle = static_cast<float>(pos[stream_of[i]]) / std::pow(theta, exponent);
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const float x0 = v[2 * i];
            const float x1 = v[2 * i + 1];
            v[2 * i] = x0 * c - x1 * s;
            v[2 * i + 1] = x0 * s + x1 * c;
        }
    }
}

void rmsnorm_heads(float* vec, int n_heads, int head_dim, const float* weight, float eps) {
    for (int h = 0; h < n_heads; ++h) {
        float* v = vec + static_cast<std::size_t>(h) * static_cast<std::size_t>(head_dim);
        rmsnorm(v, v, weight, static_cast<std::size_t>(head_dim), eps);
    }
}

} // namespace iris::tts
