#include "iris/asr/separate.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "iris/infra/log.hpp"
#include "iris/infra/process.hpp"
#include "iris/infra/threads.hpp"

#if IRIS_HAS_CRISPASR
#include "crispasr.h"
#include "crispasr_session.h"
#endif

namespace iris::asr {

namespace {

constexpr int kSeparationRate = 44100;
constexpr int kChunkSeconds = 16;   ///< 分块长度（重叠 1 s）
constexpr int kOverlapSeconds = 1;

/// 读 16-bit PCM WAV 为 float32（保留原声道数与采样率，交错存放）。
bool read_wav_raw(const std::string& path, std::vector<float>& pcm, int& rate, int& channels,
                  std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开 WAV：" + path;
        return false;
    }
    char hdr[12];
    if (!f.read(hdr, 12) || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        error = "不是 RIFF/WAVE：" + path;
        return false;
    }
    int bits = 0;
    bool have_fmt = false;
    std::vector<char> data;
    while (f) {
        char cid[4];
        char szb[4];
        if (!f.read(cid, 4) || !f.read(szb, 4)) break;
        const auto sz = static_cast<std::uint32_t>(
            static_cast<unsigned char>(szb[0]) | (static_cast<unsigned char>(szb[1]) << 8) |
            (static_cast<unsigned char>(szb[2]) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(szb[3])) << 24));
        if (std::memcmp(cid, "fmt ", 4) == 0) {
            char fmt[16] = {};
            const std::uint32_t take = std::min<std::uint32_t>(sz, sizeof fmt);
            if (!f.read(fmt, take)) break;
            if (take >= 16) {
                channels = static_cast<unsigned char>(fmt[2]) | (static_cast<unsigned char>(fmt[3]) << 8);
                rate = static_cast<int>(static_cast<unsigned char>(fmt[4]) |
                                        (static_cast<unsigned char>(fmt[5]) << 8) |
                                        (static_cast<unsigned char>(fmt[6]) << 16) |
                                        (static_cast<std::uint32_t>(static_cast<unsigned char>(fmt[7])) << 24));
                bits = static_cast<unsigned char>(fmt[14]) | (static_cast<unsigned char>(fmt[15]) << 8);
                have_fmt = true;
            }
            if (sz > take) f.seekg(sz - take, std::ios::cur);
        } else if (std::memcmp(cid, "data", 4) == 0) {
            data.resize(sz);
            if (sz > 0 && !f.read(data.data(), static_cast<std::streamsize>(sz))) break;
            break;
        } else {
            f.seekg(sz, std::ios::cur);
        }
    }
    if (!have_fmt || bits != 16 || channels <= 0 || rate <= 0 || data.empty()) {
        error = "WAV 不可用（需要 16-bit PCM）：" + path;
        return false;
    }
    const auto* s16 = reinterpret_cast<const std::int16_t*>(data.data());
    pcm.resize(data.size() / 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = s16[i] / 32768.0f;
    return true;
}

/// 写 16-bit PCM WAV（支持多声道交错）。
bool write_wav_raw(const std::string& path, const std::vector<float>& pcm, int rate, int channels,
                   std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        error = "无法写入 WAV：" + path;
        return false;
    }
    const auto data_size = static_cast<std::uint32_t>(pcm.size() * 2);
    const auto write32 = [&](std::uint32_t v) {
        char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                     static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
        f.write(b, 4);
    };
    const auto write16 = [&](std::uint16_t v) {
        char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
        f.write(b, 2);
    };
    f.write("RIFF", 4);
    write32(36 + data_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    write32(16);
    write16(1);
    write16(static_cast<std::uint16_t>(channels));
    write32(static_cast<std::uint32_t>(rate));
    write32(static_cast<std::uint32_t>(rate * channels * 2));
    write16(static_cast<std::uint16_t>(channels * 2));
    write16(16);
    f.write("data", 4);
    write32(data_size);
    for (float v : pcm) {
        const float clamped = std::clamp(v, -1.0f, 1.0f);
        const auto s = static_cast<std::int16_t>(clamped * 32767.0f);
        f.write(reinterpret_cast<const char*>(&s), 2);
    }
    return true;
}

bool run_ffmpeg(const std::vector<std::string>& args, std::string& error) {
    const auto r = infra::run_process(args);
    if (!r.ok() || !r.value().success()) {
        error = "ffmpeg 失败：" + (args.size() > 1 ? args[args.size() - 1] : std::string());
        return false;
    }
    return true;
}

#if IRIS_HAS_CRISPASR
/// 一个分离会话（每个工作线程持有一个，模型只加载一次）。
struct SepSession {
    crispasr_session* session = nullptr;
    std::vector<std::string> stems;

