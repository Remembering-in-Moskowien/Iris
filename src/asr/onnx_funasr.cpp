#include "iris/asr/onnx_funasr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "iris/asr/ct2_mel.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/threads.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace iris::asr {

namespace {

constexpr int kSampleRate = 16000;
constexpr int kFrameLengthMs = 25;
constexpr int kFrameShiftMs = 10;
constexpr int kNumMelBins = 80;
constexpr int kFrameLength = kSampleRate * kFrameLengthMs / 1000; // 400
constexpr int kFrameShift = kSampleRate * kFrameShiftMs / 1000;   // 160
constexpr int kFFTSize = 512;
constexpr float kPreemph = 0.97f;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// 512 点实数 FFT（radix-2，迭代 + 位反转）
// ---------------------------------------------------------------------------
void fft_power512(const float* frame, std::vector<float>& power) {
    std::vector<float> re(kFFTSize), im(kFFTSize, 0.0f);
    for (int i = 0; i < kFFTSize; ++i) re[i] = i < kFrameLength ? frame[i] : 0.0f;

    // bit reversal
    for (int i = 1, j = 0; i < kFFTSize; ++i) {
        int bit = kFFTSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(re[i], re[j]);
    }

    for (int len = 2; len <= kFFTSize; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const double wr = std::cos(ang);
        const double wi = std::sin(ang);
        for (int i = 0; i < kFFTSize; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k;
                const int b = i + k + len / 2;
                const double xr = re[b] * cr - im[b] * ci;
                const double xi = re[b] * ci + im[b] * cr;
                re[b] = static_cast<float>(re[a] - xr);
                im[b] = static_cast<float>(im[a] - xi);
                re[a] = static_cast<float>(re[a] + xr);
                im[a] = static_cast<float>(im[a] + xi);
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }

    power.resize(kFFTSize / 2 + 1);
    for (int i = 0; i <= kFFTSize / 2; ++i) {
        power[i] = re[i] * re[i] + im[i] * im[i];
    }
}

/// kaldi mel 滤波器组：mel(f) = 1127 ln(1 + f/700)，三角窗按 mel 域均分。
struct MelBanks {
    std::vector<std::vector<std::pair<int, float>>> bins;
};

double mel_of(double hz) { return 1127.0 * std::log(1.0 + hz / 700.0); }
double hz_of(double mel) { return 700.0 * (std::exp(mel / 1127.0) - 1.0); }

MelBanks make_mel_banks(int n_mels, int fft_size, int sample_rate, double low_hz,
                        double high_hz) {
    MelBanks banks;
    banks.bins.resize(static_cast<std::size_t>(n_mels));

    const int num_bins = fft_size / 2; // 0..256 对应 0..Nyquist
    const double mel_low = mel_of(low_hz);
    const double mel_high = mel_of(high_hz);
    std::vector<double> left(n_mels + 2), center(n_mels + 2), right(n_mels + 2);
    for (int m = 0; m < n_mels + 2; ++m) {
        const double mel = mel_low + (mel_high - mel_low) * m / (n_mels + 1);
        left[m] = mel_of(hz_of(mel - (mel_high - mel_low) / (n_mels + 1)));
        center[m] = mel;
        right[m] = mel_of(hz_of(mel + (mel_high - mel_low) / (n_mels + 1)));
    }

    for (int m = 0; m < n_mels; ++m) {
        const double l = center[m];
        const double c = center[m + 1];
        const double r = center[m + 2];
        for (int k = 0; k <= num_bins; ++k) {
            const double mel = mel_of(static_cast<double>(k) * sample_rate * 0.5 / num_bins);
            if (mel <= l || mel >= r) continue;
            const double w = mel <= c ? (mel - l) / (c - l) : (r - mel) / (r - c);
            if (w > 0.0) {
                banks.bins[static_cast<std::size_t>(m)].emplace_back(k, static_cast<float>(w));
            }
        }
    }
    return banks;
}

std::vector<float> povey_window(int n) {
    std::vector<float> w(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double base = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
        w[static_cast<std::size_t>(i)] = static_cast<float>(std::pow(base, 0.85));
    }
    return w;
}

/// 读 tokens.txt（每行 "<token> <id>"）。
bool load_tokens(const std::string& path, std::vector<std::string>& id_to_token,
                 std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "无法读取 tokens.txt：" + path;
        return false;
    }
    std::vector<std::pair<int, std::string>> items;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const auto space = line.rfind(' ');
        if (space == std::string::npos) continue;
        const std::string token = line.substr(0, space);
        const int id = std::atoi(line.c_str() + space + 1);
        if (id < 0) continue;
        items.emplace_back(id, token);
    }
    if (items.empty()) {
        error = "tokens.txt 为空：" + path;
        return false;
    }
    int max_id = 0;
    for (const auto& [id, _] : items) max_id = std::max(max_id, id);
    id_to_token.assign(static_cast<std::size_t>(max_id + 1), std::string());
    for (auto& [id, token] : items) id_to_token[static_cast<std::size_t>(id)] = std::move(token);
    return true;
}

