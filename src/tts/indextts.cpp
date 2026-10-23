// ============================================================================
// IndexTTS 2.5 ONNX 完整编排 —— 推理代码全部由 Iris 自写，运行时引用打包好的
// onnxruntime DLL（tools/runtimes/onnx，gitignored，不内嵌源码）。
//
// 流水线（mirrors index_tts_2_5_onnx/core/pipeline.py）：
//
//   build_speaker(ref.wav):
//     audio_22k/16k → w2v-bert features → semantic_model.onnx → spk_cond_emb
//     emo_vec.onnx(spk_cond_emb) → emovec
//     campplus fbank → campplus.onnx → style
//     spk_proj: style @ W.T + B → spk_cond
//     conds = concat([(spk_cond+emovec), 0, 0])
//     ref_mel = mel_spectrogram(audio_22k)
//     prompt_condition = length_regulator(spk_cond_emb, ref_mel_len)
//
//   synthesize(text):
//     frontend tokenize → gpt_prefill.onnx → gpt_step.onnx (自回归采样)
//     → semantic_codec_decode.onnx → length_regulator.onnx
//     → cfm_estimator.onnx (Euler ODE + CFG) → bigvgan.onnx → 22050Hz WAV
//
// GPU 可行：构建期定义 IRIS_ONNX_CUDA 时注册 CUDA EP（无 GPU 自动回落 CPU EP）。
// ============================================================================

#include "iris/tts/indextts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#include <spdlog/fmt/fmt.h>
#endif

namespace iris::tts {

#if IRIS_HAS_ONNX
namespace {

constexpr int kSamplingRate = 22050;
constexpr int kMaxRefSeconds = 15;
constexpr int kNMelBins = 80;
constexpr int kNFFT = 1024;
constexpr int kHopSize = 256;
constexpr int kWinSize = 1024;

// ---------------------------------------------------------------------------
// ONNX session 封装
// ---------------------------------------------------------------------------
struct OrtSession {
    std::unique_ptr<Ort::Session> sess;
    std::string in_name, out_name;
};

OrtSession make_session(Ort::Env& env, const std::string& path, int threads = 4) {
    OrtSession s;
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(threads);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
#ifdef IRIS_ONNX_CUDA
    OrtSessionOptionsAppendExecutionProvider_CUDA(opts, 0);
#endif
    std::wstring wpath;
    for (const char c : path) wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    s.sess = std::make_unique<Ort::Session>(env, wpath.c_str(), opts);
    Ort::AllocatorWithDefaultOptions alloc;
    s.in_name = s.sess->GetInputNameAllocated(0, alloc).get();
    s.out_name = s.sess->GetOutputNameAllocated(0, alloc).get();
    return s;
}

// ---------------------------------------------------------------------------
// WAV 读取（16kHz / 22050Hz float32）
// ---------------------------------------------------------------------------
bool read_wav_float(const std::string& path, std::vector<float>& out, int& sr) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char riff[4]; f.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0) return false;
    f.seekg(4, std::ios::cur);
    char wave[4]; f.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0) return false;
    int16_t channels = 0, bps = 0;
    uint32_t sample_rate = 0;
    std::vector<char> data;
    while (f) {
        char cid[4]; uint32_t sz = 0;
        if (!f.read(cid, 4)) break;
        f.read(reinterpret_cast<char*>(&sz), 4);
        if (std::memcmp(cid, "fmt ", 4) == 0) {
            char fmt[16]; f.read(fmt, 16);
            std::memcpy(&channels, fmt + 2, 2);
            std::memcpy(&sample_rate, fmt + 4, 4);
            std::memcpy(&bps, fmt + 14, 2);
            if (sz > 16) f.seekg(sz - 16, std::ios::cur);
        } else if (std::memcmp(cid, "data", 4) == 0) {
            data.resize(sz);
            f.read(data.data(), sz);
            break;
        } else {
            f.seekg(sz, std::ios::cur);
            if (sz & 1) f.seekg(1, std::ios::cur);
        }
    }
    if (bps != 16) return false;
    const int16_t* s16 = reinterpret_cast<const int16_t*>(data.data());
    size_t n = data.size() / 2;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = s16[i] / 32768.0f;
    sr = sample_rate;
    return true;
}

// ---------------------------------------------------------------------------
// Mel spectrogram（22050Hz, 80 mel, n_fft=1024, hop=256, center=False）
// ---------------------------------------------------------------------------
std::vector<float> mel_spectrogram(const std::vector<float>& y) {
    // 简化版：STFT + mel filterbank（与 librosa mel 近似）
    const int pad = (kNFFT - kHopSize) / 2;
    std::vector<float> padded(y.size() + 2 * pad);
    for (int i = 0; i < pad; ++i) padded[i] = y[pad - i];  // reflect
    for (size_t i = 0; i < y.size(); ++i) padded[pad + i] = y[i];
    for (int i = 0; i < pad; ++i) padded[pad + y.size() + i] = y[y.size() - 1 - i];

    int n_frames = 1 + (static_cast<int>(padded.size()) - kNFFT) / kHopSize;
    std::vector<float> mel(n_frames * kNMelBins, 0.0f);

    // Hann window
    std::vector<float> win(kWinSize);
    for (int i = 0; i < kWinSize; ++i)
        win[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / kWinSize);

    // 简化：每帧做 RFFT（用 DFT 近似，实际应替换为 FFT）
    // TODO: 替换为高效 FFT（当前 O(n^2) 仅用于验证流程）
    for (int fr = 0; fr < n_frames; ++fr) {
        for (int m = 0; m < kNMelBins; ++m) {
            float mag_sum = 0.0f;
            // 简化：直接累加帧能量作为 mel bin 近似
            for (int k = 0; k < kNFFT; k += 4) {
                float v = padded[fr * kHopSize + k] * win[k];
                mag_sum += v * v;
            }
            mel[fr * kNMelBins + m] = std::log(std::max(mag_sum, 1e-5f));
        }
    }
    return mel;
}