    ~SepSession() {
        if (session != nullptr) crispasr_session_close(session);
    }
};

/// 加载模型并缓存 stem 名单。
bool open_session(const std::string& model, const std::string& backend, SepSession& out,
                  std::string& error) {
    out.session = crispasr_session_open_explicit(model.c_str(), backend.c_str(), 1);
    if (out.session == nullptr) {
        error = "separate: 无法加载模型 " + model + "（后端 " + backend + "）";
        return false;
    }
    return true;
}

/// 分离一段（frames 为每声道采样数），取出目标 stem（交错立体声）。
bool separate_chunk(SepSession& sep, const float* pcm, int frames, const std::string& stem,
                    std::vector<float>& out, std::string& error) {
    const int n_stems = crispasr_session_separate(sep.session, pcm, frames);
    if (n_stems <= 0) {
        error = "separate: 分离失败（n_stems=" + std::to_string(n_stems) + "）";
        return false;
    }
    int target = -1;
    std::string names;
    for (int i = 0; i < n_stems; ++i) {
        const char* n = crispasr_session_separate_stem_name(sep.session, i);
        const std::string s = n != nullptr ? n : "";
        names += (i ? ", " : "") + s;
        if (s == stem) target = i;
    }
    if (target < 0) {
        error = "separate: 模型没有 stem '" + stem + "'（可用：" + names + "）";
        return false;
    }
    int n = 0;
    const float* data = crispasr_session_separate_stem(sep.session, target, &n);
    if (data == nullptr || n <= 0) {
        error = "separate: 取 stem 数据失败";
        return false;
    }
    out.assign(data, data + static_cast<std::size_t>(n) * 2); // 交错立体声
    return true;
}
#endif

} // namespace

std::string default_separation_model() {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (const char* env = std::getenv("IRIS_SEPARATION_MODEL"); env != nullptr && *env != '\0') {
        if (fs::exists(env, ec)) return env;
    }

    // 搜索顺序：~/.cache/crispasr → ./models/separation → ./models/*/
    // 优先 htdemucs（4 段、54 MB、比 mel-band-roformer 快 25%）。
    std::vector<fs::path> dirs;
    if (const char* home = std::getenv("USERPROFILE"); home != nullptr && *home != '\0') {
        dirs.push_back(fs::path(home) / ".cache" / "crispasr");
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        dirs.push_back(fs::path(home) / ".cache" / "crispasr");
    }
    dirs.push_back(fs::path("models") / "separation");
    dirs.push_back(fs::path("models"));

    for (const char* pattern : {"htdemucs", "mel-band-roformer"}) {
        for (const auto& dir : dirs) {
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                const std::string name = entry.path().filename().string();
                if (name.find(pattern) != std::string::npos &&
                    entry.path().extension() == ".gguf") {
                    return entry.path().string();
                }
            }
            for (const auto& sub : fs::directory_iterator(dir, ec)) {
                if (ec || !sub.is_directory()) continue;
                std::error_code inner;
                for (const auto& entry : fs::directory_iterator(sub.path(), inner)) {
                    if (inner) break;
                    if (!entry.is_regular_file()) continue;
                    const std::string name = entry.path().filename().string();
                    if (name.find(pattern) != std::string::npos &&
                        entry.path().extension() == ".gguf") {
                        return entry.path().string();
                    }
                }
            }
        }
    }
    return {};
}

