// ============================================================================
// 语音情感分析（Wav2Small 风格 ONNX）—— 运行时引用 onnxruntime DLL。
//
// 模型：72K 参数 VGG7 + Bayesian pool，输入 16kHz 单声道波形，
//       输出 Arousal / Dominance / Valence 三维连续值（0~1）。
// 量化后 ~60KB，CPU 推理 <10ms。
// ============================================================================

#include "iris/emotion/emotion.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#include <spdlog/fmt/fmt.h>
#endif

namespace iris::emotion {

#if IRIS_HAS_ONNX
namespace {

// 读 16kHz 单声道 WAV → float32 [-1,1]
bool read_wav_16k(const std::string& path, std::vector<float>& pcm, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "无法打开 WAV：" + path; return false; }
    char riff[4]; f.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0) { err = "非 RIFF/WAV"; return false; }
    f.seekg(4, std::ios::cur);
    char wave[4]; f.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0) { err = "非 WAVE"; return false; }
    // 遍历 chunk 找 fmt / data
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
    (void)channels;
    (void)sample_rate;
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

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-emotion");
    Ort::SessionOptions so_opts;
    so_opts.SetIntraOpNumThreads(1);
    std::wstring wpath;
    for (const char c : onnx_path) wpath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    Ort::Session session(env, wpath.c_str(), so_opts);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> shape = {1, static_cast<int64_t>(pcm.size())};
    Ort::Value in = Ort::Value::CreateTensor<float>(mem, pcm.data(), pcm.size(),
                                                    shape.data(), shape.size());

    const char* in_names[] = {"waveform"};
    const char* out_names[] = {"predictions"};
    auto outs = session.Run(Ort::RunOptions{}, in_names, &in, 1, out_names, 1);

    float* ptr = outs[0].GetTensorMutableData<float>();
    auto info = outs[0].GetTensorTypeAndShapeInfo();
    auto dims = info.GetShape();
    // 输出 shape: (1, 3) → arousal, dominance, valence
    out.arousal    = dims.size() >= 2 && dims[1] >= 3 ? ptr[0] : ptr[0];
    out.dominance  = dims.size() >= 2 && dims[1] >= 3 ? ptr[1] : 0.5f;
    out.valence    = dims.size() >= 2 && dims[1] >= 3 ? ptr[2] : 0.5f;
    return true;
#endif
}

} // namespace iris::emotion
