#pragma once

#include <string>
#include <vector>

namespace iris::ops {

/// VSF（Video Subtitle Finder）输出的字幕段：仅时间轴 + ROI + 置信度，不含 OCR 文本。
struct VsfSegment {
    double start_sec = 0.0;
    double end_sec = 0.0;
    double confidence = 0.0;
    int roi_x = 0;
    int roi_y = 0;
    int roi_w = 0;
    int roi_h = 0;
};

/// 自实现 VSF：ffmpeg rawvideo 流式抽帧 → ROI 裁剪 → 四信号融合单帧检测
/// → 滑动窗口时序聚合 → 段构建。不依赖 OpenCV。
/// sample_fps：抽帧采样率（默认 0.5 = 每 2 秒一帧）。
std::vector<VsfSegment> run_vsf(const std::string& video_path, double sample_fps = 0.5);

} // namespace iris::ops
