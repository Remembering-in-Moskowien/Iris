#include "iris/controller/commands.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "iris/infra/log.hpp"
#include "iris/ops/vsf.hpp"

namespace iris::controller {
namespace {

std::string srt_timestamp(double sec) {
    const int h = static_cast<int>(sec) / 3600;
    const int m = static_cast<int>(sec) / 60 % 60;
    const int s = static_cast<int>(sec) % 60;
    const int ms = static_cast<int>((sec - std::floor(sec)) * 1000);
    std::ostringstream os;
    os << std::setfill('0') << std::setw(2) << h << ':'
       << std::setw(2) << m << ':' << std::setw(2) << s << ','
       << std::setw(3) << ms;
    return os.str();
}

} // namespace

int run_vsf(const std::string& input, const std::string& output,
             int roi_x, int roi_y, int roi_w, int roi_h, double sample_fps) {
    if (input.empty()) {
        std::cerr << "vsf: 缺少输入视频\n";
        return 1;
    }
    auto segs = ops::run_vsf(input, roi_x, roi_y, roi_w, roi_h, sample_fps);
    if (segs.empty()) {
        std::cout << "vsf: 未检测到字幕段\n";
        return 0;
    }

    std::string out = output;
    if (out.empty()) {
        out = input;
        const auto p = out.find_last_of('.');
        if (p != std::string::npos) out = out.substr(0, p);
        out += ".vsf.srt";
    }

    std::ofstream ofs(out);
    if (!ofs) {
        std::cerr << "vsf: 无法写入 " << out << "\n";
        return 1;
    }
    for (std::size_t i = 0; i < segs.size(); ++i) {
        ofs << (i + 1) << '\n';
        ofs << srt_timestamp(segs[i].start_sec) << " --> "
            << srt_timestamp(segs[i].end_sec) << '\n';
        ofs << "<!-- roi=" << segs[i].roi_x << ',' << segs[i].roi_y
            << ',' << segs[i].roi_w << ',' << segs[i].roi_h
            << " conf=" << std::fixed << std::setprecision(2) << segs[i].confidence
            << " -->\n\n";
    }
    std::cout << "vsf: 写出 " << segs.size() << " 段 -> " << out << '\n';
    return 0;
}

} // namespace iris::controller
