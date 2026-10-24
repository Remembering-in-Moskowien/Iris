#include "iris/ocr/rapid_ocr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <queue>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"
#include "iris/infra/threads.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace iris::ocr {

namespace {

constexpr int kDetMaxSide = 960;
constexpr int kRecHeight = 48;
constexpr int kRecMaxWidth = 640;

/// 双线性缩放（RGB），返回 [h*w*3] float。
std::vector<float> resize_rgb(const unsigned char* rgb, int sw, int sh, int dw, int dh) {
    std::vector<float> out(static_cast<std::size_t>(dw) * dh * 3, 0.0f);
    for (int y = 0; y < dh; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, sh - 1);
        const int y1 = std::min(y0 + 1, sh - 1);
        const float wy = std::clamp(sy - static_cast<float>(y0), 0.0f, 1.0f);
        for (int x = 0; x < dw; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, sw - 1);
            const int x1 = std::min(x0 + 1, sw - 1);
            const float wx = std::clamp(sx - static_cast<float>(x0), 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                const auto p00 = rgb[(static_cast<std::size_t>(y0) * sw + x0) * 3 + c];
                const auto p01 = rgb[(static_cast<std::size_t>(y0) * sw + x1) * 3 + c];
                const auto p10 = rgb[(static_cast<std::size_t>(y1) * sw + x0) * 3 + c];
                const auto p11 = rgb[(static_cast<std::size_t>(y1) * sw + x1) * 3 + c];
                const float top = p00 + (p01 - p00) * wx;
                const float bot = p10 + (p11 - p10) * wx;
                out[(static_cast<std::size_t>(y) * dw + x) * 3 + c] = top + (bot - top) * wy;
            }
        }
    }
    return out;
}

/// 取整到 stride 的倍数（det 输入要求 32 的倍数）。
int round_to(int v, int stride) { return std::max(stride, (v + stride / 2) / stride * stride); }

} // namespace

namespace detail {

std::vector<Box> boxes_from_mask(const std::vector<float>& prob, int width, int height,
                                 float threshold, float box_threshold) {
    std::vector<Box> boxes;
    if (width <= 0 || height <= 0 || prob.size() < static_cast<std::size_t>(width) * height) {
        return boxes;
    }
    std::vector<char> visited(static_cast<std::size_t>(width) * height, 0);
    std::queue<std::pair<int, int>> q;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto idx = static_cast<std::size_t>(y) * width + x;
            if (visited[idx] || prob[idx] < threshold) continue;

            int min_x = x, max_x = x, min_y = y, max_y = y;
            double sum = 0.0;
            int count = 0;
            visited[idx] = 1;
            q.push({x, y});
            while (!q.empty()) {
                const auto [cx, cy] = q.front();
                q.pop();
                const auto ci = static_cast<std::size_t>(cy) * width + cx;
                sum += prob[ci];
                ++count;
                min_x = std::min(min_x, cx);
                max_x = std::max(max_x, cx);
                min_y = std::min(min_y, cy);
                max_y = std::max(max_y, cy);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx;
                        const int ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                        const auto ni = static_cast<std::size_t>(ny) * width + nx;
                        if (visited[ni] || prob[ni] < threshold) continue;
                        visited[ni] = 1;
                        q.push({nx, ny});
                    }
                }
            }
            Box box;
            box.x = min_x;
            box.y = min_y;
            box.w = max_x - min_x + 1;
            box.h = max_y - min_y + 1;
            box.score = count > 0 ? static_cast<float>(sum / count) : 0.0f;
            if (box.w < 3 || box.h < 3) continue;
            if (box.score < box_threshold) continue;
            boxes.push_back(box);
        }
    }

    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
        if (std::abs(a.y - b.y) > std::max(a.h, b.h) / 2) return a.y < b.y;
        return a.x < b.x;
    });
    return boxes;
}

std::vector<Box> expand_boxes(std::vector<Box> boxes, float ratio, int max_w, int max_h) {
    for (auto& box : boxes) {
        const double area = static_cast<double>(box.w) * box.h;
        const double perimeter = 2.0 * (box.w + box.h);
        const double d = perimeter > 0.0 ? area * ratio / perimeter : 0.0;
        const int pad = static_cast<int>(std::round(d));
        box.x -= pad;
        box.y -= pad;
        box.w += 2 * pad;
        box.h += 2 * pad;
        if (box.x < 0) {
            box.w += box.x;
            box.x = 0;
        }
        if (box.y < 0) {
            box.h += box.y;
            box.y = 0;
        }
        box.w = std::min(box.w, max_w - box.x);
        box.h = std::min(box.h, max_h - box.y);
    }
    boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                               [](const Box& b) { return b.w <= 1 || b.h <= 1; }),
                boxes.end());
    return boxes;
}

