#include "iris/tts/tts.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "iris/core/extension_registry.hpp"
#include "iris/infra/process.hpp"
#include "iris/infra/threads.hpp"
#include "iris/tts/qwen3_session.hpp"
#include "iris/tts/timestretch.hpp"
#include <spdlog/fmt/fmt.h>

namespace iris::tts {

namespace {

class TtsBackendSession {
public:
    virtual ~TtsBackendSession() = default;
    virtual bool open(const TtsRequest& request, std::string& error) = 0;
    virtual bool synthesize(const std::string& text, TtsResult& out,
                            std::string& error) = 0;
    [[nodiscard]] virtual bool is_open() const = 0;
};

class Qwen3BackendSession final : public TtsBackendSession {
public:
    bool open(const TtsRequest& request, std::string& error) override {
        return qwen3_open(request, session_, error);
    }

    bool synthesize(const std::string& text, TtsResult& out,
                    std::string& error) override {
        return qwen3_synthesize(session_, text, out, error);
    }

    [[nodiscard]] bool is_open() const override { return session_.session != nullptr; }

private:
    Qwen3Session session_;
};

class IndexTtsBackendSession final : public TtsBackendSession {
public:
    bool open(const TtsRequest& request, std::string&) override {
        request_ = request;
        return true;
    }

    bool synthesize(const std::string& text, TtsResult& out,
                    std::string& error) override {
        TtsRequest request = request_;
        request.text = text;
        return synth_indextts(request, out, error);
    }

    [[nodiscard]] bool is_open() const override { return false; }

private:
    TtsRequest request_;
};

std::unique_ptr<TtsBackendSession> make_qwen3_session() {
    return std::make_unique<Qwen3BackendSession>();
}

std::unique_ptr<TtsBackendSession> make_indextts_session() {
    return std::make_unique<IndexTtsBackendSession>();
}

struct TtsBackendRegistration {
    TtsEngine engine;
    bool available;
    bool (*synthesize)(const TtsRequest&, TtsResult&, std::string&);
    std::unique_ptr<TtsBackendSession> (*create_session)();
};

const core::ExtensionRegistry<TtsBackendRegistration>& tts_backends() {
    static const auto registry = [] {
        core::ExtensionRegistry<TtsBackendRegistration> result;
        const auto add = [&](std::string id, std::vector<std::string> aliases,
                             TtsEngine engine, bool available,
                             bool (*synthesize)(const TtsRequest&, TtsResult&,
                                                std::string&),
                             std::unique_ptr<TtsBackendSession> (*create_session)()) {
            std::string error;
            if (!result.register_extension(std::move(id), std::move(aliases),
                                           {engine, available, synthesize, create_session},
                                           error)) {
                throw std::logic_error("invalid built-in TTS registration: " + error);
            }
        };
        add("qwen3-tts", {"qwen3tts", "qwen3"}, TtsEngine::Qwen3Tts,
            IRIS_HAS_CRISPASR != 0, synth_qwen3_tts, make_qwen3_session);
        add("indextts", {"index-tts"}, TtsEngine::IndexTts, IRIS_HAS_ONNX != 0,
            synth_indextts, make_indextts_session);
        return result;
    }();
    return registry;
}

const core::ExtensionRegistry<TtsBackendRegistration>::Entry*
find_tts_backend(TtsEngine engine) {
    const auto& entries = tts_backends().entries();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const auto& entry) {
                                     return entry.extension.engine == engine;
                                 });
    return it == entries.end() ? nullptr : &*it;
}

// WAV 头是 little-endian，读取时按字节拼装，不依赖宿主字节序。
std::uint32_t le32(const char* p) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

std::uint16_t le16(const char* p) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(p[0]) |
                                      (static_cast<unsigned char>(p[1]) << 8));
}

void put32(std::ostream& os, std::uint32_t v) {
    char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    os.write(b, 4);
}

void put16(std::ostream& os, std::uint16_t v) {
    char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    os.write(b, 2);
}

} // namespace

// ---------------------------------------------------------------------------
// 引擎注册
// ---------------------------------------------------------------------------
bool parse_tts_engine(const std::string& name, TtsEngine& out) {
    const auto* entry = tts_backends().find(name);
    if (!entry) return false;
    out = entry->extension.engine;
    return true;
}

const char* tts_engine_name(TtsEngine engine) {
    const auto* entry = find_tts_backend(engine);
    return entry ? entry->id.c_str() : "unknown";
}

bool tts_engine_available(TtsEngine engine) {
    const auto* entry = find_tts_backend(engine);
    return entry != nullptr && entry->extension.available;
}

std::vector<TtsEngineInfo> registered_tts_engines() {
    std::vector<TtsEngineInfo> out;
    for (const auto& entry : tts_backends().entries()) {
        out.push_back({entry.id, entry.aliases, entry.extension.engine,
                       entry.extension.available});
    }
    return out;
}

// 供各引擎实现复用的线程数解析。
int resolve_tts_threads(int requested) { return infra::resolve_threads(requested); }

