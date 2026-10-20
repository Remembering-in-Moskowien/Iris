#include "iris/asr/ct2_mel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

// 80 通道官方 mel 滤波器组（源自 whisper.cpp 模型内嵌 filters，
// 即 OpenAI whisper assets/mel_filters.npz 的 mel_80）
#include "iris/asr/mel_filters_80.inc"

namespace iris::asr {

namespace {

// whisper.cpp 常数（与 OpenAI whisper / faster-whisper 一致）。
constexpr std::size_t kSampleRate = 16000;
constexpr std::size_t kNfft = 400;
constexpr std::size_t kHopLength = 160;
constexpr std::size_t kNLen = kNfft / 2 + 1;         // 201
constexpr std::size_t kWindowSamples = 30 * kSampleRate; // 480000
constexpr std::size_t kNFrames = kWindowSamples / kHopLength; // 3000

void hann_window(std::vector<float>& win) {
    // periodic Hann（torch.hann_window(periodic=True)，与 whisper.cpp fill_hann_window 一致）
    win.resize(kNfft);
    for (std::size_t i = 0; i < kNfft; ++i) {
        win[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979323846f * static_cast<float>(i) /
                                          static_cast<float>(kNfft)));
    }
}

// OpenAI whisper / faster-whisper 的 mel 滤波器组。
// 80 通道版直接使用 whisper.cpp 模型内嵌的官方 filters（mel_filters_80.inc，
// 源自 OpenAI whisper assets/mel_filters.npz 的 mel_80），保证与模型训练分布逐值一致。
// 128 通道（large-v3）暂用 HTK mel 刻度 + Slaney enorm 归一化公式计算。
void mel_filter_bank(std::size_t n_mel, std::vector<float>& filters) {
    if (n_mel == 80) {
        filters.assign(kMelFilters80, kMelFilters80 + 80 * kNLen);
        return;
    }

    constexpr double f_min = 0.0;
    constexpr double f_max = 8000.0;
    // mel(f_max) 即 mel 刻度上界（单位 mel 对应的对数步长分母）
    const double kMelStepDenom = 2595.0 * std::log10(1.0 + f_max / 700.0);

    const auto mel_of = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
    const auto freq_of = [&](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };

    std::vector<double> mels(n_mel + 2);
    for (std::size_t i = 0; i < n_mel + 2; ++i) {
        mels[i] = freq_of(mel_of(f_min) + static_cast<double>(i) * kMelStepDenom /
                                             static_cast<double>(n_mel + 1));
    }
    // Slaney-style：enorm = 2/(mels[2:] - mels[:-2])
    std::vector<double> enorm(n_mel);
    for (std::size_t i = 0; i < n_mel; ++i) {
        enorm[i] = 2.0 / (mels[i + 2] - mels[i]);
    }

    filters.assign(n_mel * kNLen, 0.0f);
    for (std::size_t j = 0; j < n_mel; ++j) {
        const double lo = mels[j], mid = mels[j + 1], hi = mels[j + 2];
        for (std::size_t k = 0; k < kNLen; ++k) {
            // FFT bin k 的中心频率 = k * (采样率 / n_fft) = k * 16000/400 = k*40 Hz
            const double freq = static_cast<double>(kSampleRate) * static_cast<double>(k) /
                                static_cast<double>(kNfft);
            const double m = mel_of(freq);
            double up = 0.0, down = 0.0;
            if (m >= lo && m <= mid) {
                up = (m - lo) / (mid - lo);
            }
            if (m >= mid && m <= hi) {
                down = (hi - m) / (hi - mid);
            }
            filters[j * kNLen + k] = static_cast<float>((up + down) * enorm[j]);
        }
    }
}

// 单窗 STFT：power spectrum（幅值平方），与 whisper.cpp 一致。
void stft_power(const std::vector<float>& samples, std::size_t offset,
                const std::vector<float>& win, std::vector<float>& spec) {
    const std::size_t n = std::min(kNfft, samples.size() - offset);
    spec.assign(kNLen, 0.0f);
    for (std::size_t k = 0; k < kNLen; ++k) {
        double re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(samples[offset + i]) * win[i];
            const double ang = 2.0 * 3.14159265358979323846 * static_cast<double>(k) *
                               static_cast<double>(i) / static_cast<double>(kNfft);
            re += x * std::cos(ang);
            im -= x * std::sin(ang);
        }
        spec[k] = static_cast<float>(re * re + im * im);
    }
}

} // namespace

