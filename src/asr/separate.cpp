#include "iris/asr/separate.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

} // namespace

std::string default_separation_model() {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (const char* env = std::getenv("IRIS_SEPARATION_MODEL"); env != nullptr && *env != '\0') {
        if (fs::exists(env, ec)) return env;
    }

    // 搜索顺序：~/.cache/crispasr → ./models/separation → ./models/*/
    std::vector<fs::path> dirs;
    if (const char* home = std::getenv("USERPROFILE"); home != nullptr && *home != '\0') {
        dirs.push_back(fs::path(home) / ".cache" / "crispasr");
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        dirs.push_back(fs::path(home) / ".cache" / "crispasr");
    }
    dirs.push_back(fs::path("models") / "separation");
    dirs.push_back(fs::path("models"));

    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name.find("htdemucs") != std::string::npos && entry.path().extension() == ".gguf") {
                return entry.path().string();
            }
        }
        // 再下探一层（models/<name>/*.gguf）
        for (const auto& sub : fs::directory_iterator(dir, ec)) {
            if (ec || !sub.is_directory()) continue;
            std::error_code inner;
            for (const auto& entry : fs::directory_iterator(sub.path(), inner)) {
                if (inner) break;
                if (!entry.is_regular_file()) continue;
                const std::string name = entry.path().filename().string();
                if (name.find("htdemucs") != std::string::npos &&
                    entry.path().extension() == ".gguf") {
                    return entry.path().string();
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

    // 2) 进程内分离
    char backend[96] = {};
    std::string backend_name = "htdemucs";
    if (crispasr_detect_backend_from_gguf(options.model.c_str(), backend,
                                          static_cast<int>(sizeof backend)) > 0 &&
        backend[0] != '\0') {
        backend_name = backend;
    }
    auto* session = crispasr_session_open_explicit(
        options.model.c_str(), backend_name.c_str(), infra::resolve_threads(options.n_threads));
    if (session == nullptr) {
        error = "separate: 无法加载模型 " + options.model + "（后端 " + backend_name + "）";
        return false;
    }

    const int n_stems = crispasr_session_separate(session, pcm.data(), frames);
    if (n_stems <= 0) {
        error = "separate: 分离失败（n_stems=" + std::to_string(n_stems) + "）";
        crispasr_session_close(session);
        return false;
    }

    int target = -1;
    std::string names;
    for (int i = 0; i < n_stems; ++i) {
        const char* name = crispasr_session_separate_stem_name(session, i);
        const std::string n = name != nullptr ? name : "";
        names += (i ? ", " : "") + n;
        if (n == options.stem) target = i;
    }
    if (target < 0) {
        error = "separate: 模型没有 stem '" + options.stem + "'（可用：" + names + "）";
        crispasr_session_close(session);
        return false;
    }

    int n_stem_samples = 0;
    const float* stem = crispasr_session_separate_stem(session, target, &n_stem_samples);
    if (stem == nullptr || n_stem_samples <= 0) {
        error = "separate: 取 stem 数据失败";
        crispasr_session_close(session);
        return false;
    }
    const int stem_rate = crispasr_session_separate_sample_rate(session);
    std::vector<float> stem_pcm(stem, stem + static_cast<std::size_t>(n_stem_samples) * 2);
    if (!write_wav_raw(stem44.string(), stem_pcm, stem_rate > 0 ? stem_rate : kSeparationRate, 2,
                       error)) {
        crispasr_session_close(session);
        return false;
    }
    crispasr_session_close(session);

    // 3) 转成下游统一吃的 16 kHz 单声道
    if (!run_ffmpeg({"ffmpeg", "-y", "-v", "error", "-i", stem44.string(), "-ac", "1", "-ar",
                     "16000", "-c:a", "pcm_s16le", out_wav16k},
                    error)) {
        return false;
    }

    spdlog::info("separate: {} → {}（{} 段 stem @ {} Hz）", options.stem,
                 fs::path(out_wav16k).filename().string(), n_stems, stem_rate);
    return true;
#endif
}

} // namespace iris::asr
