#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "iris/tts/gguf.hpp"
#include "iris/tts/tensor.hpp"

namespace iris::tts {

// ---------------------------------------------------------------------------
// Qwen3-TTS 的 talker（自研推理，P2）。
//
//   文本 token → token_embd_text(2048) → fc1+SiLU+fc2 → 1024 ┐
//   音频 token → token_embd(1024) ─────────────────────────────┤→ 28 层 Qwen3
//   （GQA 16/8 heads、head_dim 128、q/k 单头 RMSNorm、交错 mRoPE、SwiGLU）
//   → output_norm → output(1024→3072) → 下一个音频码的 logits。
//
// 位置：mRoPE 分三段（mrope_section = [24,20,20]），每段用各自的位置分量。
// 权重按 GGUF 原始量化字节驻留，逐行惰性反量化（不做 f32 展开）。
// ---------------------------------------------------------------------------

struct TalkerConfig {
    int n_layers = 28;
    int d_model = 1024;
    int n_heads = 16;
    int n_kv_heads = 8;
    int head_dim = 128;
    int ff_dim = 3072;
    int vocab = 3072;
    int text_vocab = 151936;
    int text_hidden = 2048;
    float rope_theta = 1e6f;
    float rms_eps = 1e-6f;
    std::array<int, 3> mrope = {24, 20, 20};
    int position_id_per_seconds = 13;

    /// 从 GGUF 元数据读配置；缺项保留默认值。
    static TalkerConfig from_gguf(const gguf::File& index);
};

class Talker {
public:
    /// 一个输入 token：文本（走 text 词表）或音频（走 codec 词表）+ 三路位置。
    struct Item {
        bool is_text = false;
        std::int32_t id = 0;
        std::array<std::int32_t, 3> pos{0, 0, 0};
    };

    Talker() = default;

    /// 加载权重。max_layers > 0 时只加载前若干层（测试/调试用，省内存）。
    /// with_text_embedding=false 可跳过 151936×2048 的大文本词表。
    bool load(const std::string& gguf_path, std::string& error, int max_layers = 0,
              bool with_text_embedding = true);

    [[nodiscard]] const TalkerConfig& config() const { return cfg_; }
    [[nodiscard]] int n_loaded_layers() const { return static_cast<int>(layers_.size()); }
    [[nodiscard]] std::size_t seq_len() const { return cache_len_; }

    /// 清空 KV cache（开始一段新序列）。
    void reset();

    /// 前向一批 token，返回最后一个位置的 logits（长度 = vocab）。
    bool forward(const std::vector<Item>& items, std::vector<float>& logits, std::string& error);

private:
    struct Layer {
        std::vector<float> attn_norm, ffn_norm, q_norm, k_norm; ///< 小张量，直接 f32
        RawTensor wq, wk, wv, wo;
        RawTensor gate, up, down;
        std::vector<float> k_cache; ///< [pos][n_kv_heads * head_dim]
        std::vector<float> v_cache;
    };

    bool embed(const Item& item, std::vector<float>& x, std::string& error);
    bool run_layer(Layer& layer, std::vector<float>& x, const Item& item, std::string& error);
    bool project_output(const std::vector<float>& x, std::vector<float>& logits,
                        std::string& error);

    TalkerConfig cfg_;
    WeightFile weights_;
    bool loaded_ = false;
    bool has_text_embedding_ = false;
    std::size_t cache_len_ = 0;

    RawTensor token_embd_;      ///< [1024, 3072]
    RawTensor token_embd_text_; ///< [2048, 151936]
    RawTensor text_fc1_w_, text_fc1_b_, text_fc2_w_, text_fc2_b_;
    std::vector<float> fc1_bias_, fc2_bias_; ///< 反量化后的小偏置
    RawTensor output_norm_, output_head_;
    std::vector<float> output_norm_w_;
    std::vector<Layer> layers_;
};

} // namespace iris::tts
