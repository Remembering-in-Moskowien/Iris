#include "iris/asr/asr.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "iris/asr/ct2_mel.hpp"
#include "iris/asr/onnx.hpp"
#include "iris/infra/log.hpp"

#if IRIS_HAS_CRISPASR
#include "crispasr.h"
#include "crispasr_session.h"
#endif

#if IRIS_HAS_CT2
#include <ctranslate2/models/model.h>
#include <ctranslate2/models/whisper.h>
#include <ctranslate2/storage_view.h>
#endif

namespace iris::asr {

namespace {

const char* backend_key(AsrBackend b) {
    switch (b) {
        case AsrBackend::WhisperGgml: return "whisper";
        case AsrBackend::Qwen3AsrGgml: return "qwen3-asr";
        case AsrBackend::WhisperCt2: return "whisper-ct2";
        case AsrBackend::WhisperOnnx: return "whisper-onnx";
    }
    return "unknown";
}

} // namespace

// ---------------------------------------------------------------------------
// CrispASR 后端（whisper / qwen3-asr，ggml）——推理编排自写，运行时引用
// tools/runtimes/crispasr 打包 DLL（crispasr.dll），不内嵌运行时源码。
// ---------------------------------------------------------------------------
#if IRIS_HAS_CRISPASR
bool transcribe_crispasr(AsrBackend backend, const std::string& model_path,
                         const std::string& audio_path, const std::string& language,
                         std::vector<model::Sentence>& out, std::string& error) {
    // 进程内读取 16 kHz 单声道 PCM（与 CT2 后端共用 read_wav_16k_mono），
    // 不再依赖 CrispASR 的音频解码（crisp_audio / ogg / opus 等）。
    std::vector<float> samples;
    if (!read_wav_16k_mono(audio_path, samples, error)) {
        error = "crispasr: " + error;
        return false;
    }

    const int n_threads =
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const char* crisp_backend = (backend == AsrBackend::Qwen3AsrGgml) ? "qwen3" : "whisper";
    crispasr_session* session =
        crispasr_session_open_explicit(model_path.c_str(), crisp_backend, n_threads);
    if (!session) {
        error = "crispasr: 无法加载模型 " + model_path + "（后端 " + crisp_backend + "）";
        return false;
    }

    crispasr_session_result* result =
        (language.empty() || language == "auto")
            ? crispasr_session_transcribe(session, samples.data(),
                                          static_cast<int>(samples.size()))
            : crispasr_session_transcribe_lang(session, samples.data(),
                                               static_cast<int>(samples.size()),
                                               language.c_str());
    if (!result) {
        error = std::string("crispasr: 转写失败（后端 ") +
                (crispasr_session_backend(session) ? crispasr_session_backend(session) : crisp_backend) + ")";
        crispasr_session_close(session);
        return false;
    }

    const int n_segments = crispasr_session_result_n_segments(result);
    out.clear();
    out.reserve(static_cast<std::size_t>(std::max(n_segments, 0)));
    for (int i = 0; i < n_segments; ++i) {
        model::Sentence s;
        // 会话结果时间为厘秒（centiseconds），转毫秒。
        s.start_ms = crispasr_session_result_segment_t0(result, i) * 10;
        s.end_ms = crispasr_session_result_segment_t1(result, i) * 10;
        const char* text = crispasr_session_result_segment_text(result, i);
        s.text = text ? text : "";
        if (s.text.empty()) continue;
        out.push_back(std::move(s));
    }

    crispasr_session_result_free(result);
    crispasr_session_close(session);

    spdlog::info("crispasr: {} 段（后端 {}）", out.size(), crisp_backend);
    return true;
}
#else
bool transcribe_crispasr(AsrBackend /*backend*/, const std::string& /*model_path*/,
                         const std::string& /*audio_path*/, const std::string& /*language*/,
                         std::vector<model::Sentence>& /*out*/, std::string& error) {
    error = "crispasr: 运行时 DLL 未就绪（tools/runtimes/crispasr 缺失，先放入打包好的 crispasr.dll）";
    return false;
}
#endif

// ---------------------------------------------------------------------------
// CTranslate2 后端（whisper，transformer 模型）——mel 提取/30s 窗口/token 解析
// 由 Iris 自写（src/asr/ct2_mel.cpp + 本文件），运行时引用 tools/runtimes/ct2
// 打包 DLL（ctranslate2.dll），不内嵌运行时源码。
// ---------------------------------------------------------------------------
#if IRIS_HAS_CT2
namespace {

// 从 CTranslate2 Whisper 生成的 token 序列解析带时间戳的段落。
// 布局（与 whisper/faster-whisper 一致）：时间戳 token 位于词表末尾 1500 个，
// 段边界由相邻时间戳 token 界定；文本 token 为普通 BPE 词元。
struct Ct2Segment {
    std::int64_t start_ms = -1;
    std::int64_t end_ms = -1;
    std::string text;
};

std::vector<Ct2Segment> parse_ct2_tokens(const std::vector<std::size_t>& ids,
                                         const ctranslate2::Vocabulary& vocab,
                                         std::size_t ts_begin,
                                         std::size_t eot_id) {
    std::vector<Ct2Segment> out;
    Ct2Segment cur;
    bool have_start = false;
    std::int64_t last_ts = -1;

    for (const std::size_t id : ids) {
        if (id == eot_id) break;
        if (id >= ts_begin) {
            // 时间戳 token：id 相对词表末尾每 1 个 = 20 ms。
            const auto t_ms = static_cast<std::int64_t>(id - ts_begin) * 20;
            last_ts = t_ms;
            if (!have_start) {
                cur.start_ms = t_ms;
                have_start = true;
            } else {
                cur.end_ms = t_ms;
                if (!cur.text.empty()) out.push_back(std::move(cur));
                cur = Ct2Segment{};
                have_start = false;
            }
        } else if (id < eot_id) {
            // 普通 BPE 文本 token（whisper 词表：文本 token 位于 <|endoftext|> 之前）。
            std::string tok = vocab.to_token(id);
            if (tok.empty()) continue;
            // GPT-2 风格 BPE：U+0120 'Ġ' 表示词前空格。
            if (tok[0] == '\xC4' && tok.size() > 1 && static_cast<unsigned char>(tok[1]) == 0xA0) {
                cur.text += ' ';
                cur.text.append(tok.begin() + 2, tok.end());
            } else {
                cur.text += tok;
            }
        }
        // 其余为特殊 token（语言/任务/<|nospeech|> 等），跳过。
    }

    if (have_start && !cur.text.empty()) {
        cur.end_ms = (last_ts >= 0) ? last_ts : cur.start_ms;
        out.push_back(std::move(cur));
    }
    return out;
}

} // namespace

bool transcribe_ct2(const std::string& model_path, const std::string& audio_path,
                    const std::string& language, std::vector<model::Sentence>& out,
                    std::string& error) {
    try {
        // 进程内读取 16 kHz 单声道 WAV。
        std::vector<float> samples;
        if (!read_wav_16k_mono(audio_path, samples, error)) return false;

        // 词表（用于 token 解码与时间戳边界）。
        auto model = ctranslate2::models::Model::load(
            model_path, ctranslate2::Device::CPU, 0, ctranslate2::ComputeType::FLOAT32);
        const auto* wmodel = dynamic_cast<const ctranslate2::models::WhisperModel*>(model.get());
        if (!wmodel) {
            error = "ct2: 模型不是 Whisper 架构（需先用 ct2-transformers-converter 转换）: " +
                    model_path;
            return false;
        }
        const auto& vocab = wmodel->get_vocabulary();
        const std::size_t ts_begin = vocab.to_id("<|0.00|>"); // 时间戳 token 起始 id（词表 50364）
        const std::size_t eot_id = vocab.to_id("<|endoftext|>");

        ctranslate2::models::Whisper pool(model_path, ctranslate2::Device::CPU,
                                          ctranslate2::ComputeType::DEFAULT, std::vector<int>{0});
        const std::size_t n_mels = pool.n_mels();

        // 语言 token（与 faster-whisper 的 sot_sequence 一致）。
        std::string lang = language;
        if (lang.empty() || lang == "auto") {
            // 自动检测：先取前 30 秒特征。
            std::vector<float> mel;
            if (!log_mel_spectrogram_30s(samples, n_mels, mel, error)) return false;
            ctranslate2::StorageView feat(
                ctranslate2::Shape{1, static_cast<ctranslate2::dim_t>(n_mels), 3000}, mel,
                ctranslate2::Device::CPU);
            auto det = pool.detect_language(feat);
            auto langs = det[0].get();
            if (langs.empty()) {
                error = "ct2: 语言检测失败";
                return false;
            }
            // 结果形如 "<|en|>"，去掉包裹符号。
            std::string tok = langs[0].first;
            if (tok.size() >= 5 && tok.rfind("<|", 0) == 0 && tok.compare(tok.size() - 2, 2, "|>") == 0) {
                tok = tok.substr(2, tok.size() - 4);
            }
            lang = tok;
        }
        if (lang.size() < 2 || lang.size() > 8) {
            error = "ct2: 不支持的语言代码 '" + lang + "'";
            return false;
        }
        const std::vector<std::string> prompt = {"<|startoftranscript|>", "<|" + lang + "|>",
                                                 "<|transcribe|>"};

        // 30 秒窗口处理（与 faster-whisper 一致）。
        constexpr std::size_t kWindowSamples = 30 * 16000;
        out.clear();
        for (std::size_t off = 0; off < samples.size(); off += kWindowSamples) {
            std::vector<float> win(samples.begin() + static_cast<std::ptrdiff_t>(off),
                                   samples.begin() +
                                       static_cast<std::ptrdiff_t>(std::min(off + kWindowSamples,
                                                                            samples.size())));
            std::vector<float> mel;
            if (!log_mel_spectrogram_30s(win, n_mels, mel, error)) return false;

            ctranslate2::StorageView features(
                ctranslate2::Shape{1, static_cast<ctranslate2::dim_t>(n_mels), 3000}, mel,
                ctranslate2::Device::CPU);
            ctranslate2::models::WhisperOptions opts;
            opts.beam_size = 5; // faster-whisper 默认贪心 5 路束搜索
            opts.sampling_temperature = 0.0f;
            opts.length_penalty = 0.0f;
            opts.no_repeat_ngram_size = 0;
            opts.max_initial_timestamp_index = 50;
            auto results = pool.generate(features, {prompt}, opts);
            auto res = results[0].get();
            if (res.sequences_ids.empty() || res.sequences_ids[0].empty()) continue;

            auto segs = parse_ct2_tokens(res.sequences_ids[0], vocab, ts_begin, eot_id);
            const std::int64_t offset_ms = static_cast<std::int64_t>(off / kWindowSamples) * 30'000;
            for (auto& seg : segs) {
                model::Sentence s;
                s.start_ms = seg.start_ms + offset_ms;
                s.end_ms = seg.end_ms + offset_ms;
                s.text = std::move(seg.text);
                if (s.text.empty()) continue;
                out.push_back(std::move(s));
            }
        }

        spdlog::info("ct2 (CTranslate2): {} 段", out.size());
        return true;
    } catch (const std::exception& e) {
        error = std::string("ct2: ") + e.what();
        return false;
    }
}
#else
bool transcribe_ct2(const std::string& /*model_path*/, const std::string& /*audio_path*/,
                    const std::string& /*language*/, std::vector<model::Sentence>& /*out*/,
                    std::string& error) {
    error = "ct2: 运行时 DLL 未就绪（tools/runtimes/ct2 缺失，先放入打包好的 ctranslate2.dll）";
    return false;
}
#endif

// ---------------------------------------------------------------------------

bool parse_asr_backend(const std::string& engine, AsrBackend& out) {
    if (engine == "whisper") {
        out = AsrBackend::WhisperGgml;
        return true;
    }
    if (engine == "qwen3-asr") {
        out = AsrBackend::Qwen3AsrGgml;
        return true;
    }
    if (engine == "whisper-ct2") {
        out = AsrBackend::WhisperCt2;
        return true;
    }
    if (engine == "whisper-onnx") {
        out = AsrBackend::WhisperOnnx;
        return true;
    }
    return false;
}

bool asr_backend_available(AsrBackend backend) {
    switch (backend) {
        case AsrBackend::WhisperGgml:
        case AsrBackend::Qwen3AsrGgml:
            return IRIS_HAS_CRISPASR != 0;
        case AsrBackend::WhisperCt2:
            return IRIS_HAS_CT2 != 0;
        case AsrBackend::WhisperOnnx:
            return IRIS_HAS_ONNX != 0;
    }
    return false;
}

std::string AsrEngine::name() const {
    return std::string("asr-engine (") + backend_key(backend_) + ")";
}

bool AsrEngine::transcribe(const std::string& audio_path, const std::string& language,
                          std::vector<model::Sentence>& out, std::string& error) {
    switch (backend_) {
        case AsrBackend::WhisperGgml:
        case AsrBackend::Qwen3AsrGgml:
            return transcribe_crispasr(backend_, model_path_, audio_path, language, out, error);
        case AsrBackend::WhisperCt2:
            return transcribe_ct2(model_path_, audio_path, language, out, error);
        case AsrBackend::WhisperOnnx:
            return transcribe_onnx(model_path_, audio_path, language, out, error);
    }
    error = "未知 ASR 后端";
    return false;
}

} // namespace iris::asr
