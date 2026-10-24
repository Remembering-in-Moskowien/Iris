#include "iris/ops/vsf.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <map>

#include "iris/infra/log.hpp"
#include "iris/infra/pipe_reader.hpp"
#include "iris/infra/process.hpp"

namespace iris::ops {
namespace {

// ---------- 纯 C++ 图像处理（无 OpenCV） ----------

std::vector<unsigned char> to_gray(const unsigned char* rgb, int w, int h) {
    std::vector<unsigned char> g(static_cast<std::size_t>(w) * h);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char r = rgb[i * 3 + 0];
        const unsigned char gn = rgb[i * 3 + 1];
        const unsigned char b = rgb[i * 3 + 2];
        g[i] = static_cast<unsigned char>((306 * r + 601 * gn + 117 * b + 512) >> 10);
    }
    return g;
}

// 二值化：>= thresh 记 1
std::vector<unsigned char> threshold(const std::vector<unsigned char>& g, int thresh) {
    std::vector<unsigned char> b(g.size());
    for (std::size_t i = 0; i < g.size(); ++i) b[i] = g[i] >= thresh ? 1 : 0;
    return b;
}

// HSV 黄色 mask：RGB→HSV，H∈[10,45] S∈[80,255] V∈[180,255]
std::vector<unsigned char> yellow_mask(const unsigned char* rgb, int w, int h) {
    std::vector<unsigned char> m(static_cast<std::size_t>(w) * h, 0);
    for (int i = 0; i < w * h; ++i) {
        const double r = rgb[i * 3] / 255.0;
        const double g = rgb[i * 3 + 1] / 255.0;
        const double b = rgb[i * 3 + 2] / 255.0;
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
        const double v = mx, d = mx - mn;
        if (v < 180.0 / 255.0) continue;
        const double s = mx > 1e-6 ? d / mx : 0.0;
        if (s < 80.0 / 255.0) continue;
        double hue = 0.0;
        if (d > 1e-6) {
            if (mx == r) hue = std::fmod((g - b) / d, 6.0);
            else if (mx == g) hue = (b - r) / d + 2.0;
            else hue = (r - g) / d + 4.0;
            hue *= 60.0;
            if (hue < 0) hue += 360.0;
        }
        if (hue >= 10.0 && hue <= 45.0) m[i] = 1;
    }
    return m;
}

// Sobel 边缘密度（简化 Canny 前置）
double edge_density(const std::vector<unsigned char>& g, int w, int h) {
    if (w < 3 || h < 3) return 0.0;
    int edges = 0;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const int i = y * w + x;
            const int gx = -g[i - w - 1] - 2 * g[i - 1] - g[i + w - 1]
                          + g[i - w + 1] + 2 * g[i + 1] + g[i + w + 1];
            const int gy = -g[i - w - 1] - 2 * g[i - w] - g[i - w + 1]
                          + g[i + w - 1] + 2 * g[i + w] + g[i + w + 1];
            if (std::abs(gx) + std::abs(gy) > 120) ++edges;
        }
    }
    return static_cast<double>(edges) / (w * h);
}

// 连通域面积分数（两遍法）：统计宽高比 1.5~15、面积>=80 的文本块占比
double text_blob_score(const std::vector<unsigned char>& bin, int w, int h) {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<int> labels(n, -1);
    std::vector<std::pair<int, int>> boxes; // (pixels, blob_index)

    int next_label = 0;
    // 第一遍
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = y * w + x;
            if (!bin[i]) continue;
            int left = x > 0 ? labels[i - 1] : -1;
            int up = y > 0 ? labels[i - w] : -1;
            if (left < 0 && up < 0) {
                labels[i] = next_label++;
            } else if (left >= 0 && up < 0) {
                labels[i] = left;
            } else if (left < 0 && up >= 0) {
                labels[i] = up;
            } else {
                labels[i] = std::min(left, up);
                // 合并等价类（简化：向上传播）
                if (left != up) {
                    const int lo = std::min(left, up), hi = std::max(left, up);
                    for (auto& l : labels) if (l == hi) l = lo;
                }
            }
        }
    }
    // 统计每块面积与包围盒
    std::map<int, int> area, minx, maxx, miny, maxy;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int l = labels[y * w + x];
            if (l < 0) continue;
            area[l]++;
            if (!minx.count(l)) { minx[l] = maxx[l] = x; miny[l] = maxy[l] = y; }
            minx[l] = std::min(minx[l], x); maxx[l] = std::max(maxx[l], x);
            miny[l] = std::min(miny[l], y); maxy[l] = std::max(maxy[l], y);
        }
    }
    int text_pixels = 0;
    for (const auto& [l, a] : area) {
        const int bw = maxx[l] - minx[l] + 1;
        const int bh = maxy[l] - miny[l] + 1;
        if (a < 80) continue;
        const double aspect = static_cast<double>(bw) / std::max(bh, 1);
        if (aspect < 1.5 || aspect > 15.0) continue;
        text_pixels += a;
    }
    return static_cast<double>(text_pixels) / (w * h);
}

