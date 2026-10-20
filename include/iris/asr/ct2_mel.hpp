#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace iris::asr {

/// 读取 16 kHz 单声道 16-bit WAV（ffmpeg 产物），转为 float32 PCM。
/// 返回 false 并填充 error 时表示文件缺失/格式不支持。
bool read_wav_16k_mono(const std::string& wav_path,
                       std::vector<float>& samples,
                       std::string& error);

/// 计算 log-mel 频谱（CTranslate2 Whisper 输入特征）。
///
/// 算法移植自 whisper.cpp 的 log_mel_spectrogram（MIT）——
/// 与 OpenAI whisper / faster-whisper 的特征一致：
///   n_fft=400、hop=160、Hann 窗、HTK mel 刻度（f_min=0, f_max=8000）、
///   log10 幅值、按窗口最大值归一化到 [-8, 0] 再缩放 (x+4)/4。
///
/// 输入 samples 为 16 kHz float32 单声道；最多取 30 秒（480000 样本）。
/// 输出 mel 形状为 [n_mels, n_frames]，n_frames 固定为 3000（不足补零）。
bool log_mel_spectrogram_30s(const std::vector<float>& samples,
                             std::size_t n_mels,
                             std::vector<float>& mel,
                             std::string& error);

} // namespace iris::asr
