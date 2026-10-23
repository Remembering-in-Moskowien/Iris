// ============================================================================
// 语音情感分类（wav2vec2-base, DunnBC22/wav2vec2-base-SER 微调, int8 ONNX）
// 运行时引用 onnxruntime DLL。
//
// 输入：16kHz 单声道波形（float32, per-utterance 归一化）
// 输出：6 类 softmax（SAD/ANGRY/DISGUST/FEAR/HAPPY/NEUTRAL）
// 模型：~95MB int8，CPU 推理 <200ms。
// ============================================================================

#include "iris/emotion/emotion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#include <spdlog/fmt/fmt.h>
#endif

namespace iris::emotion {

#if IRIS_HAS_ONNX
namespace {

const char* kLabels[6] = {"SAD", "ANGRY", "DISGUST", "FEAR", "HAPPY", "NEUTRAL"};

bool read_wav_16k(const std::string& path, std::vector<float>& pcm, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "无法打开 WAV：" + path; return false; }
    char riff[4]; f.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0) { err = "非 RIFF/WAV"; return false; }
    f.seekg(4, std::ios::cur);
    char wave[4]; f.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0) { err = "非 WAVE"; return false; }
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
    if (bps != 16) { err = fmt::format("仅支持 16-bit PCM（当前 {}bit）", bps); return false; }
    const int16_t* s16 = reinterpret_cast<const int16_t*>(data.data());
    size_t n = data.size() / 2;
    pcm.resize(n);
    for (size_t i = 0; i < n; ++i) pcm[i] = s16[i] / 32768.0f;
    return true;
}

} // namespace
#endif

bool analyze_emotion(const std::string& onnx_path,
                     const std::string& wav_path,
                     EmotionResult& out,
                     std::string& error) {
#if !IRIS_HAS_ONNX
    error = "Iris 未以 ONNX 运行时构建";
    return false;
#else
    std::vector<float> pcm;
    if (!read_wav_16k(wav_path, pcm, error)) return false;
    if (pcm.empty()) { error = "WAV 为空"; return false; }

    // per-utterance 归一化（do_normalize=true：减均值除标准差）
    double mean = 0.0;
    for (float v : pcm) mean += v;
    mean /= pcm.size();
    double var = 0.0;
    for (float v : pcm) var += (v - mean) * (v - mean);
    var /= pcm.size();
    float stdv = static_cast<float>(std::sqrt(var + 1e-7));
    for (auto& v : pcm) v = static_cast<float>((v - mean) / stdv);

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-emotion");
    Ort::SessionOptions so_opts;
    so_opts.SetIntraOpNumThreads(4);
    std::wstring wpath;
    for (const char c : onnx_path) wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    std::unique_ptr<Ort::Session> session;
    try {
        session = std::make_unique<Ort::Session>(env, wpath.c_str(), so_opts);
    } catch (const Ort::Exception& e) {
        error = i18n::tr("ONNX model load failed", "ONNX 模型加载失败");
        spdlog::debug("emotion ONNX exception: {}", e.what());
        return false;
    }

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> shape = {1, static_cast<int64_t>(pcm.size())};
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, pcm.data(), pcm.size(),
                                                    shape.data(), shape.size());

    Ort::AllocatorWithDefaultOptions alloc;
    std::string in_name = session->GetInputNameAllocated(0, alloc).get();
    std::string out_name = session->GetOutputNameAllocated(0, alloc).get();
    const char* in_names[1] = {in_name.c_str()};
    const char* out_names[1] = {out_name.c_str()};
    auto outs = session->Run(Ort::RunOptions{}, in_names, &in, 1, out_names, 1);

    float* logits = outs[0].GetTensorMutableData<float>();
    auto info = outs[0].GetTensorTypeAndShapeInfo();
    auto dims = info.GetShape();
    int n_cls = dims.size() >= 2 ? static_cast<int>(dims[1]) : 6;
    if (n_cls > 6) n_cls = 6;

    // softmax
    float maxv = logits[0];
    for (int i = 1; i < n_cls; ++i) maxv = std::max(maxv, logits[i]);
    float sum = 0.0f;
    for (int i = 0; i < n_cls; ++i) {
        out.scores[i] = std::exp(logits[i] - maxv);
        sum += out.scores[i];
    }
    int best = 0;
    for (int i = 0; i < n_cls; ++i) {
        out.scores[i] /= sum;
        if (out.scores[i] > out.scores[best]) best = i;
    }
    out.label = kLabels[best];
    out.score = out.scores[best];
    return true;
#endif
}

} // namespace iris::emotion