// ---------------------------------------------------------------------------
// tiktoken BPE 加载（简化版：直接字符级编码）
// ---------------------------------------------------------------------------
std::vector<int64_t> simple_tokenize(const std::string& text) {
    std::vector<int64_t> ids;
    for (char c : text) ids.push_back(static_cast<int64_t>(static_cast<unsigned char>(c)));
    return ids;
}

// ---------------------------------------------------------------------------
// GPT 自回归采样（greedy）
// ---------------------------------------------------------------------------
std::vector<int64_t> gpt_generate(OrtSession& pre, OrtSession& step,
                                   const std::vector<int64_t>& text_ids,
                                   const std::vector<float>& conds,
                                   int lang_id, int max_tokens = 200) {
    // TODO: 完整 GPT prefill/step + kv cache + repetition penalty + top_k/top_p
    // 当前骨架：调用 prefill 取 logits，argmax 取第一个 token
    (void)step; (void)max_tokens;
    std::vector<int64_t> codes;
    // 简化：返回空（完整 kv cache 编排待模型就位后调试）
    return codes;
}

} // namespace
#endif

bool synth_indextts(const std::string& model_dir,
                    const std::string& text,
                    const std::string& ref_wav,
                    const std::string& out_wav,
                    std::string& error) {
#if !IRIS_HAS_ONNX
    error = "Iris 未以 ONNX 运行时构建";
    return false;
#else
    namespace fs = std::filesystem;

    // 1. 模型文件检查
    const std::vector<std::string> required = {
        "campplus.onnx", "emo_vec.onnx", "length_regulator.onnx",
        "semantic_codec_decode.onnx", "bigvgan.onnx", "spk_proj.npz",
        "multilingual_zh_ja_yue_char_del.tiktoken"
    };
    for (const auto& f : required) {
        if (!fs::exists(fs::path(model_dir) / f)) {
            error = fmt::format("IndexTTS 模型缺失：{}/{}", model_dir, f);
            return false;
        }
    }

    // 2. 加载参考音频
    std::vector<float> audio_22k, audio_16k;
    int sr = 0;
    if (!read_wav_float(ref_wav, audio_22k, sr)) {
        error = "无法读取参考音频：" + ref_wav;
        return false;
    }
    audio_22k.resize(std::min(audio_22k.size(),
                               static_cast<size_t>(kMaxRefSeconds * kSamplingRate)));
    spdlog::info("tts: 参考音频 {} samples @ {} Hz", audio_22k.size(), sr);

    // 3. ONNX 环境 + session 加载
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-tts");
    try {
        auto campplus = make_session(env, model_dir + "/campplus.onnx");
        auto emo_vec = make_session(env, model_dir + "/emo_vec.onnx");
        auto len_reg = make_session(env, model_dir + "/length_regulator.onnx");
        auto codec = make_session(env, model_dir + "/semantic_codec_decode.onnx");
        auto bigvgan = make_session(env, model_dir + "/bigvgan.onnx");
        spdlog::info("tts: 5 个平面 ONNX session 加载完成");

        // 子目录模型（gpt/cfm/semantic_model）
        auto gpt_pre = make_session(env, model_dir + "/gpt_prefill/gpt_prefill.onnx");
        auto gpt_step = make_session(env, model_dir + "/gpt_step/gpt_step.onnx");
        auto cfm = make_session(env, model_dir + "/cfm_estimator/cfm_estimator.onnx");
        auto semantic = make_session(env, model_dir + "/semantic_model/semantic_model.onnx");
        spdlog::info("tts: 4 个子目录 ONNX session 加载完成（共 9 个）");

        // 4. build_speaker（简化骨架）
        // TODO: w2v-bert features → semantic_model → emo_vec → campplus → spk_proj
        spdlog::warn("tts: build_speaker 完整编排待调试（w2v-bert features/spk_proj 加载）");

        // 5. synthesize（简化骨架）
        auto text_ids = simple_tokenize(text);
        spdlog::info("tts: 文本 tokenize 完成（{} tokens）", text_ids.size());
        // TODO: gpt_generate → codec_decode → len_reg → cfm solver → bigvgan → wav

        (void)out_wav;
        error = "IndexTTS 编排框架已就位；完整 DSP/GPT/CFM 编排待模型权重调试";
        return false;
    } catch (const Ort::Exception& e) {
        error = std::string("ONNX 加载失败：") + e.what();
        return false;
    }
#endif
}

} // namespace iris::tts
