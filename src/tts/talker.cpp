#include "iris/tts/talker.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "iris/infra/log.hpp"
#include "iris/tts/kernels.hpp"

namespace iris::tts {

namespace {

/// 取小张量（偏置/归一化权重）并反量化为 f32。
bool load_small(WeightFile& wf, const std::string& name, std::vector<float>& out,
                std::string& error) {
    Tensor t;
    if (!wf.load(name, t, error)) return false;
    out = std::move(t.data);
    return true;
}

} // namespace

TalkerConfig TalkerConfig::from_gguf(const gguf::File& index) {
    TalkerConfig c;
    auto geti = [&](const char* key, int fallback) {
        return static_cast<int>(index.get_int(key, fallback));
    };
    c.n_layers = geti("qwen3tts.talker.n_layers", c.n_layers);
    c.d_model = geti("qwen3tts.talker.d_model", c.d_model);
    c.n_heads = geti("qwen3tts.talker.n_heads", c.n_heads);
    c.n_kv_heads = geti("qwen3tts.talker.n_kv_heads", c.n_kv_heads);
    c.head_dim = geti("qwen3tts.talker.head_dim", c.head_dim);
    c.ff_dim = geti("qwen3tts.talker.ff_dim", c.ff_dim);
    c.vocab = geti("qwen3tts.talker.vocab_size", c.vocab);
    c.text_vocab = geti("qwen3tts.talker.text_vocab_size", c.text_vocab);
    c.text_hidden = geti("qwen3tts.talker.text_hidden_size", c.text_hidden);
    c.rope_theta = static_cast<float>(index.get_float("qwen3tts.talker.rope_theta", c.rope_theta));
    c.rms_eps = static_cast<float>(index.get_float("qwen3tts.talker.rms_norm_eps", c.rms_eps));
    c.position_id_per_seconds =
        geti("qwen3tts.talker.position_id_per_seconds", c.position_id_per_seconds);

    if (const gguf::MetaValue* v = index.find("qwen3tts.talker.mrope_section");
        v != nullptr && v->ints.size() == 3) {
        c.mrope = {static_cast<int>(v->ints[0]), static_cast<int>(v->ints[1]),
                   static_cast<int>(v->ints[2])};
    }
    return c;
}

bool Talker::load(const std::string& gguf_path, std::string& error, int max_layers,
                  bool with_text_embedding) {
    layers_.clear();
    loaded_ = false;
    cache_len_ = 0;

    if (!weights_.open(gguf_path, error)) return false;
    cfg_ = TalkerConfig::from_gguf(weights_.index());

    const int want_layers = max_layers > 0 ? std::min(max_layers, cfg_.n_layers) : cfg_.n_layers;

    if (!weights_.load_raw("talker.token_embd.weight", token_embd_, error)) return false;
    if (!weights_.load_raw("talker.output.weight", output_head_, error)) return false;
    if (!load_small(weights_, "talker.output_norm.weight", output_norm_w_, error)) return false;

    has_text_embedding_ = false;
    if (with_text_embedding) {
        if (!weights_.load_raw("talker.token_embd_text.weight", token_embd_text_, error) ||
            !weights_.load_raw("talker.text_proj.fc1.weight", text_fc1_w_, error) ||
            !weights_.load_raw("talker.text_proj.fc2.weight", text_fc2_w_, error) ||
            !load_small(weights_, "talker.text_proj.fc1.bias", fc1_bias_, error) ||
            !load_small(weights_, "talker.text_proj.fc2.bias", fc2_bias_, error)) {
            return false;
        }
        has_text_embedding_ = true;
    }

    layers_.resize(static_cast<std::size_t>(want_layers));
    for (int i = 0; i < want_layers; ++i) {
        Layer& l = layers_[static_cast<std::size_t>(i)];
        const std::string p = "talker.blk." + std::to_string(i) + ".";
        if (!load_small(weights_, p + "attn_norm.weight", l.attn_norm, error) ||
            !load_small(weights_, p + "ffn_norm.weight", l.ffn_norm, error) ||
            !load_small(weights_, p + "attn_q_norm.weight", l.q_norm, error) ||
            !load_small(weights_, p + "attn_k_norm.weight", l.k_norm, error) ||
            !weights_.load_raw(p + "attn_q.weight", l.wq, error) ||
            !weights_.load_raw(p + "attn_k.weight", l.wk, error) ||
            !weights_.load_raw(p + "attn_v.weight", l.wv, error) ||
            !weights_.load_raw(p + "attn_output.weight", l.wo, error) ||
            !weights_.load_raw(p + "ffn_gate.weight", l.gate, error) ||
            !weights_.load_raw(p + "ffn_up.weight", l.up, error) ||
            !weights_.load_raw(p + "ffn_down.weight", l.down, error)) {
            return false;
        }
    }

    loaded_ = true;
    spdlog::info(
        "talker: 加载完成 —— {} 层 / d_model {} / {} heads（kv {}）/ head_dim {} / vocab {}{}",
        want_layers, cfg_.d_model, cfg_.n_heads, cfg_.n_kv_heads, cfg_.head_dim, cfg_.vocab,
        has_text_embedding_ ? "（含文本词表）" : "");
    return true;
}

void Talker::reset() {
    cache_len_ = 0;
    for (auto& l : layers_) {
        l.k_cache.clear();
        l.v_cache.clear();
    }
}

bool Talker::embed(const Item& item, std::vector<float>& x, std::string& error) {
    x.assign(static_cast<std::size_t>(cfg_.d_model), 0.0f);

    if (!item.is_text) {
        if (item.id < 0 || item.id >= cfg_.vocab) {
            error = "talker: 音频 token 越界 " + std::to_string(item.id);
            return false;
        }
        return row_to_float(token_embd_, static_cast<std::size_t>(item.id), x.data(), error);
    }

    if (!has_text_embedding_) {
        error = "talker: 文本通道未加载（load 时 with_text_embedding=false）";
        return false;
    }
    if (item.id < 0 || item.id >= cfg_.text_vocab) {
        error = "talker: 文本 token 越界 " + std::to_string(item.id);
        return false;
    }

    // 文本：token_embd_text[2048] → fc1(2048) → SiLU → fc2(1024)
    const auto hidden = static_cast<std::size_t>(cfg_.text_hidden);
    std::vector<float> h(hidden);
    if (!row_to_float(token_embd_text_, static_cast<std::size_t>(item.id), h.data(), error)) {
        return false;
    }
    std::vector<float> mid(hidden);
    if (!matvec_raw(text_fc1_w_, h.data(), mid.data(), error)) return false;
    for (std::size_t i = 0; i < mid.size(); ++i) mid[i] += fc1_bias_[i];
    silu_inplace(mid.data(), mid.size());

    if (!matvec_raw(text_fc2_w_, mid.data(), x.data(), error)) return false;
    for (std::size_t i = 0; i < x.size(); ++i) x[i] += fc2_bias_[i];
    return true;
}

bool Talker::run_layer(Layer& layer, std::vector<float>& x, const Item& item, std::string& error) {
    const int d = cfg_.d_model;
    const int nh = cfg_.n_heads;
    const int nkv = cfg_.n_kv_heads;
    const int hd = cfg_.head_dim;
    const int q_dim = nh * hd;
    const int kv_dim = nkv * hd;
    const int group = nh / nkv;
    const float eps = cfg_.rms_eps;

    std::vector<float> hn(static_cast<std::size_t>(d));
    std::vector<float> q(static_cast<std::size_t>(q_dim));
    std::vector<float> k(static_cast<std::size_t>(kv_dim));
    std::vector<float> v(static_cast<std::size_t>(kv_dim));

    // ---- 注意力前段：norm → q/k/v → 单头 norm → mRoPE ----
    rmsnorm(hn.data(), x.data(), layer.attn_norm.data(), hn.size(), eps);
    if (!matvec_raw(layer.wq, hn.data(), q.data(), error)) return false;
    if (!matvec_raw(layer.wk, hn.data(), k.data(), error)) return false;
    if (!matvec_raw(layer.wv, hn.data(), v.data(), error)) return false;

    rmsnorm_heads(q.data(), nh, hd, layer.q_norm.data(), eps);
    rmsnorm_heads(k.data(), nkv, hd, layer.k_norm.data(), eps);
    rope_mrope(q.data(), nh, hd, item.pos.data(), cfg_.mrope.data(), cfg_.rope_theta);
    rope_mrope(k.data(), nkv, hd, item.pos.data(), cfg_.mrope.data(), cfg_.rope_theta);

    // ---- 追加 KV 并做注意力（GQA）----
    layer.k_cache.insert(layer.k_cache.end(), k.begin(), k.end());
    layer.v_cache.insert(layer.v_cache.end(), v.begin(), v.end());
    const std::size_t T = cache_len_ + 1;

    std::vector<float> ctx(static_cast<std::size_t>(q_dim), 0.0f);
    std::vector<float> scores(T);
    const float scale = 1.0f / std::sqrt(static_cast<float>(hd));
    for (int h = 0; h < nh; ++h) {
        const int kvh = h / group;
        const float* qh = q.data() + static_cast<std::size_t>(h) * hd;
        for (std::size_t t = 0; t < T; ++t) {
            const float* kt = layer.k_cache.data() + t * static_cast<std::size_t>(kv_dim) +
                              static_cast<std::size_t>(kvh) * hd;
            float s = 0.0f;
            for (int i = 0; i < hd; ++i) s += qh[i] * kt[i];
            scores[t] = s * scale;
        }
        softmax_inplace(scores.data(), T);
        float* ch = ctx.data() + static_cast<std::size_t>(h) * hd;
        for (std::size_t t = 0; t < T; ++t) {
            const float* vt = layer.v_cache.data() + t * static_cast<std::size_t>(kv_dim) +
                              static_cast<std::size_t>(kvh) * hd;
            const float a = scores[t];
            for (int i = 0; i < hd; ++i) ch[i] += a * vt[i];
        }
    }

    std::vector<float> proj(static_cast<std::size_t>(d));
    if (!matvec_raw(layer.wo, ctx.data(), proj.data(), error)) return false;
    for (std::size_t i = 0; i < proj.size(); ++i) x[i] += proj[i];

    // ---- FFN：norm → SwiGLU → down ----
    const auto ff = static_cast<std::size_t>(cfg_.ff_dim);
    std::vector<float> g(ff);
    std::vector<float> u(ff);
    rmsnorm(hn.data(), x.data(), layer.ffn_norm.data(), hn.size(), eps);
    if (!matvec_raw(layer.gate, hn.data(), g.data(), error)) return false;
    if (!matvec_raw(layer.up, hn.data(), u.data(), error)) return false;
    silu_inplace(g.data(), g.size());
    for (std::size_t i = 0; i < ff; ++i) g[i] *= u[i];
    if (!matvec_raw(layer.down, g.data(), proj.data(), error)) return false;
    for (std::size_t i = 0; i < proj.size(); ++i) x[i] += proj[i];
    return true;
}

bool Talker::project_output(const std::vector<float>& x, std::vector<float>& logits,
                            std::string& error) {
    std::vector<float> h(static_cast<std::size_t>(cfg_.d_model));
    rmsnorm(h.data(), x.data(), output_norm_w_.data(), h.size(), cfg_.rms_eps);
    logits.assign(static_cast<std::size_t>(cfg_.vocab), 0.0f);
    return matvec_raw(output_head_, h.data(), logits.data(), error);
}

bool Talker::forward(const std::vector<Item>& items, std::vector<float>& logits,
                     std::string& error) {
    if (!loaded_) {
        error = "talker: 未加载权重";
        return false;
    }
    if (items.empty()) {
        error = "talker: 空输入";
        return false;
    }

    std::vector<float> x;
    for (const Item& item : items) {
        if (!embed(item, x, error)) return false;
        for (auto& layer : layers_) {
            if (!run_layer(layer, x, item, error)) return false;
        }
        ++cache_len_;
    }
    return project_output(x, logits, error);
}

} // namespace iris::tts