bool read_wav_16k_mono(const std::string& wav_path, std::vector<float>& samples, std::string& error) {
    std::ifstream in(wav_path, std::ios::binary);
    if (!in) {
        error = "无法打开 WAV 文件: " + wav_path;
        return false;
    }

    char hdr[44];
    in.read(hdr, 44);
    if (in.gcount() < 44 || std::string(hdr, 4) != "RIFF" || std::string(hdr + 8, 4) != "WAVE") {
        error = "不是有效的 RIFF/WAVE 文件: " + wav_path;
        return false;
    }

    const auto u16 = [&](std::size_t off) {
        return static_cast<std::uint16_t>(static_cast<unsigned char>(hdr[off]) |
                                          (static_cast<unsigned char>(hdr[off + 1]) << 8));
    };
    const auto u32 = [&](std::size_t off) {
        return static_cast<std::uint32_t>(
            static_cast<unsigned char>(hdr[off]) | (static_cast<unsigned char>(hdr[off + 1]) << 8) |
            (static_cast<unsigned char>(hdr[off + 2]) << 16) |
            (static_cast<unsigned char>(hdr[off + 3]) << 24));
    };

    const std::uint16_t audio_format = u16(20);
    const std::uint16_t num_channels = u16(22);
    const std::uint32_t sample_rate = u32(24);
    const std::uint16_t bits_per_sample = u16(34);

    if (audio_format != 1 || num_channels != 1 || sample_rate != kSampleRate ||
        bits_per_sample != 16) {
        error = "需要 16 kHz 单声道 16-bit PCM WAV（当前: " +
                std::to_string(sample_rate) + " Hz / " + std::to_string(num_channels) +
                " ch / " + std::to_string(bits_per_sample) + " bit）";
        return false;
    }

    samples.clear();
    std::int16_t s16;
    while (in.read(reinterpret_cast<char*>(&s16), 2)) {
        samples.push_back(static_cast<float>(s16) / 32768.0f);
    }
    if (samples.empty()) {
        error = "WAV 中没有音频数据: " + wav_path;
        return false;
    }
    return true;
}

bool log_mel_spectrogram_30s(const std::vector<float>& samples, std::size_t n_mels,
                             std::vector<float>& mel, std::string& error) {
    if (samples.empty()) {
        error = "空音频";
        return false;
    }
    if (n_mels != 80 && n_mels != 128) {
        error = "不支持的 mel 通道数: " + std::to_string(n_mels);
        return false;
    }

    // 只取前 30 秒；不足补零（与 faster-whisper 一致）。
    std::vector<float> window(kWindowSamples, 0.0f);
    const std::size_t n = std::min(samples.size(), kWindowSamples);
    std::copy_n(samples.begin(), static_cast<std::ptrdiff_t>(n), window.begin());

    std::vector<float> win;
    hann_window(win);
    std::vector<float> filters;
    mel_filter_bank(n_mels, filters);

    mel.assign(n_mels * kNFrames, 0.0f);
    std::vector<float> spec(kNLen);
    for (std::size_t frame = 0; frame < kNFrames; ++frame) {
        const std::size_t offset = frame * kHopLength;
        stft_power(window, offset, win, spec);

        // 对数 mel 幅值：log10(max(mel 能量, 1e-10))。
        for (std::size_t j = 0; j < n_mels; ++j) {
            double sum = 0.0;
            for (std::size_t k = 0; k < kNLen; ++k) {
                sum += static_cast<double>(spec[k]) * filters[j * kNLen + k];
            }
            mel[j * kNFrames + frame] = static_cast<float>(std::log10(std::max(sum, 1e-10)));
        }
    }

    // 归一化（whisper.cpp）：不低于窗口最大值 -8，再 (x+4)/4。
    float mmax = -1e20f;
    for (const float v : mel) mmax = std::max(mmax, v);
    for (float& v : mel) {
        v = std::max(v, mmax - 8.0f);
        v = (v + 4.0f) / 4.0f;
    }
    return true;
}

} // namespace iris::asr
