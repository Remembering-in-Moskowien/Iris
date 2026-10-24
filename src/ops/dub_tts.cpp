#include "iris/ops/dub_tts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"

namespace iris::ops {

namespace {

constexpr const char* kDubFileName = "dub.wav";
constexpr std::int64_t kMaxCueMs = 60'000; ///< 单句时间码上限（防脏数据撑爆时间轴）

/// 选出要配音的轨道：translation 优先（有译文才配音），否则 transcribe。
const model::Track* pick_track(const model::WorkflowState& st) {
    const model::Track* fallback = nullptr;
    for (const auto& t : st.tracks) {
        if (t.sentences.empty()) continue;
        if (t.type == "translation") return &t;
        if (!fallback) fallback = &t;
    }
    return fallback;
}

/// 一句的配音文本：有译文用译文，否则用原文。
std::string cue_text(const model::Sentence& s) {
    if (s.translated_text && !s.translated_text->empty()) return *s.translated_text;
    return s.text;
}

/// 待配音的一句：时间码 + 文本。
struct Cue {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text;
};

std::vector<Cue> collect_cues(const model::Track& track) {
    std::vector<Cue> cues;
    for (const auto& s : track.sentences) {
        std::string text = cue_text(s);
        if (text.empty()) continue;
        cues.push_back({std::clamp<std::int64_t>(s.start_ms, 0, kMaxCueMs),
                        std::clamp<std::int64_t>(s.end_ms, 0, kMaxCueMs), std::move(text)});
    }
    // 时间码必须单调递增，否则后面「下一句起点」会算成负数。
    for (std::size_t i = 1; i < cues.size(); ++i) {
        if (cues[i].start_ms < cues[i - 1].start_ms) cues[i].start_ms = cues[i - 1].start_ms;
    }
    return cues;
}

double duration_s(std::size_t samples, int sample_rate) {
    return sample_rate > 0 ? static_cast<double>(samples) / sample_rate : 0.0;
}

} // namespace

bool DubTts::execute(core::PipelineContext& ctx) {
    namespace fs = std::filesystem;

    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;

    const model::Track* track = pick_track(st);
    if (!track) {
        spdlog::error("dub_tts: IR 里没有可配音的句子（先跑 asr 或用 --ir 提供 IR）");
        return false;
    }
    const std::vector<Cue> cues = collect_cues(*track);
    if (cues.empty()) {
        spdlog::error("dub_tts: 轨道里没有可配音的文本");
        return false;
    }

    const fs::path out =
        opts_.output_path.empty() ? fs::path(ctx.work_dir) / kDubFileName : fs::path(opts_.output_path);

    std::vector<float> timeline;
    int sample_rate = 0;
    int done = 0;
    const auto total = static_cast<int>(cues.size());
    std::int64_t prev_end_ms = -1; ///< 上一句的结束时间码（用于保证句间停顿）
    std::int64_t drift_ms = 0;     ///< 因放不进时隙而顺延的累计毫秒

    for (std::size_t i = 0; i < cues.size(); ++i) {
        const Cue& cue = cues[i];

        tts::TtsRequest req = opts_.request;
        req.text = cue.text;

        tts::TtsResult result;
        std::string error;
        if (!tts::synthesize(req, result, error)) {
            spdlog::error("dub_tts: 第 {}/{} 句合成失败：{}", i + 1, total, error);
            return false;
        }
        if (result.sample_rate != sample_rate) {
            if (sample_rate != 0 && !timeline.empty()) {
                spdlog::error("dub_tts: 引擎采样率在同一批次内变化（{} -> {} Hz）", sample_rate,
                              result.sample_rate);
                return false;
            }
            sample_rate = result.sample_rate;
        }

        // 引擎输出常带首尾静音，先裁掉再量长度——否则会凭空「变长」。
        tts::trim_silence(result.pcm, sample_rate);
        if (result.pcm.empty()) {
            spdlog::warn("dub_tts: 第 {} 句合成结果为静音，跳过", i + 1);
            continue;
        }

        const double natural_s = duration_s(result.pcm.size(), sample_rate);
        double final_s = natural_s;

        // 落位：优先原句起点；若前一句还没说完，就顺延到「前一句结束 + 停顿」，
        // 这样任何情况下两句之间都有 gap_ms 的静音。
        std::int64_t place_ms = cue.start_ms;
        if (prev_end_ms >= 0) {
            place_ms = std::max(place_ms, prev_end_ms + std::max<std::int64_t>(0, opts_.gap_ms));
        }
        if (place_ms > cue.start_ms) {
            drift_ms += place_ms - cue.start_ms;
            spdlog::info("dub_tts: [{}/{}] 顺延 {}ms（累计 {}ms）", i + 1, total,
                         place_ms - cue.start_ms, drift_ms);
        }

        // 时长对齐：把多出来的部分变速压掉（at most max_speed），减少「被拉长」的感觉；
        // 压到上限仍放不进时隙的那一点点，交给下一句顺延，不截断语音。
        if (opts_.fit_timing) {
            const std::int64_t slot_end =
                (i + 1 < cues.size()) ? cues[i + 1].start_ms : cue.end_ms;
            const std::int64_t budget_ms =
                slot_end - place_ms - std::max<std::int64_t>(0, opts_.gap_ms);
            const double budget_s = static_cast<double>(budget_ms) / 1000.0;

            if (budget_ms > 0 && final_s > budget_s) {
                const float wanted = static_cast<float>(final_s / budget_s);
                const float applied = std::min(wanted, std::max(1.0f, opts_.max_speed));

                std::vector<float> squeezed;
                std::string squeeze_error;
                if (tts::time_compress(result.pcm, sample_rate, applied, squeezed, squeeze_error)) {
                    result.pcm = std::move(squeezed);
                    final_s = duration_s(result.pcm.size(), sample_rate);
                } else {
                    spdlog::warn("dub_tts: 第 {} 句变速失败（{}），保持原速", i + 1, squeeze_error);
                }
                spdlog::info("dub_tts: [{}/{}] 时长对齐 {:.2f}s → {:.2f}s（{:.2f}x，时隙 {:.2f}s）",
                             i + 1, total, natural_s, final_s, applied, budget_s);
            }
        }

        const auto offset = static_cast<std::size_t>(static_cast<std::int64_t>(place_ms) *
                                                     sample_rate / 1000);
        tts::mix_at(timeline, result.pcm, offset);
        prev_end_ms = place_ms + static_cast<std::int64_t>(std::llround(final_s * 1000.0));

        ++done;
        spdlog::info("dub_tts: [{}/{}] @{}ms {} 字符 → {:.2f}s", i + 1, total, place_ms,
                     cue.text.size(), final_s);
    }

    if (timeline.empty() || sample_rate <= 0) {
        spdlog::error("dub_tts: 没有合成出任何音频");
        return false;
    }

    // 句子交叠时会相加超过 0 dBFS；这里整体缩放到 -0.1 dBFS 以内，
    // 保住句间相对音量，同时避免 16-bit 写出时硬削波。
    constexpr float kPeakCeiling = 0.988f; // ≈ -0.1 dBFS
    const float peak = tts::peak_abs(timeline);
    if (peak > kPeakCeiling) {
        const float gain = kPeakCeiling / peak;
        for (float& v : timeline) v *= gain;
        spdlog::info("dub_tts: 峰值 {:.2f} 超限，整体归一 ×{:.3f}", peak, gain);
    }

    std::string error;
    if (!tts::write_wav(out.string(), timeline, sample_rate, error)) {
        spdlog::error("dub_tts: {}", error);
        return false;
    }

    st.dubbed_audio_path = out.string();
    st.is_dubbed = true;
    ctx.record_provenance(name(), opts_.request.model);
    spdlog::info("dub_tts: {} 句 → {}（{:.2f}s @ {} Hz）", done, out.string(),
                 duration_s(timeline.size(), sample_rate), sample_rate);
    return true;
}

} // namespace iris::ops
