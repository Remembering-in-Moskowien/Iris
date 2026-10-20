// ============================================================================
// whisper（ONNX）后端 —— 推理代码全部由 Iris 自写，运行时引用打包好的
// onnxruntime DLL（tools/runtimes/onnx，gitignored，不内嵌源码）。
//
// 模型格式：whisperX / Xenova 风格 ONNX 目录
//   encoder_model.onnx         mel(1,80,3000) → encoder_out(1,n_ctx,d_model)
//   decoder_model_merged.onnx  input_ids(1,n)、audio(1,n_ctx,d_model)、
//                              offset(1)、past_key_values.N.key/value
//                              → logits(1,n,vocab)、present_key_values.N.key/value
//
// 解码：30 秒窗口 log-mel（复用 src/asr/ct2_mel.cpp），自回归贪心（温度 0）
// + 时间戳交替规则（与 faster-whisper 语义一致），kv 缓存随轮传递，
// 段落解析与 CT2 后端共用同一套时间戳边界逻辑。
// GPU 可行：构建期定义 IRIS_ONNX_CUDA 时注册 CUDA EP（本机无 GPU 时回落 CPU EP）。
// ============================================================================

#include "iris/asr/onnx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "iris/asr/ct2_mel.hpp"
#include "iris/infra/log.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>

#include <nlohmann/json.hpp>

#include <spdlog/fmt/fmt.h>
#endif