double fraction_nonzero(const std::vector<unsigned char>& b) {
    int c = 0;
    for (auto v : b) c += v;
    return static_cast<double>(c) / b.size();
}

} // namespace

std::vector<VsfSegment> run_vsf(const std::string& video_path,
                                 int roi_x, int roi_y, int roi_w, int roi_h,
                                 double sample_fps, double score_threshold) {
    std::vector<VsfSegment> out;

    // 1. ffprobe 取分辨率
    auto pr = infra::run_process({
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height",
        "-of", "csv=p=0", video_path});
    if (!pr.ok() || !pr.value().success()) {
        spdlog::error("vsf: ffprobe 失败（ffmpeg/ffprobe 是否在 PATH？）");
        return out;
    }
    int W = 0, H = 0;
    std::sscanf(pr.value().stdout_text.c_str(), "%d,%d", &W, &H);
    if (W <= 0 || H <= 0) {
        spdlog::error("vsf: 无法解析视频分辨率");
        return out;
    }

    // ROI 边界夹紧
    const int rx = std::max(0, roi_x);
    const int ry = std::max(0, roi_y);
    const int rw = std::min(roi_w, W - rx);
    const int rh = std::min(roi_h, H - ry);
    if (rw <= 0 || rh <= 0) {
        spdlog::error("vsf: ROI 越界（视频 {}x{}，请求 {},{},{},{}）",
                      W, H, roi_x, roi_y, roi_w, roi_h);
        return out;
    }
    spdlog::info("vsf: {}x{} ROI={},{},{},{} 采样 {} fps", W, H, rx, ry, rw, rh, sample_fps);

    // 2. 启动 ffmpeg 抽帧
    infra::StreamProcess proc;
    if (!proc.start({"ffmpeg", "-hide_banner", "-loglevel", "error",
                     "-i", video_path,
                     "-vf", "fps=" + std::to_string(sample_fps),
                     "-f", "rawvideo", "-pix_fmt", "rgb24", "-"})) {
        spdlog::error("vsf: 无法启动 ffmpeg");
        return out;
    }

    const std::size_t frame_bytes = static_cast<std::size_t>(W) * H * 3;
    std::vector<unsigned char> frame(frame_bytes);

    // 时序聚合：时间窗口（秒），采样率无关
    struct Sample { bool has; double t; };
    std::deque<Sample> window;
    const double window_sec = 2.0;     // 时间窗
    const double min_span_sec = 1.0;   // 窗内需覆盖至少 1 秒
    const int min_hits = 2;            // 窗内至少 2 帧有字幕

    // 段构建
    bool active = false;
    VsfSegment cur;

    int frame_idx = 0;
    while (true) {
        const std::size_t got = proc.read_exact(frame.data(), frame_bytes);
        if (got < frame_bytes) break;

        const double t = frame_idx / sample_fps;
        ++frame_idx;

        // 裁剪 ROI（拷贝子区域）
        std::vector<unsigned char> roi(static_cast<std::size_t>(rw) * rh * 3);
        for (int y = 0; y < rh; ++y) {
            std::copy_n(frame.data() + ((ry + y) * W + rx) * 3,
                        static_cast<std::size_t>(rw) * 3,
                        roi.begin() + static_cast<std::size_t>(y) * rw * 3);
        }

        // 四信号
        auto gray = to_gray(roi.data(), rw, rh);
        auto bright = threshold(gray, 200);
        auto yellow = yellow_mask(roi.data(), rw, rh);
        const double sBright = fraction_nonzero(bright);
        const double sYellow = fraction_nonzero(yellow);
        const double sEdge = edge_density(gray, rw, rh);
        const double sBlob = text_blob_score(bright, rw, rh);

        const double score =
            0.30 * std::min(sBright * 5.0, 1.0) +
            0.20 * std::min(sYellow * 8.0, 1.0) +
            0.25 * std::min(sEdge * 4.0, 1.0) +
            0.25 * std::min(sBlob * 6.0, 1.0);
        const bool has = score > score_threshold;

        window.push_back({has, t});
        while (!window.empty() && t - window.front().t > window_sec) window.pop_front();

        bool stable = false;
        if (!window.empty()) {
            const double span = window.back().t - window.front().t;
            int hits = 0;
            for (const auto& s : window) if (s.has) ++hits;
            stable = span >= min_span_sec && hits >= min_hits;
        }

        spdlog::debug("vsf t={:.2f} score={:.3f} has={} stable={}", t, score, has, stable);

        if (stable && has) {
            if (!active) {
                active = true;
                cur = VsfSegment{t, t, score, rx, ry, rw, rh};
            }
            cur.end_sec = t;
            cur.confidence = std::max(cur.confidence, score);
        } else if (active && !has) {
            // 段结束（最小时长 0.3s 过滤）
            if (cur.end_sec - cur.start_sec >= 0.3) out.push_back(cur);
            active = false;
        }
    }
    if (active && cur.end_sec - cur.start_sec >= 0.3) out.push_back(cur);

    spdlog::info("vsf: 检测到 {} 个字幕段", out.size());
    return out;
}