// ---------------------------------------------------------------------------
// 复用型会话
// ---------------------------------------------------------------------------
struct TtsSession::Impl {
    TtsRequest req;
    std::unique_ptr<TtsBackendSession> backend;
};

TtsSession::TtsSession() : impl_(std::make_unique<Impl>()) {}
TtsSession::~TtsSession() = default;

bool TtsSession::open(const TtsRequest& req, std::string& error) {
    impl_->req = req;
    impl_->backend.reset();

    if (!tts_engine_available(req.engine)) {
        error = std::string("tts: 引擎 '") + tts_engine_name(req.engine) +
                "' 的推理运行时未随本次构建提供";
        return false;
    }
    const auto* entry = find_tts_backend(req.engine);
    if (!entry) {
        error = "tts: 未知引擎";
        return false;
    }
    impl_->backend = entry->extension.create_session();
    if (!impl_->backend) {
        error = std::string("tts: 无法创建引擎会话 '") + entry->id + "'";
        return false;
    }
    if (!impl_->backend->open(req, error)) {
        impl_->backend.reset();
        return false;
    }
    return true;
}

bool TtsSession::synthesize(const std::string& text, TtsResult& out, std::string& error) {
    out = TtsResult{};
    if (text.empty()) {
        error = "tts: 待合成文本为空";
        return false;
    }
    if (impl_->backend) return impl_->backend->synthesize(text, out, error);

    // 未打开会话时仍使用一次性引擎入口。
    TtsRequest req = impl_->req;
    req.text = text;
    return iris::tts::synthesize(req, out, error);
}

bool TtsSession::is_open() const {
    return impl_ != nullptr && impl_->backend != nullptr && impl_->backend->is_open();
}

const TtsRequest& TtsSession::request() const { return impl_->req; }

// ---------------------------------------------------------------------------
// 分发
// ---------------------------------------------------------------------------
bool synthesize(const TtsRequest& req, TtsResult& out, std::string& error) {
    out = TtsResult{};
    if (req.text.empty()) {
        error = "tts: 待合成文本为空";
        return false;
    }
    const auto* entry = find_tts_backend(req.engine);
    if (!entry) {
        error = "tts: 未知引擎";
        return false;
    }
    return entry->extension.synthesize(req, out, error);
}

// ---------------------------------------------------------------------------
// WAV I/O
// ---------------------------------------------------------------------------
bool read_wav(const std::string& path, std::vector<float>& pcm, int& sample_rate,
              std::string& error) {
    pcm.clear();
    sample_rate = 0;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开 WAV：" + path;
        return false;
    }
    auto fail = [&](const char* why) {
        error = std::string(why) + "：" + path;
        return false;
    };

    char hdr[12];
    if (!f.read(hdr, 12)) return fail("WAV 头不完整");
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
        return fail("不是 RIFF/WAVE 文件");

    int channels = 0;
    int bits = 0;
    bool have_fmt = false;
    std::vector<char> data;

    while (f) {
        char cid[4];
        char szb[4];
        if (!f.read(cid, 4) || !f.read(szb, 4)) break;
        const std::uint32_t sz = le32(szb);
        if (std::memcmp(cid, "fmt ", 4) == 0) {
            char fmt[16] = {};
            const std::uint32_t take = std::min<std::uint32_t>(sz, sizeof fmt);
            if (!f.read(fmt, take)) return fail("fmt 块损坏");
            if (take >= 16) {
                channels = le16(fmt + 2);
                sample_rate = static_cast<int>(le32(fmt + 4));
                bits = le16(fmt + 14);
                have_fmt = true;
            }
            if (sz > take) f.seekg(sz - take, std::ios::cur);
        } else if (std::memcmp(cid, "data", 4) == 0) {
            data.resize(sz);
            if (sz > 0 && !f.read(data.data(), static_cast<std::streamsize>(sz)))
                return fail("data 块损坏");
            break;
        } else {
            f.seekg(sz, std::ios::cur);
        }
        if (sz & 1) f.seekg(1, std::ios::cur); // RIFF 块按偶数对齐
    }

    if (!have_fmt || channels <= 0 || sample_rate <= 0) return fail("缺少有效的 fmt 块");
    if (bits != 16) return fail("仅支持 16-bit PCM WAV");
    if (data.empty()) return fail("没有音频数据");

    const std::size_t frames = data.size() / (2 * static_cast<std::size_t>(channels));
    pcm.resize(frames);
    const auto* s16 = reinterpret_cast<const std::int16_t*>(data.data());
    for (std::size_t i = 0; i < frames; ++i) {
        pcm[i] = static_cast<float>(s16[i * static_cast<std::size_t>(channels)]) / 32768.0f;
    }
    return true;
}