namespace iris::asr {

#if IRIS_HAS_ONNX

namespace {

// ---------------------------------------------------------------------------
// tokenizer：从 tokenizer.json（HF 格式）读取 vocab，做 whisper 风格解码。
// ---------------------------------------------------------------------------
struct OnnxTokenizer {
    std::vector<std::string> id_to_token;  // id → token 字符串（BPE 表示，Ġ=U+0120）
    std::size_t vocab_size = 0;
    std::size_t sot = 0;        // <|startoftranscript|>
    std::size_t eot = 0;        // <|endoftext|>
    std::size_t transcribe = 0; // <|transcribe|>
    std::size_t lang_en = 0;    // <|en|>
    std::size_t ts_begin = 0;   // <|0.00|>
    std::size_t notimestamps = 0;
    std::size_t nospeech = 0;
};

bool load_tokenizer(const std::string& model_dir, OnnxTokenizer& tok, std::string& error) {
    std::ifstream f(model_dir + "/tokenizer.json");
    if (!f) {
        error = "onnx: 缺少 tokenizer.json（模型目录 " + model_dir + "）";
        return false;
    }
    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        error = std::string("onnx: tokenizer.json 解析失败: ") + e.what();
        return false;
    }
    const auto& vocab = j.at("model").at("vocab");
    tok.vocab_size = vocab.size();
    // added_tokens（特殊 token）id 通常超出 vocab，先扩容再合并。
    std::size_t max_id = tok.vocab_size;
    if (j.contains("added_tokens")) {
        for (const auto& at : j.at("added_tokens"))
            max_id = std::max(max_id, at.at("id").get<std::size_t>() + 1);
    }
    tok.id_to_token.resize(max_id);
    for (auto it = vocab.begin(); it != vocab.end(); ++it) {
        const std::size_t id = it.value().get<std::size_t>();
        if (id < tok.id_to_token.size()) tok.id_to_token[id] = it.key();
    }
    if (j.contains("added_tokens")) {
        for (const auto& at : j.at("added_tokens")) {
            const std::string content = at.at("content").get<std::string>();
            const std::size_t id = at.at("id").get<std::size_t>();
            if (id < tok.id_to_token.size()) tok.id_to_token[id] = content;
        }
    }
    const auto find = [&](const char* name) -> std::size_t {
        for (std::size_t i = 0; i < tok.id_to_token.size(); ++i)
            if (tok.id_to_token[i] == name) return i;
        return 0;
    };
    tok.sot = find("<|startoftranscript|>");
    tok.eot = find("<|endoftext|>");
    tok.transcribe = find("<|transcribe|>");
    tok.lang_en = find("<|en|>");
    tok.notimestamps = find("<|notimestamps|>");
    tok.nospeech = find("<|nospeech|>");
    if (tok.sot == 0 || tok.eot == 0) {
        error = "onnx: tokenizer.json 缺少必要特殊 token（<|startoftranscript|>/<|endoftext|>）";
        return false;
    }
    // 时间戳 token 不在 tokenizer 中：transformers whisper 布局下
    // ts_begin = vocab_size - 1500。vocab_size 以模型 config.json 为准，
    // 缺失时回退用 tokenizer 可解析的最大 id + 1 推测。
    std::size_t vocab_size = 0;
    {
        std::ifstream cf(model_dir + "/config.json");
        if (cf) {
            nlohmann::json cj;
            try { cf >> cj; } catch (...) {}
            const auto it = cj.find("vocab_size");
            if (it != cj.end()) vocab_size = it->get<std::size_t>();
        }
    }
    if (vocab_size == 0) {
        std::size_t max_id = 0;
        for (std::size_t i = 0; i < tok.id_to_token.size(); ++i)
            if (!tok.id_to_token[i].empty()) max_id = i;
        vocab_size = max_id + 1;
    }
    tok.ts_begin = vocab_size > 1500 ? vocab_size - 1500 : 0;
    tok.vocab_size = vocab_size;
    if (tok.ts_begin == 0) {
        error = "onnx: 无法确定时间戳 token 起始 id（vocab_size 异常）";
        return false;
    }
    return true;
}

// BPE 字符串解码：Ġ（U+0120，UTF-8 0xC4 0xA0）→ 空格；其余原样保留。
std::string decode_bpe_token(const std::string& tok) {
    std::string out;
    out.reserve(tok.size() + 4);
    for (std::size_t i = 0; i < tok.size();) {
        if (i + 1 < tok.size() && static_cast<unsigned char>(tok[i]) == 0xC4 &&
            static_cast<unsigned char>(tok[i + 1]) == 0xA0) {
            out += ' ';
            i += 2;
        } else {
            out += tok[i];
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// ONNX 会话封装：按名字取输入/输出，自动记录形状。
// ---------------------------------------------------------------------------
struct OnnxSession {
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::vector<std::vector<std::int64_t>> input_shapes;

    [[nodiscard]] std::int64_t index_of(const std::string& key) const {
        for (std::size_t i = 0; i < input_names.size(); ++i)
            if (input_names[i].find(key) != std::string::npos)
                return static_cast<std::int64_t>(i);
        return -1;
    }
    [[nodiscard]] std::int64_t index_of_out(const std::string& key) const {
        for (std::size_t i = 0; i < output_names.size(); ++i)
            if (output_names[i].find(key) != std::string::npos)
                return static_cast<std::int64_t>(i);
        return -1;
    }
};

bool open_session(const Ort::Env& env, const std::string& path, Ort::SessionOptions& opts,
                  OnnxSession& sess, std::string& error) {
    try {
        // onnxruntime 在 Windows 上以 ORTCHAR_T（wchar_t）接受模型路径，UTF-8 → 宽字符。
        std::wstring wpath;
        wpath.reserve(path.size());
        for (const char c : path) wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        sess.session = std::make_unique<Ort::Session>(env, wpath.c_str(), opts);
    } catch (const Ort::Exception& e) {
        error = std::string("onnx: 加载 ") + path + " 失败: " + e.what();
        return false;
    }
    const Ort::AllocatorWithDefaultOptions alloc;
    for (std::size_t i = 0; i < sess.session->GetInputCount(); ++i) {
        auto name = sess.session->GetInputNameAllocated(i, alloc);
        sess.input_names.emplace_back(name.get());
        auto info = sess.session->GetInputTypeInfo(i);
        if (info.GetONNXType() == ONNX_TYPE_TENSOR) {
            const auto& shape = info.GetTensorTypeAndShapeInfo().GetShape();
            std::vector<std::int64_t> dims;
            for (const auto d : shape) dims.push_back(d);
            sess.input_shapes.push_back(std::move(dims));
        } else {
            sess.input_shapes.emplace_back();
        }
    }
    for (std::size_t i = 0; i < sess.session->GetOutputCount(); ++i) {
        auto name = sess.session->GetOutputNameAllocated(i, alloc);
        sess.output_names.emplace_back(name.get());
    }
    return true;
}

// ---------------------------------------------------------------------------
// 解码候选规则（与 whisper.cpp / faster-whisper 语义一致）：
//  - 文本 token 集合 = [0, eot)；eot 恒可用；
//  - 特殊 token（(eot, ts_begin) 区间，如 sot/lang/transcribe/notimestamps 等）禁止；
//  - 时间戳 = [ts_begin, ts_begin+1500)，必须严格递增；
//  - 上一 token 是时间戳 → 本轮禁止时间戳（时间戳成对出现，不能连发），只许文本或 eot；
//  - 首帧（尚未生成任何 token）→ 只许时间戳或 eot（whisper 强制首时间戳）；
//  - 其余（文本后 / eot 后）→ 文本、时间戳、eot 均可（模型自由，句子间自发出时间戳）。
// ---------------------------------------------------------------------------
struct DecodeRules {
    std::size_t eot = 0;
    std::size_t ts_begin = 0;
    std::size_t last_ts = 0;    // 最近时间戳（0 = 尚无）
    bool prev_is_ts = false;
    bool generated_any = false; // 是否已生成过 token（含 eot 判断）

    // 在 logits 行上按规则取最高分 token。
    [[nodiscard]] std::int64_t pick(const float* logits, std::size_t vocab_size) const {
        std::int64_t best = static_cast<std::int64_t>(eot);
        float best_score = logits[eot];
        auto consider = [&](std::int64_t id) {
            const float s = logits[id];
            if (s > best_score) { best_score = s; best = id; }
        };
        if (prev_is_ts) {
            // 文本 + eot（禁时间戳）
            for (std::int64_t id = 0; id < static_cast<std::int64_t>(eot); ++id) consider(id);
        } else if (!generated_any) {
            // 首帧：文本 + 时间戳 + eot 全开放（greedy 下模型可能直接输出文本）
            for (std::int64_t id = 0; id < static_cast<std::int64_t>(eot); ++id) consider(id);
            const std::size_t lo = ts_begin;
            for (std::size_t id = lo; id < ts_begin + 1500; ++id) consider(static_cast<std::int64_t>(id));
        } else {
            // 文本后：文本 + 时间戳（递增）+ eot 全开放
            for (std::int64_t id = 0; id < static_cast<std::int64_t>(eot); ++id) consider(id);
            const std::size_t lo = last_ts ? last_ts + 1 : ts_begin;
            for (std::size_t id = lo; id < ts_begin + 1500; ++id) consider(static_cast<std::int64_t>(id));
        }
        return best;
    }
};

// ---------------------------------------------------------------------------
// 主转写：whisper(ONNX)
// ---------------------------------------------------------------------------
bool transcribe_onnx_impl(const std::string& model_dir, const std::string& audio_path,
                          const std::string& language, std::vector<model::Sentence>& out,
                          std::string& error) {
    // 模型文件：兼容 flat（model_dir/encoder_model.onnx）与
    // Xenova/whisperX 子目录（model_dir/onnx/encoder_model.onnx）两种布局。
    std::string enc_path = model_dir + "/encoder_model.onnx";
    std::string dec_path = model_dir + "/decoder_model_merged.onnx";
    if (!std::ifstream(enc_path).good() || !std::ifstream(dec_path).good()) {
        const std::string alt = model_dir + "/onnx/";
        if (std::ifstream(alt + "encoder_model.onnx").good() &&
            std::ifstream(alt + "decoder_model_merged.onnx").good()) {
            enc_path = alt + "encoder_model.onnx";
            dec_path = alt + "decoder_model_merged.onnx";
        }
    }
    if (!std::ifstream(enc_path).good() || !std::ifstream(dec_path).good()) {
        error = "onnx: 缺少 encoder_model.onnx / decoder_model_merged.onnx（whisperX/Xenova 格式）";
        return false;
    }
    OnnxTokenizer tok;
    if (!load_tokenizer(model_dir, tok, error)) return false;

    std::vector<float> samples;
    if (!read_wav_16k_mono(audio_path, samples, error)) return false;
    constexpr std::size_t kWindowSamples = 30 * 16000;
    const std::size_t n_mels = 80;  // whisper 标准

    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-asr");
    Ort::SessionOptions enc_opts, dec_opts;
    enc_opts.SetIntraOpNumThreads(0);
    dec_opts.SetIntraOpNumThreads(0);
    enc_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    dec_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef IRIS_ONNX_CUDA
    {  // GPU 可行：构建期启用 CUDA EP（无 GPU 自动回落 CPU）。
        OrtCUDAProviderOptions cuda_opts{};
        try { enc_opts.AppendExecutionProvider_CUDA(cuda_opts); } catch (...) {}
        try { dec_opts.AppendExecutionProvider_CUDA(cuda_opts); } catch (...) {}
    }
#endif

    OnnxSession enc, dec;
    if (!open_session(env, enc_path, enc_opts, enc, error)) return false;
    if (!open_session(env, dec_path, dec_opts, dec, error)) return false;

    const auto enc_out_idx = enc.index_of_out("last_hidden_state");
    const auto audio_in = dec.index_of("encoder_hidden_states");
    const auto cache_branch = dec.index_of("use_cache_branch");
    const auto ids_in = dec.index_of("input_ids");
    const auto logits_out = dec.index_of_out("logits");
    spdlog::debug("onnx sig: enc_in=[{}] enc_out=[{}] dec_in=[{}] dec_out=[{}]",
                  fmt::join(enc.input_names, ","), fmt::join(enc.output_names, ","),
                  fmt::join(dec.input_names, ","), fmt::join(dec.output_names, ","));
    if (enc_out_idx < 0 || audio_in < 0 || cache_branch < 0 || ids_in < 0 || logits_out < 0) {
        error = "onnx: 模型输入/输出签名不符合 whisper onnx（Optimum/Xenova merged）格式";
        return false;
    }
    // past 输入数 = 4 × n_layers（decoder key/value + encoder key/value）。
    std::size_t n_past_inputs = 0;
    for (const auto& nm : dec.input_names)
        if (nm.rfind("past_key_values.", 0) == 0) ++n_past_inputs;
    if (n_past_inputs == 0 || n_past_inputs % 4 != 0) {
        error = "onnx: past_key_values 输入数量异常（无法推断层数）";
        return false;
    }
    const std::size_t n_layers = n_past_inputs / 4;
    const std::int64_t head_dim = 64;  // whisper tiny/base：d_model / n_head

    if (!language.empty() && language != "auto" && language != "en") {
        error = "onnx: 当前模型为英文单语（whisper-tiny.en），暂不支持语言 " + language;
        return false;
    }
    const std::size_t lang_id = tok.lang_en ? tok.lang_en : tok.sot + 1;

    out.clear();
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    for (std::size_t off = 0; off < samples.size(); off += kWindowSamples) {
        std::vector<float> win(samples.begin() + static_cast<std::ptrdiff_t>(off),
                               samples.begin() +
                                   static_cast<std::ptrdiff_t>(std::min(off + kWindowSamples,
                                                                        samples.size())));
        std::vector<float> mel;
        if (!log_mel_spectrogram_30s(win, n_mels, mel, error)) return false;

        // ---- encoder ----
        const std::vector<std::int64_t> mel_shape{1, 80, 3000};
        std::vector<Ort::Value> enc_inputs;
        enc_inputs.push_back(Ort::Value::CreateTensor<float>(
            mem_info, mel.data(), mel.size(), mel_shape.data(), mel_shape.size()));
        std::vector<const char*> enc_in_names{enc.input_names[0].c_str()};
        std::vector<const char*> enc_out_names{
            enc.output_names[static_cast<std::size_t>(enc_out_idx)].c_str()};
        auto enc_outputs = enc.session->Run(Ort::RunOptions{nullptr}, enc_in_names.data(),
                                           enc_inputs.data(), enc_inputs.size(),
                                           enc_out_names.data(), enc_out_names.size());
        if (enc_outputs.empty()) { error = "onnx: encoder 推理失败"; return false; }
        auto enc_shape = enc_outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (enc_shape.size() != 3) { error = "onnx: encoder 输出形状异常"; return false; }
        const std::int64_t n_ctx = enc_shape[1];
        const std::int64_t d_model = enc_shape[2];
        const float* enc_data = enc_outputs[0].GetTensorData<float>();
        const std::vector<float> enc_vec(enc_data, enc_data + enc_shape[0] * n_ctx * d_model);

        // ---- 自回归解码（贪心，温度 0 + 交替规则，kv 缓存轮转） ----
        const std::int64_t n_head = d_model / head_dim;  // whisper：head_dim = d_model / n_head
        std::vector<std::int64_t> tokens{static_cast<std::int64_t>(tok.sot),
                                         static_cast<std::int64_t>(lang_id),
                                         static_cast<std::int64_t>(tok.transcribe)};
        DecodeRules rules;
        rules.eot = tok.eot;
        rules.ts_begin = tok.ts_begin;
        // 缓存槽：decoder key/value（2*n_layers）＋ encoder key/value（2*n_layers）。
        const std::size_t n_slots = 4 * n_layers;
        std::vector<std::vector<float>> past(n_slots);
        std::vector<std::int64_t> past_len(n_slots, 0);
        const std::size_t max_len = 448;

        for (std::size_t step = 0; step < max_len; ++step) {
            const std::size_t n_new = tokens.size() - static_cast<std::size_t>(past_len[0]);
            const bool use_cache = past_len[0] > 0;  // 首轮全量建缓存，之后增量
            const std::vector<std::int64_t> ids_shape{1, static_cast<std::int64_t>(n_new)};
            const std::vector<std::int64_t> cache_shape{1};
            const std::vector<std::int64_t> enc_h_shape{1, n_ctx, d_model};

            std::vector<Ort::Value> dec_inputs;
            std::vector<const char*> dec_in_names;
            for (std::size_t i = 0; i < dec.input_names.size(); ++i) {
                const auto& nm = dec.input_names[i];
                dec_in_names.push_back(nm.c_str());
                if (nm.find("input_ids") != std::string::npos) {
                    dec_inputs.push_back(Ort::Value::CreateTensor<std::int64_t>(
                        mem_info, const_cast<std::int64_t*>(tokens.data()) + past_len[0], n_new,
                        ids_shape.data(), ids_shape.size()));
                } else if (nm.find("encoder_hidden_states") != std::string::npos) {
                    dec_inputs.push_back(Ort::Value::CreateTensor<float>(
                        mem_info, const_cast<float*>(enc_vec.data()), enc_vec.size(),
                        enc_h_shape.data(), enc_h_shape.size()));
                } else if (nm.find("use_cache_branch") != std::string::npos) {
                    dec_inputs.push_back(Ort::Value::CreateTensor<bool>(
                        mem_info, const_cast<bool*>(&use_cache), 1, cache_shape.data(),
                        cache_shape.size()));
                } else if (nm.rfind("past_key_values.", 0) == std::string::npos) {
                    error = "onnx: decoder 存在未知输入 " + nm;
                    return false;
                } else {
                    // "past_key_values.N.decoder.key/value" 或 "past_key_values.N.encoder.key/value"
                    const std::string prefix = "past_key_values.";
                    const std::size_t n0 = prefix.size();
                    const std::size_t n1 = nm.find('.', n0);
                    const std::size_t layer = std::stoul(nm.substr(n0, n1 - n0));
                    const bool is_encoder = nm.find(".encoder.", n1) != std::string::npos;
                    const bool is_value = nm.find(".value", n1) != std::string::npos;
                    const std::size_t slot =
                        (is_encoder ? 2 * n_layers : 0) + layer * 2 + (is_value ? 1 : 0);
                    const std::vector<std::int64_t> k_shape{1, n_head, past_len[slot], head_dim};
                    if (past_len[slot] == 0) {
                        dec_inputs.push_back(Ort::Value::CreateTensor<float>(
                            mem_info, nullptr, 0, k_shape.data(), k_shape.size()));
                    } else {
                        dec_inputs.push_back(Ort::Value::CreateTensor<float>(
                            mem_info, past[slot].data(),
                            static_cast<std::size_t>(k_shape[0] * k_shape[1] * k_shape[2] *
                                                    k_shape[3]),
                            k_shape.data(), k_shape.size()));
                    }
                }
            }

            std::vector<const char*> dec_out_names;
            for (const auto& nm : dec.output_names) dec_out_names.push_back(nm.c_str());
            auto dec_outputs = dec.session->Run(Ort::RunOptions{nullptr}, dec_in_names.data(),
                                               dec_inputs.data(), dec_inputs.size(),
                                               dec_out_names.data(), dec_out_names.size());
            if (dec_outputs.empty()) { error = "onnx: decoder 推理失败"; return false; }

            std::size_t logits_idx = 0;
            for (std::size_t i = 0; i < dec.output_names.size(); ++i)
                if (dec.output_names[i].find("logits") != std::string::npos) { logits_idx = i; break; }
            const auto lg_shape = dec_outputs[logits_idx].GetTensorTypeAndShapeInfo().GetShape();
            const std::int64_t n_input_actual = lg_shape[1];
            const std::int64_t vocab_size = lg_shape[2];
            const float* lg = dec_outputs[logits_idx].GetTensorData<float>();
            const float* last_row = lg + (n_input_actual - 1) * vocab_size;

            const std::int64_t next = rules.pick(last_row, static_cast<std::size_t>(vocab_size));
            if (next == static_cast<std::int64_t>(tok.eot)) break;
            tokens.push_back(next);
            const bool next_is_ts = next >= static_cast<std::int64_t>(tok.ts_begin);
            if (next_is_ts) rules.last_ts = static_cast<std::size_t>(next);
            rules.prev_is_ts = next_is_ts;
            rules.generated_any = true;
            if (next >= static_cast<std::int64_t>(tok.ts_begin + 1500 - 1)) break;

            // 更新 kv 缓存：present.N.decoder/encoder.key/value 拷入 past。
            // 注意：增量轮（use_cache=true）的 present.encoder 只是占位（长度 1），
            // cross-attn 缓存须保持首轮全量 forward 的 1500 长度，不在此覆盖。
            for (std::size_t i = 0; i < dec.output_names.size(); ++i) {
                const auto& onm = dec.output_names[i];
                if (onm.rfind("present.", 0) != 0) continue;
                const std::size_t n0 = 8;  // "present."
                const std::size_t n1 = onm.find('.', n0);
                const std::size_t layer = std::stoul(onm.substr(n0, n1 - n0));
                const bool is_encoder = onm.find(".encoder.", n1) != std::string::npos;
                const bool is_value = onm.find(".value", n1) != std::string::npos;
                if (use_cache && is_encoder) continue;  // 增量轮跳过 encoder 缓存
                const std::size_t slot =
                    (is_encoder ? 2 * n_layers : 0) + layer * 2 + (is_value ? 1 : 0);
                const float* pd = dec_outputs[i].GetTensorData<float>();
                const auto ps = dec_outputs[i].GetTensorTypeAndShapeInfo().GetShape();
                std::size_t total = 1;
                for (const auto d : ps) total *= static_cast<std::size_t>(d);
                past[slot].assign(pd, pd + total);
                past_len[slot] = ps.size() >= 2 ? ps[ps.size() - 2] : 0;
            }
        }

        // ---- 段落解析（时间戳边界 + BPE 解码，与 CT2 后端同构） ----
        struct Seg { std::int64_t start_ms, end_ms; std::string text; };
        std::vector<Seg> segs;
        bool has_ts = false;
        for (const auto id : tokens)
            if (id >= static_cast<std::int64_t>(tok.ts_begin)) { has_ts = true; break; }
        if (!has_ts) {
            // 无时间戳（greedy 兜底）：整窗文本作为一段。
            Seg whole{0, 30'000, ""};
            for (const auto id : tokens) {
                if (id >= 0 && static_cast<std::size_t>(id) < tok.id_to_token.size() &&
                    !(id > static_cast<std::int64_t>(tok.eot) &&
                      id < static_cast<std::int64_t>(tok.ts_begin)))
                    whole.text += decode_bpe_token(tok.id_to_token[static_cast<std::size_t>(id)]);
            }
            if (!whole.text.empty()) segs.push_back(std::move(whole));
        } else {
            Seg cur{-1, -1, ""};
            for (const auto id : tokens) {
                if (id == static_cast<std::int64_t>(tok.sot) ||
                    id == static_cast<std::int64_t>(tok.transcribe) ||
                    id == static_cast<std::int64_t>(lang_id) ||
                    (id > static_cast<std::int64_t>(tok.eot) &&
                     id < static_cast<std::int64_t>(tok.ts_begin)))
                    continue;
                if (id >= static_cast<std::int64_t>(tok.ts_begin) &&
                    id < static_cast<std::int64_t>(tok.ts_begin + 1500)) {
                    const std::int64_t ms = (id - static_cast<std::int64_t>(tok.ts_begin)) * 20;
                    if (cur.start_ms < 0) {
                        cur.start_ms = ms;
                    } else {
                        if (!cur.text.empty()) {
                            cur.end_ms = ms;
                            segs.push_back(cur);
                        }
                        cur = Seg{ms, -1, ""};
                    }
                } else if (id >= 0 && static_cast<std::size_t>(id) < tok.id_to_token.size()) {
                    cur.text += decode_bpe_token(tok.id_to_token[static_cast<std::size_t>(id)]);
                }
            }
        }
        const std::int64_t offset_ms = static_cast<std::int64_t>(off / kWindowSamples) * 30'000;
        for (auto& seg : segs) {
            if (seg.text.empty() || seg.end_ms < 0) continue;
            model::Sentence s;
            s.start_ms = seg.start_ms + offset_ms;
            s.end_ms = seg.end_ms + offset_ms;
            s.text = std::move(seg.text);
            out.push_back(std::move(s));
        }
    }

    spdlog::info("onnx (whisper ONNX): {} 段", out.size());
    return true;
}

} // namespace

#endif // IRIS_HAS_ONNX

bool transcribe_onnx(const std::string& model_dir, const std::string& audio_path,
                     const std::string& language, std::vector<model::Sentence>& out,
                     std::string& error) {
#if IRIS_HAS_ONNX
    try {
        return transcribe_onnx_impl(model_dir, audio_path, language, out, error);
    } catch (const Ort::Exception& e) {
        error = std::string("onnx: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        error = std::string("onnx: ") + e.what();
        return false;
    }
#else
    (void)model_dir; (void)audio_path; (void)language; (void)out;
    error = "onnx: 运行时 DLL 未就绪（tools/runtimes/onnx 缺失，先放入打包好的 onnxruntime.dll）";
    return false;
#endif
}

} // namespace iris::asr
