#pragma once

#include <string>
#include <vector>

namespace iris::tts {

// ---------------------------------------------------------------------------
// 变速不变调（WSOLA：Waveform Similarity Overlap-Add）。
//
// 与 ffmpeg 的 atempo 同类思路，但由 Iris 自己实现，好处是：
//   - 不依赖外部进程，参数（窗长/搜索范围）可按配音场景调；
//   - 相关性搜索在「上一帧尾部的自然延续」上做，避免相位跳变造成的金属声；
//   - 短句（< 40 ms）不会被拉成噪声。
//
// speed > 1 表示加速（输出变短）；speed <= 1 原样返回（dub 只做压缩）。
// ---------------------------------------------------------------------------

/// 用 WSOLA 把 pcm 变速为 speed 倍。sample_rate 决定窗长（默认 40 ms 窗、50% 重叠）。
/// 失败（参数非法）返回 false 并写 error；out 在成功时长度约为 in.size() / speed。
bool wsola_stretch(const std::vector<float>& in, int sample_rate, float speed,
                   std::vector<float>& out, std::string& error);

} // namespace iris::tts
