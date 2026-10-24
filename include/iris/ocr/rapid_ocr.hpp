#pragma once

#include <string>
#include <vector>

namespace iris::ocr {

// ---------------------------------------------------------------------------
// 本地 OCR（PP-OCRv5，onnxruntime，CPU）：
//   det = DBNet 文本检测（概率图 → 二值化 → 连通域 → 框）
//   rec = CRNN + CTC 识别（灰度 48 高，字典 ppocrv5_dict.txt）
//
// 只做「字幕图 → 文本」，不依赖 OpenCV 与任何图像解码库：
// 输入是调用方给的 RGB 像素（通常由 ffmpeg rawvideo 裁好字幕区域后喂进来）。
// ---------------------------------------------------------------------------

struct OcrLine {
    std::string text;
    float score = 0.0f;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct OcrOptions {
    std::string model_dir = "models/ocr"; ///< 含 det/rec onnx 与 ppocrv5_dict.txt
    int n_threads = 0;
    float det_threshold = 0.3f;  ///< 概率图二值化阈值
    float box_threshold = 0.5f;  ///< 框平均置信度下限
    float unclip_ratio = 1.5f;   ///< 框外扩比例（PaddleOCR 的 unclip）
};

class RapidOcr {
public:
    RapidOcr() = default;
    ~RapidOcr();
    RapidOcr(const RapidOcr&) = delete;
    RapidOcr& operator=(const RapidOcr&) = delete;

    bool open(const OcrOptions& options, std::string& error);
    [[nodiscard]] bool ready() const { return ready_; }

    /// 识别一张 RGB 图里的所有文本行（按从上到下、从左到右排序）。
    bool recognize_rgb(const unsigned char* rgb, int width, int height, std::vector<OcrLine>& out,
                       std::string& error);

private:
    bool ready_ = false;
    OcrOptions opts_;
    void* env_ = nullptr;
    void* det_ = nullptr;
    void* rec_ = nullptr;
    std::vector<std::string> dict_;
};

namespace detail {

/// 检测框（原图坐标）。
struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float score = 0.0f;
};

/// 概率图 → 连通域 → 框（含最小尺寸与置信度过滤），按 y 再按 x 排序。
std::vector<Box> boxes_from_mask(const std::vector<float>& prob, int width, int height,
                                 float threshold, float box_threshold);

/// 按 PaddleOCR 的 unclip 公式外扩：d = area * ratio / perimeter。
std::vector<Box> expand_boxes(std::vector<Box> boxes, float ratio, int max_w, int max_h);

/// CTC 贪心解码：0 为 blank，1..N 对应 dict[0..N-1]，末尾额外一维是空格。
std::string ctc_decode(const std::vector<float>& logits, int steps, int classes,
                       const std::vector<std::string>& dict, float* score);

} // namespace detail

} // namespace iris::ocr
