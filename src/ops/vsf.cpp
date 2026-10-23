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

// 默认 ROI：底部 75%~95%
void default_roi(int w, int h, int& rx, int& ry, int& rw, int& rh) {
    rx = 0; ry = static_cast<int>(h * 0.75);
    rw = w; rh = static_cast<int>(h * 0.95) - ry;
}

} // namespace

std::vector<VsfSegment> run_vsf(const std::string& video_path, double sample_fps) {
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
    spdlog::info("vsf: {}x{} 采样 {} fps", W, H, sample_fps);

    // 2. 启动 ffmpeg 抽帧
    infra::StreamProcess proc;
    if (!proc.start({"ffmpeg", "-hide_banner", "-loglevel", "error",
                     "-i", video_path,
                     "-vf", "fps=" + std::to_string(sample_fps),
                     "-f", "rawvideo", "-pix_fmt", "rgb24", "-"})) {
        spdlog::error("vsf: 无法启动 ffmpeg");
        return out;
    }

    int rx, ry, rw, rh;
    default_roi(W, H, rx, ry, rw, rh);

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
        const bool has = score > 0.35;

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

} // namespace iris::ops