std::string ctc_decode(const std::vector<float>& logits, int steps, int classes,
                       const std::vector<std::string>& dict, float* score) {
    std::string text;
    double score_sum = 0.0;
    int kept = 0;
    int prev = -1;
    for (int t = 0; t < steps; ++t) {
        const float* row = logits.data() + static_cast<std::size_t>(t) * classes;
        int best = 0;
        float best_v = row[0];
        float sum = 0.0f;
        for (int c = 0; c < classes; ++c) {
            sum += std::exp(row[c] - best_v); // 数值稳定的 softmax 分母（后续迭代）
            if (row[c] > best_v) {
                best_v = row[c];
                best = c;
            }
        }
        (void)sum;
        if (best != 0 && best != prev) {
            if (best >= 1 && static_cast<std::size_t>(best) <= dict.size()) {
                text += dict[static_cast<std::size_t>(best - 1)];
            } else if (best == static_cast<int>(dict.size()) + 1) {
                text += ' ';
            }
            score_sum += best_v;
            ++kept;
        }
        prev = best;
    }
    if (score != nullptr) *score = kept > 0 ? static_cast<float>(score_sum / kept) : 0.0f;
    return text;
}

} // namespace detail

RapidOcr::~RapidOcr() {
#if IRIS_HAS_ONNX
    delete static_cast<Ort::Session*>(det_);
    delete static_cast<Ort::Session*>(rec_);
    delete static_cast<Ort::Env*>(env_);
#endif
}

bool RapidOcr::open(const OcrOptions& options, std::string& error) {
#if !IRIS_HAS_ONNX
    (void)options;
    error = "ocr: Iris 未以 onnxruntime 构建（tools/runtimes/onnx 缺失）";
    return false;
#else
    namespace fs = std::filesystem;
    opts_ = options;
    const fs::path dir(options.model_dir);
    const fs::path det = dir / "PP-OCRv5_det.onnx";
    const fs::path rec = dir / "ch_PP-OCRv5_rec_mobile_infer.onnx";
    const fs::path dict = dir / (fs::exists(dir / "ppocrv5_dict.txt") ? "ppocrv5_dict.txt" : "ppocr_keys_v1.txt");
    for (const auto& p : {det, rec, dict}) {
        if (!fs::exists(p)) {
            error = "ocr: 缺少模型文件 " + p.string();
            return false;
        }
    }

    std::ifstream f(dict);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        dict_.push_back(line);
    }
    if (dict_.empty()) {
        error = "ocr: 字典为空：" + dict.string();
        return false;
    }

    const int threads = infra::resolve_threads(options.n_threads);
    try {
        auto* env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "iris-ocr");
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(threads);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        std::wstring wdet = det.wstring();
        std::wstring wrec = rec.wstring();
        auto* d = new Ort::Session(*env, wdet.c_str(), so);
        auto* r = new Ort::Session(*env, wrec.c_str(), so);
        env_ = env;
        det_ = d;
        rec_ = r;
    } catch (const Ort::Exception& e) {
        error = std::string("ocr: 模型加载失败：") + e.what();
        return false;
    }
    ready_ = true;
    spdlog::info("ocr: PP-OCRv5 就绪（字典 {} 字，{} 线程）", dict_.size(), threads);
    return true;
#endif
}

