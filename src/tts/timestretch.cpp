#include "iris/tts/timestretch.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace iris::tts {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFrameSeconds = 0.040; ///< 分析/合成长度 40 ms
constexpr int kSearchStep = 2;          ///< 相关性搜索的采样步长

std::size_t round_even(double v) {
    auto n = static_cast<std::size_t>(std::max(8.0, std::round(v)));
    return (n / 2) * 2;
}

} // namespace

bool wsola_stretch(const std::vector<float>& in, int sample_rate, float speed,
                   std::vector<float>& out, std::string& error) {
    if (sample_rate <= 0) {
        error = "wsola: 采样率无效";
        return false;
    }
    if (in.empty() || speed <= 1.0001f) {
        out = in;
        return true;
    }

    const std::size_t frame = round_even(kFrameSeconds * sample_rate);
    if (in.size() < frame * 2) { // 太短就原样返回，压缩交给调用方的时隙控制
        out = in;
        return true;
    }

    const std::size_t hop_s = frame / 2;                  // 合成跳距
    const std::size_t hop_a =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(hop_s * speed)));
    const std::size_t overlap = frame - hop_s;            // 重叠区长度
    const std::size_t tol = std::max<std::size_t>(1, frame / 4); // 相关性搜索半径

    std::vector<float> win(frame);
    for (std::size_t i = 0; i < frame; ++i) {
        win[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / (frame - 1))));
    }

    const std::size_t out_len =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(
                                     static_cast<double>(in.size()) / speed)));
    out.assign(out_len, 0.0f);
    std::vector<float> weight(out_len, 0.0f);

    std::vector<float> prev_tail; // 上一帧尾部的原始波形（自然延续的参照）

    for (std::size_t k = 0;; ++k) {
        const std::size_t synth_at = k * hop_s;
        if (synth_at >= out_len) break;

        std::size_t nominal = k * hop_a;
        if (nominal + frame > in.size()) {
            if (in.size() < frame) break;
            nominal = in.size() - frame; // 末帧贴住结尾，避免越界
        }

        // 在 [nominal - tol, nominal + tol] 内找与上一帧尾部最相似的起点（WSOLA 的核心）。
        std::size_t best = nominal;
        if (!prev_tail.empty()) {
            float best_score = -1.0f;
            const long long lo = static_cast<long long>(nominal) - static_cast<long long>(tol);
            const long long hi = static_cast<long long>(nominal) + static_cast<long long>(tol);
            for (long long cand = lo; cand <= hi; cand += kSearchStep) {
                if (cand < 0) continue;
                const auto p = static_cast<std::size_t>(cand);
                if (p + frame > in.size()) break;

                float score = 0.0f;
                for (std::size_t i = 0; i < overlap; ++i) {
                    score += in[p + i] * prev_tail[i];
                }
                if (score > best_score) {
                    best_score = score;
                    best = p;
                }
            }
            if (best_score <= 0.0f) best = nominal; // 找不到正相关就退回名义位置
        }

        // 叠加窗函数帧 + 权重（端点处权重不足时由归一化兜住）。
        for (std::size_t i = 0; i < frame; ++i) {
            const std::size_t dst = synth_at + i;
            if (dst >= out_len) break;
            const float w = win[i];
            out[dst] += in[best + i] * w;
            weight[dst] += w;
        }

        // 记录本帧尾部，作为下一帧相关性搜索的参照。
        prev_tail.assign(in.begin() + static_cast<std::ptrdiff_t>(best + hop_s),
                         in.begin() + static_cast<std::ptrdiff_t>(best + frame));
    }

    // 权重归一：Hann 窗 50% 重叠时权重和约为 1，端点处不足则按实际权重补回。
    // 权重几乎为 0 的端点保持上一个样本，避免结尾突变出爆音。
    constexpr float kMinWeight = 0.05f;
    float held = 0.0f;
    for (std::size_t i = 0; i < out_len; ++i) {
        if (weight[i] > kMinWeight) {
            out[i] /= weight[i];
            held = out[i];
        } else {
            out[i] = held;
        }
    }
    return true;
}

} // namespace iris::tts
