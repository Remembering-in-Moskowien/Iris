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
                                 double sample_fps = 0.5,
                                 double score_threshold = 0.35);

/// 自动标定字幕 ROI：按行统计亮像素分布，找出最密集的水平带（再按列收敛左右边界）。
/// 失败时 ok=false（调用方退回「画面下方固定比例」）。
struct VsfRoi {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool ok = false;
};

VsfRoi detect_text_roi(const std::string& video_path);

} // namespace iris::ops
