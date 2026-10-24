#include "iris/controller/commands.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/pipe_reader.hpp"
#include "iris/infra/process.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/ocr/rapid_ocr.hpp"
#include "iris/ops/vsf.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

/// ffprobe 取视频宽高。
bool probe_size(const std::string& video, int& width, int& height, std::string& error) {
    const auto r = infra::run_process({"ffprobe", "-v", "error", "-select_streams", "v:0",
                                       "-show_entries", "stream=width,height", "-of",
                                       "csv=p=0", video});
    if (!r.ok() || !r.value().success()) {
        error = "ffprobe 失败（ffmpeg 是否在 PATH？）";
        return false;
    }
    const std::string out = r.value().stdout_text;
    if (std::sscanf(out.c_str(), "%d,%d", &width, &height) != 2) {
        error = "无法解析视频尺寸：" + out;
        return false;
    }
    return true;
}

/// 在 t 秒处取一帧，裁出 ROI，返回 RGB24 像素。
bool grab_rgb(const std::string& video, double t_sec, int x, int y, int w, int h,
              std::vector<unsigned char>& rgb, std::string& error) {
    char tbuf[32];
    std::snprintf(tbuf, sizeof tbuf, "%.3f", t_sec);
    char crop[96];
    std::snprintf(crop, sizeof crop, "crop=%d:%d:%d:%d", w, h, x, y);

    infra::StreamProcess proc;
    if (!proc.start({"ffmpeg", "-hide_banner", "-loglevel", "error", "-ss", tbuf, "-i", video,
                     "-frames:v", "1", "-vf", crop, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"})) {
        error = "无法启动 ffmpeg 取帧";
        return false;
    }
    rgb.assign(static_cast<std::size_t>(w) * h * 3, 0);
    const std::size_t got = proc.read_exact(rgb.data(), rgb.size());
    proc.close();
    if (got < rgb.size()) {
        error = "取帧数据不足（可能需要更早的时间点）";
        return false;
    }
    if (std::getenv("IRIS_OCR_DEBUG") != nullptr) {
        static int dump = 0;
        std::ofstream f(".cache/verify/roi_" + std::to_string(dump++) + ".raw", std::ios::binary);
        f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    }
    return true;
}

/// 把多行 OCR 结果合并成一条字幕文本（同一条字幕通常是一两行）。
/// 中文之间不加空格（检测常把一行按空隙切成多块）。
std::string join_lines(const std::vector<ocr::OcrLine>& lines) {
    std::string text;
    for (const auto& line : lines) {
        if (line.text.empty()) continue;
        if (!text.empty()) {
            const bool left_cjk = static_cast<unsigned char>(text.back()) >= 0x80;
            const bool right_cjk = static_cast<unsigned char>(line.text.front()) >= 0x80;
            if (!(left_cjk && right_cjk)) text.push_back(' ');
        }
        text += line.text;
    }
    return text;
}

} // namespace

int run_ocr(const OcrCommandOptions& opts) {
    if (!fs::exists(opts.input)) {
        std::cerr << _("ocr: input not found: ", "ocr: 输入文件不存在：") << opts.input << '\n';
        return 1;
    }

    int width = 0;
    int height = 0;
    std::string error;
    if (!probe_size(opts.input, width, height, error)) {
        std::cerr << "ocr: " << error << '\n';
        return 1;
    }
    if (width <= 0 || height <= 0) {
        std::cerr << _("ocr: no video stream\n", "ocr: 输入里没有视频流\n");
        return 1;
    }

    // ROI：优先用 --roi；否则自动标定（按行亮度分布找字幕带）；再不行退回下方 30%。
    int rx = 0;
    int ry = static_cast<int>(height * 0.70);
    int rw = width;
    int rh = height - ry;
    if (opts.roi.empty()) {
        const auto auto_roi = ops::detect_text_roi(opts.input);
        if (auto_roi.ok) {
            rx = auto_roi.x;
            ry = auto_roi.y;
            rw = auto_roi.w;
            rh = auto_roi.h;
        }
    } else {
        int px = 0, py = 0, pw = 0, ph = 0;
        if (std::sscanf(opts.roi.c_str(), "%d,%d,%d,%d", &px, &py, &pw, &ph) == 4) {
            rx = std::clamp(px, 0, width - 1);
            ry = std::clamp(py, 0, height - 1);
            rw = std::clamp(pw, 1, width - rx);
            rh = std::clamp(ph, 1, height - ry);
        } else {
            std::cerr << _("ocr: --roi must be x,y,w,h\n", "ocr: --roi 格式应为 x,y,w,h\n");
            return 1;
        }
    }
    // ffmpeg 的 crop 在 yuv420p 上会把宽高等取整为偶数，这里先对齐，避免读到的字节数不符。
    rx = std::max(0, rx & ~1);
    ry = std::max(0, ry & ~1);
    rw = std::max(2, rw & ~1);
    rh = std::max(2, rh & ~1);
    spdlog::info("ocr: 视频 {}x{}，ROI {},{} {}x{}", width, height, rx, ry, rw, rh);

    // 1) 找字幕段（vsf）
    // 字幕条只占 ROI 的一小部分，vsf 的四信号分数会被稀释，所以单独给一个更低的阈值。
    const auto segments = ops::run_vsf(opts.input, rx, ry, rw, rh, opts.sample_fps, 0.10);
    if (segments.empty()) {
        std::cerr << _("ocr: no subtitle segments detected\n", "ocr: 没有检测到字幕段\n");
        return 1;
    }

    // 2) 逐段取帧 + OCR
    ocr::RapidOcr engine;
    ocr::OcrOptions ocr_opts;
    ocr_opts.model_dir = opts.model_dir;
    if (!engine.open(ocr_opts, error)) {
        std::cerr << "ocr: " << error << '\n';
        return 1;
    }

    struct Cue {
        double start = 0.0;
        double end = 0.0;
        std::string text;
        float score = 0.0f;
    };
    std::vector<Cue> cues;

    // 一段「有字幕」可能横跨多条不同的字幕（比如连续的对话），所以段内按固定步长
    // 多次采样，文本一变就切一条 cue；相邻相同文本合并。
    const auto same_text = [](const std::string& a, const std::string& b) {
        const auto norm = [](const std::string& s) {
            std::string out;
            for (char c : s) {
                if (c != ' ' && c != '\t') out.push_back(c);
            }
            return out;
        };
        return norm(a) == norm(b);
    };

    int sampled = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];
        const double span = std::max(0.2, seg.end_sec - seg.start_sec);
        const double step = std::clamp(span / 6.0, 0.3, 1.0);
        for (double t = seg.start_sec; t <= seg.end_sec + 1e-6; t += step) {
            std::vector<unsigned char> rgb;
            if (!grab_rgb(opts.input, t, rx, ry, rw, rh, rgb, error)) {
                spdlog::warn("ocr: [{}/{}] {:.2f}s 取帧失败（{}），跳过", i + 1, segments.size(), t, error);
                continue;
            }
            std::vector<ocr::OcrLine> lines;
            if (!engine.recognize_rgb(rgb.data(), rw, rh, lines, error)) {
                spdlog::error("ocr: [{}/{}] 识别失败：{}", i + 1, segments.size(), error);
                return 1;
            }
            std::string text = join_lines(lines);
            if (text.empty()) continue;
            float score = 0.0f;
            for (const auto& line : lines) score = std::max(score, line.score);
            ++sampled;
            spdlog::info("ocr: {:.2f}s → \"{}\"（{:.2f}）", t, text, static_cast<double>(score));

            if (!cues.empty() && same_text(cues.back().text, text)) {
                cues.back().end = t;
            } else {
                // 文本换了：上一句到此刻为止，新句子从此刻开始
                if (!cues.empty()) cues.back().end = std::max(cues.back().end, t);
                cues.push_back({t, t, std::move(text), score});
            }
        }
    }
    // 收尾：最后一条的结束时间取所在段的终点
    if (!cues.empty() && !segments.empty()) {
        cues.back().end = std::max(cues.back().end, segments.back().end_sec);
        if (cues.back().end <= cues.back().start) cues.back().end = cues.back().start + 0.3;
    }
    if (sampled == 0) {
        std::cerr << _("ocr: nothing recognised\n", "ocr: 没有识别出任何文本\n");
        return 1;
    }

    if (cues.empty()) {
        std::cerr << _("ocr: nothing recognised\n", "ocr: 没有识别出任何文本\n");
        return 1;
    }

    // 3) 写 IR
    model::IrDocument doc;
    doc.generator.version = kVersion;
    doc.generator.command = "ocr";
    doc.generator.generated_at = "now";
    doc.generator.input_file = opts.input;
    doc.config.input_file_path = opts.input;
    doc.config.command_name = "ocr";
    doc.config.language = opts.language;
    doc.config.transcriber_engine = "ocr";

    model::Track track;
    track.type = "transcribe";
    track.language = opts.language;
    for (const auto& cue : cues) {
        model::Sentence s;
        s.start_ms = static_cast<std::int64_t>(std::llround(cue.start * 1000.0));
        s.end_ms = static_cast<std::int64_t>(std::llround(cue.end * 1000.0));
        s.text = cue.text;
        track.sentences.push_back(std::move(s));
    }
    doc.state.tracks.push_back(std::move(track));
    doc.provenance.push_back({"ocr", opts.input, std::nullopt, std::nullopt});

    const std::string out = opts.out_ir.empty() ? opts.input + ".ocr.iris.json" : opts.out_ir;
    doc.save(out);
    std::cout << _("ocr: cues ", "ocr: 字幕条数 ") << cues.size() << _(" → ", " → ") << out << '\n';
    return 0;
}

} // namespace iris::controller