/// 在模型目录里找 onnx 文件（int8 优先）。
std::string find_onnx(const std::string& dir) {
    namespace fs = std::filesystem;
    for (const char* name : {"model.int8.onnx", "model_quant.onnx", "model.onnx"}) {
        const fs::path p = fs::path(dir) / name;
        std::error_code ec;
        if (fs::exists(p, ec)) return p.string();
    }
    return {};
}

int language_id(const std::string& lang) {
    // SenseVoice 的语言 id 来自模型 metadata（lang_*）
    if (lang == "zh" || lang == "cmn") return 3;
    if (lang == "en") return 4;
    if (lang == "yue") return 7;
    if (lang == "ja") return 11;
    if (lang == "ko") return 12;
    return 0; // auto
}

} // namespace

namespace detail {

std::vector<float> kaldi_fbank80(const std::vector<float>& pcm16k) {
    if (pcm16k.empty()) return {};

    const int n = static_cast<int>(pcm16k.size());
    const int num_frames = (n + kFrameShift / 2) / kFrameShift;
    const auto window = povey_window(kFrameLength);
    const auto banks = make_mel_banks(kNumMelBins, kFFTSize, kSampleRate, 20.0,
                                      kSampleRate * 0.5);

    std::vector<float> feats(static_cast<std::size_t>(num_frames) * kNumMelBins, 0.0f);
    std::vector<float> frame(kFrameLength);
    std::vector<float> power;

    for (int f = 0; f < num_frames; ++f) {
        const int start = f * kFrameShift - kFrameLength / 2;
        for (int i = 0; i < kFrameLength; ++i) {
            int idx = start + i;
            // kaldi snip_edges=false：越界按镜像反射
            while (idx < 0 || idx >= n) {
                if (idx < 0) {
                    idx = -idx - 1;
                } else {
                    idx = 2 * n - 1 - idx;
                }
            }
            frame[static_cast<std::size_t>(i)] = pcm16k[static_cast<std::size_t>(idx)];
        }

        // 去直流
        double mean = 0.0;
        for (float v : frame) mean += v;
        const auto m = static_cast<float>(mean / kFrameLength);
        for (float& v : frame) v -= m;

        // 预加重
        for (int i = kFrameLength - 1; i > 0; --i) {
            frame[static_cast<std::size_t>(i)] -= kPreemph * frame[static_cast<std::size_t>(i - 1)];
        }
        frame[0] -= kPreemph * frame[0];

        for (int i = 0; i < kFrameLength; ++i) frame[static_cast<std::size_t>(i)] *= window[static_cast<std::size_t>(i)];

        fft_power512(frame.data(), power);

        for (int mb = 0; mb < kNumMelBins; ++mb) {
            double sum = 0.0;
            for (const auto& [bin, weight] : banks.bins[static_cast<std::size_t>(mb)]) {
                sum += power[static_cast<std::size_t>(bin)] * weight;
            }
            const float val = static_cast<float>(std::log(std::max(sum, 1e-10)));
            feats[static_cast<std::size_t>(f) * kNumMelBins + static_cast<std::size_t>(mb)] = val;
        }
    }
    return feats;
}

std::vector<float> lfr(const std::vector<float>& fbank, int m, int n) {
    if (fbank.empty() || m <= 0 || n <= 0) return {};
    const auto feat_dim = static_cast<int>(fbank.size());
    const int num_frames = feat_dim / kNumMelBins;
    if (num_frames <= 0) return {};

    const int out_frames = (num_frames + n - 1) / n;
    const int left_pad = (m - 1) / 2;
    std::vector<float> out(static_cast<std::size_t>(out_frames) * kNumMelBins * m, 0.0f);

    for (int i = 0; i < out_frames; ++i) {
        for (int j = 0; j < m; ++j) {
            int src = i * n + j - left_pad;
            src = std::clamp(src, 0, num_frames - 1);
            for (int d = 0; d < kNumMelBins; ++d) {
                out[(static_cast<std::size_t>(i) * m + static_cast<std::size_t>(j)) * kNumMelBins +
                    static_cast<std::size_t>(d)] =
                    fbank[static_cast<std::size_t>(src) * kNumMelBins + static_cast<std::size_t>(d)];
            }
        }
    }
    return out;
}

void apply_cmvn(std::vector<float>& features, const std::vector<float>& neg_mean,
                const std::vector<float>& inv_stddev) {
    if (neg_mean.empty() || inv_stddev.size() != neg_mean.size()) return;
    const std::size_t dim = neg_mean.size();
    if (features.size() % dim != 0) return;
    for (std::size_t i = 0; i < features.size(); ++i) {
        const std::size_t d = i % dim;
        features[i] = (features[i] + neg_mean[d]) * inv_stddev[d];
    }
}

std::string ctc_greedy_text(const std::vector<int>& token_ids,
                            const std::vector<std::string>& id_to_token, bool strip_tags) {
    std::string text;
    for (int id : token_ids) {
        if (id <= 0 || static_cast<std::size_t>(id) >= id_to_token.size()) continue;
        const std::string& tok = id_to_token[static_cast<std::size_t>(id)];
        if (tok.empty()) continue;
        if (strip_tags) {
            if (tok.size() > 3 && tok.rfind("<|", 0) == 0) continue; // <|zh|> 等控制标签
            if (tok == "<unk>" || tok == "<s>" || tok == "</s>" || tok == "<blank>") continue;
            // SentencePiece 的 ▁ 表示词边界
            std::string clean = tok;
            const std::string marker = "\xE2\x96\x81"; // ▁
            std::size_t pos = 0;
            while ((pos = clean.find(marker, pos)) != std::string::npos) {
                clean.replace(pos, marker.size(), " ");
                ++pos;
            }
            text += clean;
        } else {
            if (tok == "<blank>" || tok == "<s>" || tok == "</s>" || tok == "<unk>") continue;
            if (tok.size() >= 2 && tok.compare(tok.size() - 2, 2, "@@") == 0) {
                text += tok.substr(0, tok.size() - 2); // BPE 续接
            } else {
                text += tok;
            }
        }
    }
    // 收尾：清洗非法 UTF-8（某些词表条目会把多字节字符拆开），再压缩空格。
    std::string cleaned;
    cleaned.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            ++i; // 孤立续字节，丢弃
            continue;
        }
        if (i + len > text.size()) break;
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            ++i;
            continue;
        }
        cleaned.append(text, i, len);
        i += len;
    }
    text = std::move(cleaned);

    std::string out;
    out.reserve(text.size());
    bool prev_space = false;
    for (char c : text) {
        const bool space = c == ' ';
        if (space && prev_space) continue;
        out.push_back(c);
        prev_space = space;
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace detail

#if IRIS_HAS_ONNX
namespace {

/// 读模型 metadata（neg_mean / inv_stddev 等逗号分隔浮点串）。
std::vector<float> read_meta_floats(const Ort::ModelMetadata& meta, const char* key) {
    std::vector<float> out;
    Ort::AllocatorWithDefaultOptions alloc;
    auto value = meta.LookupCustomMetadataMapAllocated(key, alloc);
    if (!value) return out;
    const char* p = value.get();
    while (*p != '\0') {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) break;
        out.push_back(v);
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    return out;
}

bool run_inference(const std::string& model_path, FunasrModel kind, const FunasrOptions& options,
                   const std::vector<float>& features, int n_frames, std::vector<int>& token_ids,
                   std::string& error) {
    const int feat_dim = static_cast<int>(features.size() / std::max(1, n_frames));

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-funasr");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(infra::resolve_threads(options.n_threads));
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    std::wstring wpath(model_path.begin(), model_path.end());
    Ort::Session session(env, wpath.c_str(), opts);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::vector<int64_t> x_shape = {1, n_frames, feat_dim};
    Ort::Value x = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(features.data()),
                                                   features.size(), x_shape.data(), x_shape.size());
    std::vector<int32_t> len_data = {static_cast<int32_t>(n_frames)};
    const std::vector<int64_t> len_shape = {1};
    Ort::Value len = Ort::Value::CreateTensor<int32_t>(mem, len_data.data(), len_data.size(),
                                                       len_shape.data(), len_shape.size());

    Ort::AllocatorWithDefaultOptions alloc;
    std::vector<std::string> in_names, out_names;
    for (std::size_t i = 0; i < session.GetInputCount(); ++i)
        in_names.push_back(session.GetInputNameAllocated(i, alloc).get());
    for (std::size_t i = 0; i < session.GetOutputCount(); ++i)
        out_names.push_back(session.GetOutputNameAllocated(i, alloc).get());

    std::vector<const char*> in_c;
    std::vector<Ort::Value> inputs;
    std::vector<int32_t> lang_data{static_cast<int32_t>(language_id(options.language))};
    std::vector<int32_t> norm_data{14}; // with_itn

    for (const auto& name : in_names) {
        if (name == "x" || name == "speech") {
            in_c.push_back(name.c_str());
            inputs.emplace_back(std::move(x));
        } else if (name == "x_length" || name == "speech_lengths") {
            in_c.push_back(name.c_str());
            inputs.emplace_back(std::move(len));
        } else if (name == "language") {
            in_c.push_back(name.c_str());
            inputs.emplace_back(Ort::Value::CreateTensor<int32_t>(
                mem, lang_data.data(), lang_data.size(), len_shape.data(), len_shape.size()));
        } else if (name == "text_norm") {
            in_c.push_back(name.c_str());
            inputs.emplace_back(Ort::Value::CreateTensor<int32_t>(
                mem, norm_data.data(), norm_data.size(), len_shape.data(), len_shape.size()));
        }
    }
    if (inputs.empty()) {
        error = "funasr: 模型没有可识别的输入（x / speech）";
        return false;
    }

    std::vector<const char*> out_c;
    for (const auto& name : out_names) out_c.push_back(name.c_str());

    auto outputs = session.Run(Ort::RunOptions{}, in_c.data(), inputs.data(), inputs.size(),
                               out_c.data(), out_c.size());
    if (outputs.empty()) {
        error = "funasr: 推理没有输出";
        return false;
    }

    // 第一个输出是 logits [1, T, V]；Paraformer 的第二个输出是 token_num
    const auto info = outputs[0].GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    const float* logits = outputs[0].GetTensorData<float>();
    int n_out_frames = shape.size() >= 2 ? static_cast<int>(shape[1]) : 0;
    int vocab = shape.size() >= 3 ? static_cast<int>(shape[2]) : 0;
    if (kind == FunasrModel::Paraformer && outputs.size() > 1 && shape.size() >= 3) {
        if (outputs[1].IsTensor()) {
            const int32_t* tn = outputs[1].GetTensorData<int32_t>();
            const auto tn_shape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();
            if (tn_shape.size() >= 1 && tn[0] > 0) n_out_frames = std::min(n_out_frames, tn[0]);
        }
    }
    if (n_out_frames <= 0 || vocab <= 0) {
        error = "funasr: logits 形状异常";
        return false;
    }

    token_ids.clear();
    token_ids.reserve(static_cast<std::size_t>(n_out_frames));
    int prev = -1;
    for (int t = 0; t < n_out_frames; ++t) {
        const float* row = logits + static_cast<std::size_t>(t) * vocab;
        int best = 0;
        float best_v = row[0];
        for (int v = 1; v < vocab; ++v) {
            if (row[v] > best_v) {
                best_v = row[v];
                best = v;
            }
        }
        if (best != 0 && best != prev) token_ids.push_back(best);
        prev = best;
    }
    (void)env;
    return true;
}

} // namespace
#endif