VsfRoi detect_text_roi(const std::string& video_path) {
    VsfRoi roi;
    auto pr = infra::run_process({"ffprobe", "-v", "error", "-select_streams", "v:0",
                                  "-show_entries", "stream=width,height",
                                  "-of", "csv=p=0", video_path});
    if (!pr.ok() || !pr.value().success()) return roi;
    int W = 0, H = 0;
    std::sscanf(pr.value().stdout_text.c_str(), "%d,%d", &W, &H);
    if (W <= 0 || H <= 0) return roi;

    auto dr = infra::run_process({"ffprobe", "-v", "error", "-show_entries", "format=duration",
                                  "-of", "csv=p=0", video_path});
    double duration = 0.0;
    if (dr.ok() && dr.value().success()) {
        duration = std::atof(dr.value().stdout_text.c_str());
    }
    if (duration <= 0.0) return roi;

    // 只在下半部分找（字幕绝大多数在画面下部），按行累计亮像素
    const int y_start = static_cast<int>(H * 0.5);
    const int band_h = H - y_start;
    std::vector<double> row_density(static_cast<std::size_t>(band_h), 0.0);
    int left = W, right = -1;
    int samples = 0;

    const int kSamples = 10;
    for (int i = 0; i < kSamples; ++i) {
        const double t = duration * (i + 0.5) / kSamples;
        char tbuf[32];
        std::snprintf(tbuf, sizeof tbuf, "%.3f", t);
        char crop[96];
        std::snprintf(crop, sizeof crop, "crop=%d:%d:0:%d", W, band_h, y_start);
        infra::StreamProcess proc;
        if (!proc.start({"ffmpeg", "-hide_banner", "-loglevel", "error", "-ss", tbuf, "-i",
                         video_path, "-frames:v", "1", "-vf", crop, "-f", "rawvideo",
                         "-pix_fmt", "rgb24", "-"})) {
            continue;
        }
        std::vector<unsigned char> frame(static_cast<std::size_t>(W) * band_h * 3, 0);
        const std::size_t got = proc.read_exact(frame.data(), frame.size());
        proc.close();
        if (got < frame.size()) continue;
        ++samples;

        for (int y = 0; y < band_h; ++y) {
            const unsigned char* row = frame.data() + static_cast<std::size_t>(y) * W * 3;
            for (int x = 0; x < W; ++x) {
                const int g = (0.299 * row[x * 3] + 0.587 * row[x * 3 + 1] + 0.114 * row[x * 3 + 2]);
                if (g >= 200) {
                    row_density[static_cast<std::size_t>(y)] += 1.0;
                    left = std::min(left, x);
                    right = std::max(right, x);
                }
            }
        }
    }
    if (samples == 0 || right < 0) return roi;

    // 找最密集的一行，按峰值 20% 扩展成连续带
    const auto peak_it = std::max_element(row_density.begin(), row_density.end());
    const double peak = *peak_it;
    if (peak <= 0.0) return roi;
    int peak_row = static_cast<int>(peak_it - row_density.begin());
    int top = peak_row;
    int bottom = peak_row;
    while (top > 0 && row_density[static_cast<std::size_t>(top - 1)] > peak * 0.2) --top;
    while (bottom + 1 < band_h && row_density[static_cast<std::size_t>(bottom + 1)] > peak * 0.2) {
        ++bottom;
    }

    const int pad = 8;
    roi.y = std::max(0, y_start + top - pad);
    roi.h = std::min(H - roi.y, bottom - top + 1 + 2 * pad);
    roi.x = std::max(0, left - 20);
    roi.w = std::min(W - roi.x, right - left + 1 + 40);
    roi.ok = roi.w > 16 && roi.h > 8;
    if (roi.ok) {
        spdlog::info("vsf: 自动 ROI = {},{} {}x{}（峰值行 {}）", roi.x, roi.y, roi.w, roi.h,
                     y_start + peak_row);
    }
    return roi;
}

} // namespace iris::ops