bool RapidOcr::recognize_rgb(const unsigned char* rgb, int width, int height,
                             std::vector<OcrLine>& out, std::string& error) {
    out.clear();
#if !IRIS_HAS_ONNX
    (void)rgb;
    (void)width;
    (void)height;
    error = "ocr: 未构建 onnxruntime";
    return false;
#else
    if (!ready_) {
        error = "ocr: 模型未加载";
        return false;
    }
    if (rgb == nullptr || width <= 1 || height <= 1) {
        error = "ocr: 图像为空";
        return false;
    }

    Ort::Env& env = *static_cast<Ort::Env*>(env_);
    Ort::Session& det = *static_cast<Ort::Session*>(det_);
    Ort::Session& rec = *static_cast<Ort::Session*>(rec_);
    (void)env;
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::AllocatorWithDefaultOptions alloc;

    // ---- 检测 ----
    const float scale = std::min(1.0f, static_cast<float>(kDetMaxSide) /
                                          static_cast<float>(std::max(width, height)));
    const int dw = std::max(32, round_to(static_cast<int>(std::round(width * scale)), 32));
    const int dh = std::max(32, round_to(static_cast<int>(std::round(height * scale)), 32));
    const auto resized = resize_rgb(rgb, width, height, dw, dh);
    const bool bgr = std::getenv("IRIS_OCR_RGB") == nullptr; // PP-OCR 默认按 BGR 训练

    std::vector<float> det_input(static_cast<std::size_t>(3) * dw * dh, 0.0f);
    static const float mean[3] = {0.485f, 0.456f, 0.406f};
    static const float stdv[3] = {0.229f, 0.224f, 0.225f};
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < dw * dh; ++i) {
            const float v = resized[static_cast<std::size_t>(i) * 3 + (bgr ? (2 - c) : c)] / 255.0f;
            det_input[static_cast<std::size_t>(c) * dw * dh + i] = (v - mean[c]) / stdv[c];
        }
    }

    const std::vector<int64_t> det_shape = {1, 3, dh, dw};
    auto det_x = Ort::Value::CreateTensor<float>(mem, det_input.data(), det_input.size(),
                                                 det_shape.data(), det_shape.size());
    std::vector<std::string> det_in_names;
    for (std::size_t i = 0; i < det.GetInputCount(); ++i) {
        det_in_names.push_back(det.GetInputNameAllocated(i, alloc).get());
    }
    std::vector<const char*> det_in;
    for (const auto& n : det_in_names) det_in.push_back(n.c_str());
    // 必须把 AllocatedStringPtr 存住：临时对象在语句结束就释放，get() 会变成悬垂指针。
    const auto det_out_alloc = det.GetOutputNameAllocated(0, alloc);
    const char* det_out_name = det_out_alloc.get();
    std::vector<Ort::Value> det_outputs;
    try {
        det_outputs = det.Run(Ort::RunOptions{}, det_in.data(), &det_x, 1, &det_out_name, 1);
    } catch (const Ort::Exception& e) {
        error = std::string("ocr: 检测失败：") + e.what();
        return false;
    }
    const auto det_info = det_outputs[0].GetTensorTypeAndShapeInfo();
    const auto det_shape_out = det_info.GetShape();
    const int ph = static_cast<int>(det_shape_out.size() >= 3 ? det_shape_out[2] : 0);
    const int pw = static_cast<int>(det_shape_out.size() >= 3 ? det_shape_out[3] : 0);
    const std::size_t det_elems = det_info.GetElementCount();
    std::vector<float> prob(det_elems);
    std::copy(det_outputs[0].GetTensorData<float>(),
              det_outputs[0].GetTensorData<float>() + det_elems, prob.begin());

    auto boxes = detail::boxes_from_mask(prob, pw, ph, opts_.det_threshold, opts_.box_threshold);
    const bool debug = std::getenv("IRIS_OCR_DEBUG") != nullptr;
    if (debug) spdlog::info("ocr: prob {}x{} 最大 {:.2f}，检出 {} 个框", pw, ph, *std::max_element(prob.begin(), prob.end()), boxes.size());
    if (boxes.empty()) return true; // 没有文字

    // 概率图坐标 → 原图坐标
    const float sx = static_cast<float>(width) / static_cast<float>(pw);
    const float sy = static_cast<float>(height) / static_cast<float>(ph);
    for (auto& b : boxes) {
        b.x = static_cast<int>(std::round(b.x * sx));
        b.y = static_cast<int>(std::round(b.y * sy));
        b.w = static_cast<int>(std::round(b.w * sx));
        b.h = static_cast<int>(std::round(b.h * sy));
    }
    boxes = detail::expand_boxes(std::move(boxes), opts_.unclip_ratio, width, height);

    // ---- 识别 ----
    std::vector<std::string> rec_in_names;
    for (std::size_t i = 0; i < rec.GetInputCount(); ++i) {
        rec_in_names.push_back(rec.GetInputNameAllocated(i, alloc).get());
    }
    std::vector<const char*> rec_in;
    for (const auto& n : rec_in_names) rec_in.push_back(n.c_str());
    const auto rec_out_alloc = rec.GetOutputNameAllocated(0, alloc);
    const char* rec_out_name = rec_out_alloc.get();

    std::vector<OcrLine> lines;
    for (const auto& box : boxes) {
        const int bw = std::max(1, std::min(box.w, width - box.x));
        const int bh = std::max(1, std::min(box.h, height - box.y));
        int rw = std::max(16, static_cast<int>(std::round(static_cast<float>(kRecHeight) * bw / bh)));
        rw = std::min(rw, kRecMaxWidth);

        // 子图的真实行距是父图宽度：先拷成紧凑缓冲，再缩放（否则每行错位成条纹）。
        std::vector<unsigned char> packed(static_cast<std::size_t>(bw) * bh * 3);
        for (int y = 0; y < bh; ++y) {
            std::copy_n(rgb + (static_cast<std::size_t>(box.y + y) * width + box.x) * 3,
                        static_cast<std::size_t>(bw) * 3,
                        packed.begin() + static_cast<std::size_t>(y) * bw * 3);
        }
        const auto crop = resize_rgb(packed.data(), bw, bh, rw, kRecHeight);

        if (debug) {
            double mean = 0.0;
            for (float v : crop) mean += v;
            mean /= static_cast<double>(crop.size());
            spdlog::info("ocr: crop {}x{} → {}x{} mean={:.1f}", bw, bh, rw, kRecHeight, mean);
            static int dump_idx = 0;
            if (dump_idx < 3) {
                std::ofstream pgm(".cache/verify/ocr_crop_" + std::to_string(dump_idx++) + ".pgm", std::ios::binary);
                pgm << "P5\n" << rw << " " << kRecHeight << "\n255\n";
                for (int yy = 0; yy < kRecHeight; ++yy) {
                    for (int xx = 0; xx < rw; ++xx) {
                        const float v = crop[(static_cast<std::size_t>(yy) * rw + xx) * 3];
                        pgm.put(static_cast<char>(std::clamp(v, 0.0f, 255.0f)));
                    }
                }
            }
        }
        std::vector<float> rec_input(static_cast<std::size_t>(3) * rw * kRecHeight, 0.0f);
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < rw * kRecHeight; ++i) {
                const float v = crop[static_cast<std::size_t>(i) * 3 + (bgr ? (2 - c) : c)] / 255.0f;
                rec_input[static_cast<std::size_t>(c) * rw * kRecHeight + i] = (v - 0.5f) / 0.5f;
            }
        }
        const std::vector<int64_t> rec_shape = {1, 3, kRecHeight, rw};
        auto rec_x = Ort::Value::CreateTensor<float>(mem, rec_input.data(), rec_input.size(),
                                                     rec_shape.data(), rec_shape.size());
        std::vector<Ort::Value> rec_outputs;
        try {
            rec_outputs = rec.Run(Ort::RunOptions{}, rec_in.data(), &rec_x, 1, &rec_out_name, 1);
        } catch (const Ort::Exception& e) {
            error = std::string("ocr: 识别失败：") + e.what();
            return false;
        }
        const auto rec_info = rec_outputs[0].GetTensorTypeAndShapeInfo();
        const auto rec_shape_out = rec_info.GetShape();
        const int steps = static_cast<int>(rec_shape_out.size() >= 2 ? rec_shape_out[1] : 0);
        const int classes = static_cast<int>(rec_shape_out.size() >= 3 ? rec_shape_out[2] : 0);
        const std::size_t rec_elems = rec_info.GetElementCount();
        std::vector<float> logits(rec_elems);
        std::copy(rec_outputs[0].GetTensorData<float>(),
                  rec_outputs[0].GetTensorData<float>() + rec_elems, logits.begin());

        float score = 0.0f;
        const std::string text = detail::ctc_decode(logits, steps, classes, dict_, &score);
        if (debug) {
            float lo = logits[0], hi = logits[0];
            for (float v : logits) { lo = std::min(lo, v); hi = std::max(hi, v); }
            std::string argmaxes;
            for (int t2 = 0; t2 < std::min(steps, 6); ++t2) {
                int best = 0; float bv = logits[static_cast<std::size_t>(t2) * classes];
                for (int c = 1; c < classes; ++c) {
                    const float v = logits[static_cast<std::size_t>(t2) * classes + c];
                    if (v > bv) { bv = v; best = c; }
                }
                argmaxes += std::to_string(best) + "(" + std::to_string(bv).substr(0, 5) + ") ";
            }
            spdlog::info("ocr: logits lo={} hi={} 前几步 argmax={}", lo, hi, argmaxes);
        }
        if (debug) spdlog::info("ocr: box {}x{} @{},{} steps={} classes={} -> \"{}\"", bw, bh, box.x, box.y, steps, classes, text);
        if (text.empty()) continue;
        OcrLine line;
        line.text = text;
        line.score = score;
        line.x = box.x;
        line.y = box.y;
        line.w = box.w;
        line.h = box.h;
        lines.push_back(std::move(line));
    }

    out = std::move(lines);
    return true;
#endif
}

} // namespace iris::ocr
