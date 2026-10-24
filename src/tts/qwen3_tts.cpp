// ============================================================================
// Qwen3-TTS 后端 —— CrispASR 进程内推理（`qwen3-tts`），运行时只引用打包好的
// crispasr.dll（tools/runtimes/crispasr），不内嵌运行时源码。
//
// 模型由两个 GGUF 组成：
//   --model  talker / code predictor / speaker encoder
//            （qwen3-tts-12hz-0.6b-base-q8_0.gguf）
//   --codec  12Hz tokenizer / codec
//            （qwen3-tts-tokenizer-12hz.gguf）
//
// 音色克隆：--ref <wav> 且必须同时给 --ref-text（参考音频的逐字稿）；
// 不给 --ref 时使用后端自带的默认音色。
// 输出：单声道 float32 PCM，采样率由后端报告（该模型为 24000 Hz）。
// ============================================================================

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"
#include "iris/tts/gguf.hpp"
#include "iris/tts/tts.hpp"

#if IRIS_HAS_CRISPASR
#include "crispasr.h"
#include "crispasr_session.h"
#endif

#if IRIS_HAS_CRISPASR
#include <spdlog/fmt/fmt.h>
#endif

namespace iris::tts {

#if IRIS_HAS_CRISPASR
namespace {

/// qwen3-tts 的 speaker encoder 固定要求 24 kHz 参考音频。
constexpr int kRefSampleRate = 24000;

/// CrispASR 的 PCM 由库分配，必须用 crispasr_pcm_free 归还。
struct PcmDeleter {
    void operator()(float* p) const {
        if (p) crispasr_pcm_free(p);
    }
};
using PcmPtr = std::unique_ptr<float, PcmDeleter>;

struct SessionDeleter {
    void operator()(crispasr_session* s) const {
        if (s) crispasr_session_close(s);
    }
};
using SessionPtr = std::unique_ptr<crispasr_session, SessionDeleter>;

/// 从 GGUF 元数据探测后端名（避免手写后端别名与库内注册名不一致）。
std::string detect_backend(const std::string& model) {
    char buf[96] = {};
    if (crispasr_detect_backend_from_gguf(model.c_str(), buf, static_cast<int>(sizeof buf)) > 0 &&
        buf[0] != '\0') {
        return buf;
    }
    return "qwen3-tts";
}

} // namespace
#endif

bool synth_qwen3_tts(const TtsRequest& req, TtsResult& out, std::string& error) {
#if !IRIS_HAS_CRISPASR
    (void)req;
    (void)out;
    error = "qwen3-tts: CrispASR 运行时 DLL 未就绪（tools/runtimes/crispasr 缺失，"
            "先放入打包好的 crispasr.dll）";
    return false;
#else
    namespace fs = std::filesystem;

    if (req.model.empty()) {
        error = "qwen3-tts: 需要 --model 指定 talker GGUF";
        return false;
    }
    if (!fs::exists(req.model)) {
        error = "qwen3-tts: 模型文件不存在: " + req.model;
        return false;
    }
    if (req.codec_model.empty()) {
        error = "qwen3-tts: 需要 --codec 指定 12Hz tokenizer GGUF"
                "（qwen3-tts-tokenizer-12hz.gguf）";
        return false;
    }
    if (!fs::exists(req.codec_model)) {
        error = "qwen3-tts: tokenizer/codec 文件不存在: " + req.codec_model;
        return false;
    }

    // 自研运行时入口：先用我们自己的 GGUF 解析器核对权重结构（架构、层数、词表、
    // 12 Hz tokenizer 的码本数），结构不对就直接报错，不交给推理后端猜。
    gguf::File model_gguf;
    std::string gguf_error;
    if (!gguf::load(req.model, model_gguf, gguf_error)) {
        error = "qwen3-tts: 模型文件不可解析（" + gguf_error + "）";
        return false;
    }
    const std::string arch = model_gguf.get_string("general.architecture");
    if (arch != "qwen3tts") {
        error = fmt::format("qwen3-tts: 模型架构是 '{}'，不是 qwen3tts", arch);
        return false;
    }
    spdlog::info(
        "qwen3-tts: 权重解析 OK — arch={} tensors={} layers={} hidden={} vocab={} codebooks={}",
        arch, model_gguf.tensor_count, model_gguf.get_int("qwen3tts.talker.n_layers"),
        model_gguf.get_int("qwen3tts.talker.d_model"), model_gguf.get_int("qwen3tts.talker.vocab_size"),
        model_gguf.get_int("qwen3tts.n_codebooks"));

    const std::string backend = detect_backend(req.model);
    SessionPtr session{crispasr_session_open_explicit(
        req.model.c_str(), backend.c_str(), resolve_tts_threads(req.n_threads))};
    if (!session) {
        error = fmt::format("qwen3-tts: 无法加载模型 {}（后端 {}）", req.model, backend);
        return false;
    }

    if (crispasr_session_set_codec_path(session.get(), req.codec_model.c_str()) != 0) {
        error = "qwen3-tts: tokenizer/codec 加载失败: " + req.codec_model;
        return false;
    }
    if (!req.language.empty() && req.language != "auto") {
        crispasr_session_set_source_language(session.get(), req.language.c_str());
    }
    if (req.steps > 0) crispasr_session_set_tts_steps(session.get(), req.steps);
    if (req.cfg_scale > 0.0f) crispasr_session_set_tts_cfg_scale(session.get(), req.cfg_scale);

    if (!req.ref_wav.empty()) {
        // qwen3-tts 的 speaker encoder 只接受 24 kHz 参考音频；其它采样率先归一再加载。
        std::string ref_path;
        std::string ref_error;
        if (!ensure_reference_rate(req.ref_wav, kRefSampleRate, ref_path, ref_error)) {
            error = "qwen3-tts: " + ref_error;
            return false;
        }
        if (ref_path != req.ref_wav) {
            spdlog::info("qwen3-tts: 参考音频重采样为 {} Hz 副本：{}", kRefSampleRate, ref_path);
        }

        const int rc = crispasr_session_set_voice(
            session.get(), ref_path.c_str(), req.ref_text.empty() ? nullptr : req.ref_text.c_str());
        if (rc != 0) {
            error = fmt::format("qwen3-tts: 参考音频加载失败（{}，rc={}）{}", req.ref_wav, rc,
                                req.ref_text.empty() ? "；用 WAV 克隆时必须同时给 --ref-text（参考音频逐字稿）"
                                                     : "");
            return false;
        }
    }

    int n_samples = 0;
    PcmPtr pcm{crispasr_session_synthesize(session.get(), req.text.c_str(), &n_samples)};
    if (!pcm || n_samples <= 0) {
        const char* why = crispasr_session_last_synth_error(session.get());
        error = std::string("qwen3-tts: 合成失败") + (why != nullptr && *why != '\0' ? std::string("：") + why : "");
        return false;
    }

    const int sample_rate = crispasr_session_output_sample_rate(session.get());
    if (sample_rate <= 0) {
        error = "qwen3-tts: 后端未报告输出采样率";
        return false;
    }

    out.pcm.assign(pcm.get(), pcm.get() + n_samples);
    out.sample_rate = sample_rate;
    spdlog::info("qwen3-tts: {} 采样 @ {} Hz（后端 {}）", out.pcm.size(), sample_rate, backend);
    return true;
#endif
}

} // namespace iris::tts
