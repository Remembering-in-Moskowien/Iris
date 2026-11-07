#include "iris/vsf/vsf_api.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

std::vector<unsigned char> to_gray(const unsigned char* rgb, int width, int height) {
    std::vector<unsigned char> gray(static_cast<std::size_t>(width) * height);
    for (int i = 0; i < width * height; ++i) {
        gray[static_cast<std::size_t>(i)] = static_cast<unsigned char>(
            (306 * rgb[i * 3] + 601 * rgb[i * 3 + 1] + 117 * rgb[i * 3 + 2] + 512) >> 10);
    }
    return gray;
}

} // namespace

extern "C" IRIS_VSF_API int iris_vsf_abi_version(void) {
    return 1;
}

extern "C" IRIS_VSF_API double iris_vsf_score_rgb(const unsigned char* rgb,
                                                    int width, int height) {
    if (rgb == nullptr || width < 3 || height < 3) return 0.0;

    const auto gray = to_gray(rgb, width, height);
    std::vector<int> row_hits(static_cast<std::size_t>(height), 0);
    std::vector<int> col_hits(static_cast<std::size_t>(width), 0);
    int bright = 0;
    int edges = 0;

    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            const int i = y * width + x;
            const int r = rgb[i * 3];
            const int g = rgb[i * 3 + 1];
            const int b = rgb[i * 3 + 2];
            const bool light = gray[static_cast<std::size_t>(i)] >= 175 &&
                               std::max({r, g, b}) - std::min({r, g, b}) < 80;
            const bool yellow = r > 150 && g > 120 && b < 150 && r > b * 1.25;
            if (light || yellow) {
                ++bright;
                ++row_hits[static_cast<std::size_t>(y)];
                ++col_hits[static_cast<std::size_t>(x)];
            }

            const int gx = -gray[static_cast<std::size_t>(i - width - 1)] -
                           2 * gray[static_cast<std::size_t>(i - 1)] -
                           gray[static_cast<std::size_t>(i + width - 1)] +
                           gray[static_cast<std::size_t>(i - width + 1)] +
                           2 * gray[static_cast<std::size_t>(i + 1)] +
                           gray[static_cast<std::size_t>(i + width + 1)];
            const int gy = -gray[static_cast<std::size_t>(i - width - 1)] -
                           2 * gray[static_cast<std::size_t>(i - width)] -
                           gray[static_cast<std::size_t>(i - width + 1)] +
                           gray[static_cast<std::size_t>(i + width - 1)] +
                           2 * gray[static_cast<std::size_t>(i + width)] +
                           gray[static_cast<std::size_t>(i + width + 1)];
            if (std::abs(gx) + std::abs(gy) > 160) ++edges;
        }
    }

    const int pixels = std::max(1, (width - 2) * (height - 2));
    const double bright_fraction = static_cast<double>(bright) / pixels;
    const double edge_fraction = static_cast<double>(edges) / pixels;
    const int peak_rows = *std::max_element(row_hits.begin(), row_hits.end());
    const double row_concentration = static_cast<double>(peak_rows) / std::max(1, width);
    const double col_span = static_cast<double>(
        std::count_if(col_hits.begin(), col_hits.end(), [](int n) { return n > 0; })) /
        std::max(1, width);
    return 0.35 * std::min(bright_fraction * 14.0, 1.0) +
           0.20 * std::min(edge_fraction * 8.0, 1.0) +
           0.30 * std::min(row_concentration * 3.0, 1.0) +
           0.15 * std::min(col_span * 2.0, 1.0);
}