bool transcribe_funasr(FunasrModel kind, const FunasrOptions& options, const std::string& wav16k_path,
                       std::vector<model::Sentence>& out, std::string& error) {
    out.clear();
#if !IRIS_HAS_ONNX
    (void)kind;
    (void)options;
    (void)wav16k_path;
    error = "funasr: Iris 未以 onnxruntime 构建（tools/runtimes/onnx 缺失）";
    return false;
#else
    namespace fs = std::filesystem;
    if (!fs::is_directory(options.model_dir)) {
        error = "funasr: 模型目录不存在：" + options.model_dir;
        return false;
    }
    const std::string model_path = find_onnx(options.model_dir);
    if (model_path.empty()) {
        error = "funasr: 目录里没有 model.onnx / model.int8.onnx：" + options.model_dir;
        return false;
    }
    const std::string tokens_path = (fs::path(options.model_dir) / "tokens.txt").string();
    std::vector<std::string> id_to_token;
    if (!load_tokens(tokens_path, id_to_token, error)) return false;

    std::vector<float> pcm;
    std::string read_error;
    if (!read_wav_16k_mono(wav16k_path, pcm, read_error)) {
        error = "funasr: " + read_error;
        return false;
    }
    if (pcm.empty()) {
        error = "funasr: 音频为空";
        return false;
    }

    // 前处理：fbank → LFR → CMVN
    std::vector<float> fbank = detail::kaldi_fbank80(pcm);
    if (fbank.empty()) {
        error = "funasr: fbank 提取失败";
        return false;
    }
    std::vector<float> features = detail::lfr(fbank, 7, 6);
    const int n_frames = static_cast<int>(features.size() / (kNumMelBins * 7));

    // CMVN 参数来自模型 metadata
    {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-funasr-meta");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        std::wstring wpath(model_path.begin(), model_path.end());
        Ort::Session session(env, wpath.c_str(), opts);
        auto meta = session.GetModelMetadata();
        const auto neg_mean = read_meta_floats(meta, "neg_mean");
        const auto inv_stddev = read_meta_floats(meta, "inv_stddev");
        if (!neg_mean.empty()) {
            detail::apply_cmvn(features, neg_mean, inv_stddev);
        } else {
            spdlog::warn("funasr: 模型没有 neg_mean/inv_stddev metadata，跳过 CMVN");
        }
    }

    std::vector<int> token_ids;
    if (!run_inference(model_path, kind, options, features, n_frames, token_ids, error)) {
        return false;
    }

    if (std::getenv("IRIS_FUNASR_DEBUG") != nullptr) {
        std::string dump;
        for (int id : token_ids) {
            if (id <= 0 || static_cast<std::size_t>(id) >= id_to_token.size()) continue;
            dump += std::to_string(id) + ":" ;
            for (unsigned char ch : id_to_token[static_cast<std::size_t>(id)]) {
                if (ch < 0x20 || ch >= 0x7F) {
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "\\x%02X", ch);
                    dump += hex;
                } else {
                    dump.push_back(static_cast<char>(ch));
                }
            }
            dump += " ";
        }
        spdlog::info("funasr tokens: {}", dump);
    }

    const bool strip_tags = (kind == FunasrModel::SenseVoice);
    const std::string text = detail::ctc_greedy_text(token_ids, id_to_token, strip_tags);
    if (text.empty()) {
        error = "funasr: 解码结果为空（可能音频无人声）";
        return false;
    }

    const auto duration_ms =
        static_cast<std::int64_t>(static_cast<double>(pcm.size()) * 1000.0 / kSampleRate);
    model::Sentence s;
    s.start_ms = 0;
    s.end_ms = duration_ms;
    s.text = text;
    out.push_back(std::move(s));

    // 日志按「字符」截断：按字节切会切出半个汉字（显示成 �）
    std::string preview;
    for (std::size_t i = 0, chars = 0; i < text.size() && chars < 60;) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = (c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 1));
        if (i + len > text.size()) break;
        preview.append(text, i, len);
        i += len;
        ++chars;
    }
    spdlog::info("funasr: {} 帧 → {} token → \"{}\"", n_frames, token_ids.size(), preview);
    return true;
#endif
}

} // namespace iris::asr