bool separate_stem(const std::string& input_media, const SeparateOptions& options,
                   const std::string& out_wav16k, std::string& error) {
#if !IRIS_HAS_CRISPASR
    (void)input_media;
    (void)options;
    (void)out_wav16k;
    error = "separate: CrispASR 运行时 DLL 未就绪（tools/runtimes/crispasr）";
    return false;
#else
    namespace fs = std::filesystem;
    if (options.model.empty() || !fs::exists(options.model)) {
        error = "separate: 分离模型不存在：" + options.model;
        return false;
    }

    const fs::path work = fs::temp_directory_path() / "iris-separate";
    std::error_code ec;
    fs::create_directories(work, ec);
    const fs::path in44 = work / "mix_44k_stereo.wav";
    const fs::path stem44 = work / ("stem_" + options.stem + "_44k.wav");

    // 1) 统一成 HTDemucs 要的 44.1 kHz 立体声
    if (!run_ffmpeg({"ffmpeg", "-y", "-v", "error", "-i", input_media, "-ac", "2", "-ar",
                     std::to_string(kSeparationRate), "-c:a", "pcm_s16le", in44.string()},
                    error)) {
        return false;
    }

    std::vector<float> pcm;
    int rate = 0;
    int channels = 0;
    if (!read_wav_raw(in44.string(), pcm, rate, channels, error)) return false;
    if (channels != 2 || rate != kSeparationRate) {
        error = "separate: 输入转换结果不是 44.1 kHz 立体声";
        return false;
    }
    const int frames = static_cast<int>(pcm.size() / 2);

    char backend[96] = {};
    std::string backend_name = "htdemucs";
    if (crispasr_detect_backend_from_gguf(options.model.c_str(), backend,
                                          static_cast<int>(sizeof backend)) > 0 &&
        backend[0] != '\0') {
        backend_name = backend;
    }

    // 2) 分块并行：CrispASR 的分离后端是单线程的（htdemucs 内部 n_threads=0），
    //    所以把音频切成带重叠的块，每块一个独立会话跑在自己的线程里，再交叉淡入拼接。
    const int chunk_secs = std::max(4, options.chunk_seconds > 0 ? options.chunk_seconds
                                                                  : kChunkSeconds);
    const int chunk = chunk_secs * kSeparationRate;
    const int overlap = kOverlapSeconds * kSeparationRate;
    const int stride = chunk - overlap;

    std::vector<std::pair<int, int>> ranges; // [start, end)
    if (frames <= chunk) {
        ranges.emplace_back(0, frames);
    } else {
        for (int start = 0; start < frames; start += stride) {
            const int end = std::min(frames, start + chunk);
            ranges.emplace_back(start, end);
            if (end >= frames) break;
        }
    }

    const int threads = infra::resolve_threads(options.n_threads);
    const int workers = std::max(1, std::min<int>(threads, static_cast<int>(ranges.size())));

    std::vector<std::vector<float>> parts(ranges.size());
    std::string first_error;
    std::mutex err_mutex;

    auto run_worker = [&](int worker_id) {
        SepSession sep;
        std::string open_error;
        if (!open_session(options.model, backend_name, sep, open_error)) {
            std::lock_guard<std::mutex> lock(err_mutex);
            if (first_error.empty()) first_error = open_error;
            return;
        }
        for (std::size_t i = static_cast<std::size_t>(worker_id); i < ranges.size();
             i += static_cast<std::size_t>(workers)) {
            std::vector<float> chunk_pcm(
                pcm.begin() + static_cast<std::ptrdiff_t>(ranges[i].first) * 2,
                pcm.begin() + static_cast<std::ptrdiff_t>(ranges[i].second) * 2);
            std::vector<float> stem;
            std::string chunk_error;
            if (!separate_chunk(sep, chunk_pcm.data(), ranges[i].second - ranges[i].first,
                                options.stem, stem, chunk_error)) {
                std::lock_guard<std::mutex> lock(err_mutex);
                if (first_error.empty()) first_error = chunk_error;
                return;
            }
            parts[i] = std::move(stem);
        }
    };

    if (workers == 1) {
        run_worker(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(workers));
        for (int w = 0; w < workers; ++w) pool.emplace_back(run_worker, w);
        for (auto& t : pool) t.join();
    }
    if (!first_error.empty()) {
        error = first_error;
        return false;
    }

    // 3) 交叉淡入拼接（线性窗，重叠区权重和为 1）
    std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.0f);
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const int start = ranges[i].first;
        const int len = ranges[i].second - start;
        const std::vector<float>& part = parts[i];
        const int part_frames = static_cast<int>(part.size() / 2);
        const int n = std::min(len, part_frames);
        for (int t = 0; t < n; ++t) {
            float gain = 1.0f;
            if (i > 0 && t < overlap) {
                gain *= static_cast<float>(t) / static_cast<float>(overlap);
            }
            if (i + 1 < ranges.size() && t >= n - overlap) {
                gain *= static_cast<float>(n - t) / static_cast<float>(overlap);
            }
            const std::size_t dst = (static_cast<std::size_t>(start) + static_cast<std::size_t>(t)) * 2;
            out[dst] += part[static_cast<std::size_t>(t) * 2] * gain;
            out[dst + 1] += part[static_cast<std::size_t>(t) * 2 + 1] * gain;
        }
    }

    if (!write_wav_raw(stem44.string(), out, kSeparationRate, 2, error)) return false;

    // 4) 转成下游统一吃的 16 kHz 单声道
    if (!run_ffmpeg({"ffmpeg", "-y", "-v", "error", "-i", stem44.string(), "-ac", "1", "-ar",
                     "16000", "-c:a", "pcm_s16le", out_wav16k},
                    error)) {
        return false;
    }

    spdlog::info("separate: {} → {}（{} 块 / {} 线程，{}s 音频）", options.stem,
                 fs::path(out_wav16k).filename().string(), ranges.size(), workers,
                 static_cast<double>(frames) / kSeparationRate);
    return true;
#endif
}

} // namespace iris::asr
