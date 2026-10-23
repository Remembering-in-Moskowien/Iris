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

/// 自实现 VSF：ffmpeg rawvideo 流式抽帧 → 精准 ROI 裁剪（用户传参）→ 四信号融合单帧检测
/// → 时间窗口时序聚合 → 段构建。不依赖 OpenCV、不自动校准 ROI。
/// roi: 精准绝对坐标 (x, y, w, h)。sample_fps: 抽帧采样率。
std::vector<VsfSegment> run_vsf(const std::string& video_path,
                                 int roi_x, int roi_y, int roi_w, int roi_h,
                                 double sample_fps = 0.5);

} // namespace iris::ops