bool write_wav(const std::string& path, const std::vector<float>& pcm, int sample_rate,
               std::string& error) {
    if (sample_rate <= 0) {
        error = "tts: 采样率无效";
        return false;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        error = "无法写入 WAV：" + path;
        return false;
    }

    const std::uint32_t data_size = static_cast<std::uint32_t>(pcm.size() * 2);
    f.write("RIFF", 4);
    put32(f, 36 + data_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(f, 16);
    put16(f, 1); // PCM
    put16(f, 1); // 单声道
    put32(f, static_cast<std::uint32_t>(sample_rate));
    put32(f, static_cast<std::uint32_t>(sample_rate) * 2); // byte rate
    put16(f, 2);                                           // block align
    put16(f, 16);                                          // bits
    f.write("data", 4);
    put32(f, data_size);

    std::vector<std::int16_t> s16(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const float v = std::clamp(pcm[i], -1.0f, 1.0f);
        s16[i] = static_cast<std::int16_t>(v * 32767.0f);
    }
    if (!s16.empty()) {
        f.write(reinterpret_cast<const char*>(s16.data()),
                static_cast<std::streamsize>(s16.size() * sizeof(std::int16_t)));
    }
    f.flush();
    if (!f) {
        error = "写入 WAV 失败：" + path;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 参考音频采样率归一
// ---------------------------------------------------------------------------
bool ensure_reference_rate(const std::string& ref_wav, int target_rate, std::string& out_path,
                           std::string& error) {
    namespace fs = std::filesystem;

    out_path.clear();
    std::vector<float> probe;
    int rate = 0;
    if (!read_wav(ref_wav, probe, rate, error)) return false;
    if (rate == target_rate) {
        out_path = ref_wav;
        return true;
    }

    // 媒体转码是既定的外部依赖（ffmpeg），此处复用它做重采样而不是自写滤波器。
    const fs::path out = fs::temp_directory_path() /
                         ("iris-ref-" + std::to_string(target_rate) + "-" +
                          fs::path(ref_wav).stem().string() + ".wav");
    const auto result = infra::run_process({"ffmpeg", "-y", "-v", "error", "-i", ref_wav,
                                            "-ac", "1", "-ar", std::to_string(target_rate),
                                            "-c:a", "pcm_s16le", out.string()});
    if (!result.ok() || !result.value().success()) {
        error = fmt::format("参考音频重采样失败（{} Hz -> {} Hz，需要 ffmpeg）：{}", rate,
                            target_rate, ref_wav);
        return false;
    }
    out_path = out.string();
    return true;
}

// ---------------------------------------------------------------------------
// 时间轴混音
// ---------------------------------------------------------------------------
void mix_at(std::vector<float>& dst, const std::vector<float>& src, std::size_t offset_samples) {
    if (src.empty()) return;
    if (dst.size() < offset_samples + src.size()) dst.resize(offset_samples + src.size(), 0.0f);
    // 纯相加（不做限幅）：重叠段可能超过 1.0，由调用方统一做峰值归一，
    // 这样既保住了相对音量，也不会在多句交叠处硬削波。
    for (std::size_t i = 0; i < src.size(); ++i) dst[offset_samples + i] += src[i];
}

float peak_abs(const std::vector<float>& pcm) {
    float peak = 0.0f;
    for (float v : pcm) peak = std::max(peak, std::abs(v));
    return peak;
}

// ---------------------------------------------------------------------------
// 时长对齐
// ---------------------------------------------------------------------------
void trim_silence(std::vector<float>& pcm, int sample_rate, float threshold, int keep_ms) {
    if (pcm.empty() || sample_rate <= 0) return;

    const std::size_t frame = std::max<std::size_t>(1, static_cast<std::size_t>(sample_rate) / 100);
    const std::size_t frames = (pcm.size() + frame - 1) / frame;

    auto frame_is_silent = [&](std::size_t i) {
        const std::size_t begin = i * frame;
        const std::size_t end = std::min(pcm.size(), begin + frame);
        float peak = 0.0f;
        for (std::size_t k = begin; k < end; ++k) peak = std::max(peak, std::abs(pcm[k]));
        return peak < threshold;
    };

    std::size_t first = 0;
    while (first < frames && frame_is_silent(first)) ++first;
    if (first == frames) { // 整段都是静音：直接清空，交给上层判断
        pcm.clear();
        return;
    }
    std::size_t last = frames - 1;
    while (last > first && frame_is_silent(last)) --last;

    const auto keep = static_cast<std::size_t>(std::max(0, keep_ms)) * sample_rate / 1000;
    const std::size_t begin = first * frame > keep ? first * frame - keep : 0;
    const std::size_t end = std::min(pcm.size(), (last + 1) * frame + keep);
    pcm = std::vector<float>(pcm.begin() + static_cast<std::ptrdiff_t>(begin),
                             pcm.begin() + static_cast<std::ptrdiff_t>(end));
}

bool time_compress(const std::vector<float>& in, int sample_rate, float speed,
                   std::vector<float>& out, std::string& error) {
    // 变速完全由自研 WSOLA 完成（见 timestretch.cpp），不再调用 ffmpeg：
    // 无外部进程、参数可调，且短句不会被拉成噪声。
    return wsola_stretch(in, sample_rate, speed, out, error);
}

} // namespace iris::tts
