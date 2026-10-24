// ============================================================================
// IndexTTS 2.5 ONNX 编排（实验性）。推理代码由 Iris 自写，运行时引用打包好的
// onnxruntime DLL（tools/runtimes/onnx，gitignored，不内嵌源码）。
//
// 流水线（mirrors index_tts_2_5_onnx/core/pipeline.py）：
//   build_speaker(ref.wav):
//     audio_22k/16k → w2v-bert features → semantic_model.onnx → spk_cond_emb
//     emo_vec.onnx(spk_cond_emb) → emovec
//     campplus fbank → campplus.onnx → style
//     spk_proj: style @ W.T + B → spk_cond
//     conds = concat([(spk_cond+emovec), 0, 0])
//     ref_mel = mel_spectrogram(audio_22k)
//     prompt_condition = length_regulator(spk_cond_emb, ref_mel_len)
//   synthesize(text):
//     frontend tokenize → gpt_prefill.onnx → gpt_step.onnx（自回归采样）
//     → semantic_codec_decode.onnx → length_regulator.onnx
//     → cfm_estimator.onnx（Euler ODE + CFG）→ bigvgan.onnx → 22050 Hz WAV
//
// 现状：9 个 ONNX session 的加载与模型目录校验已完成；w2v-bert 特征、tiktoken
// 前端、GPT 自回归与 CFM solver 仍未接通，因此 synth_indextts 目前一律返回
// 明确的「编排未完成」错误。可用引擎见 qwen3-tts（CrispASR，已端到端可用）。
//
// GPU 可行：构建期定义 IRIS_ONNX_CUDA 时注册 CUDA EP（无 GPU 自动回落 CPU EP）。
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"
#include "iris/tts/tts.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#include <spdlog/fmt/fmt.h>
#endif

namespace iris::tts {

#if IRIS_HAS_ONNX
namespace {

constexpr int kIndexTtsRate = 22050;

// ---------------------------------------------------------------------------
// ONNX session 封装
// ---------------------------------------------------------------------------
struct OrtSession {
    std::unique_ptr<Ort::Session> sess;
    std::vector<std::string> in_names, out_names;
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
    for (std::size_t i = 0; i < s.sess->GetInputCount(); ++i)
        s.in_names.push_back(s.sess->GetInputNameAllocated(i, alloc).get());
    for (std::size_t i = 0; i < s.sess->GetOutputCount(); ++i)
        s.out_names.push_back(s.sess->GetOutputNameAllocated(i, alloc).get());
    return s;
}

/// 模型目录必需的 ONNX 图（IndexTTS 2.5 onnx 权重布局）。
constexpr const char* kRequiredFiles[] = {
    "campplus.onnx", "emo_vec.onnx", "length_regulator.onnx",
    "semantic_codec_decode.onnx", "bigvgan.onnx", "spk_proj.npz",
    "gpt_prefill/gpt_prefill.onnx", "gpt_step/gpt_step.onnx",
    "cfm_estimator/cfm_estimator.onnx", "semantic_model/semantic_model.onnx",
};

/// 校验模型目录，返回第一个缺失文件的相对路径（全部齐备返回空串）。
std::string first_missing(const std::string& model_dir) {
    namespace fs = std::filesystem;
    for (const char* f : kRequiredFiles) {
        if (!fs::exists(fs::path(model_dir) / f)) return f;
    }
    return {};
}

} // namespace
#endif

bool synth_indextts(const TtsRequest& req, TtsResult& out, std::string& error) {
    (void)out;
#if !IRIS_HAS_ONNX
    (void)req;
    error = "indextts: Iris 未以 onnxruntime 构建（tools/runtimes/onnx 缺失）";
    return false;
#else
    namespace fs = std::filesystem;

    if (req.model.empty() || !fs::is_directory(req.model)) {
        error = "indextts: --model 需要 IndexTTS ONNX 模型目录: " + req.model;
        return false;
    }
    if (const std::string missing = first_missing(req.model); !missing.empty()) {
        error = fmt::format("indextts: 模型缺少 {}/{}", req.model, missing);
        return false;
    }

    // 参考音频（音色克隆）。IndexTTS 要求 22050 Hz；此处只做可读性检查，
    // 特征提取接通后再校验采样率。
    if (!req.ref_wav.empty()) {
        std::vector<float> ref;
        int ref_rate = 0;
        std::string wav_err;
        if (!read_wav(req.ref_wav, ref, ref_rate, wav_err)) {
            error = "indextts: 参考音频无效：" + wav_err;
            return false;
        }
        spdlog::info("indextts: 参考音频 {} 采样 @ {} Hz（目标 {} Hz）", ref.size(), ref_rate,
                     kIndexTtsRate);
    }

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-indextts");
    try {
        // 9 个 ONNX 图全部加载后再进入编排；加载失败会带上具体文件。
        auto campplus = make_session(env, req.model + "/campplus.onnx");
        auto emo_vec = make_session(env, req.model + "/emo_vec.onnx");
        auto len_reg = make_session(env, req.model + "/length_regulator.onnx");
        auto codec = make_session(env, req.model + "/semantic_codec_decode.onnx");
        auto bigvgan = make_session(env, req.model + "/bigvgan.onnx");
        auto gpt_pre = make_session(env, req.model + "/gpt_prefill/gpt_prefill.onnx");
        auto gpt_step = make_session(env, req.model + "/gpt_step/gpt_step.onnx");
        auto cfm = make_session(env, req.model + "/cfm_estimator/cfm_estimator.onnx");
        auto semantic = make_session(env, req.model + "/semantic_model/semantic_model.onnx");
        spdlog::info("indextts: 9 个 ONNX session 全部加载完成");
        (void)campplus; (void)emo_vec; (void)len_reg; (void)codec; (void)bigvgan;
        (void)gpt_pre; (void)gpt_step; (void)cfm; (void)semantic;
    } catch (const Ort::Exception& e) {
        error = std::string("indextts: ONNX 加载失败：") + e.what();
        return false;
    }

    error = "indextts: 编排未完成（缺 w2v-bert 特征 / tiktoken 前端 / GPT 自回归 / "
            "CFM solver）；请先用 --engine qwen3-tts 跑通配音链路";
    return false;
#endif
}

} // namespace iris::tts
