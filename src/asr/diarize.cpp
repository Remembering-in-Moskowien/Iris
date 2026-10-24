#include "iris/asr/diarize.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <thread>
#include <utility>

#include "iris/asr/ct2_mel.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/threads.hpp"

#if IRIS_HAS_CRISPASR
#include "crispasr.h"
#include "crispasr_session.h"

// CrispASR 的 diarize ABI 在公开头里是不透明声明，这里按 64 位布局补齐
// （与 src/crispasr_c_api.cpp 的 static_assert 一致：seg / turn 24 B、opts 48 B）。
struct crispasr_diarize_seg_abi {
    std::int64_t t0_cs;
    std::int64_t t1_cs;
    std::int32_t speaker;
    std::int32_t pad;
};

struct crispasr_diarize_turn_abi {
    std::int64_t t0_cs;
    std::int64_t t1_cs;
    std::int32_t speaker;
    std::int32_t pad;
};

struct crispasr_diarize_opts_abi {
    std::int32_t method;
    std::int32_t n_threads;
    std::int64_t slice_t0_cs;
    const char* pyannote_model_path;   // method 3 / 5 复用
    const char* foxnose_embedder_path; // method 4
    std::int32_t min_speakers;
    std::int32_t max_speakers;
    std::int32_t num_speakers;
    std::int32_t pad2;
};
#endif

namespace iris::asr {

#if IRIS_HAS_CRISPASR
static_assert(sizeof(crispasr_diarize_seg_abi) == 24, "diarize seg ABI 布局变了");
static_assert(sizeof(crispasr_diarize_turn_abi) == 24, "diarize turn ABI 布局变了");
static_assert(sizeof(crispasr_diarize_opts_abi) == 48, "diarize opts ABI 布局变了");
#endif

std::string speaker_label(int id) {
    return id < 0 ? std::string("SPEAKER_?") : "SPEAKER_" + std::to_string(id);
}

int speaker_at(std::int64_t t_ms, const std::vector<SpeakerSpan>& spans) {
    for (const auto& s : spans) {
        if (t_ms >= s.t0_ms && t_ms < s.t1_ms) return s.speaker;
    }
    // 落在空隙里就取最近的一段（同一说话人通常连着说）
    int best = -1;
    std::int64_t best_dist = 0;
    for (const auto& s : spans) {
        if (s.speaker < 0) continue;
        const std::int64_t dist = t_ms < s.t0_ms ? s.t0_ms - t_ms : t_ms - s.t1_ms;
        if (best < 0 || dist < best_dist) {
            best = s.speaker;
            best_dist = dist;
        }
    }
    return best;
}

std::string default_embedder_model() {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path base;
    if (const char* home = std::getenv("USERPROFILE"); home != nullptr && *home != '\0') {
        base = fs::path(home) / ".cache" / "crispasr";
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        base = fs::path(home) / ".cache" / "crispasr";
    }
    if (base.empty() || !fs::is_directory(base, ec)) return {};

    // 优先 WeSpeaker（method 4 的推荐嵌入模型），其次 TitaNet
    for (const char* pattern : {"wespeaker", "titanet"}) {
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name.find(pattern) != std::string::npos && entry.path().extension() == ".gguf") {
                return entry.path().string();
            }
        }
    }
    return {};
}

bool diarize_sentences(const std::string& wav16k_path, const DiarizeOptions& options,
                       std::vector<model::Sentence>& sentences, int& out_n_speakers,
                       std::string& error) {
    out_n_speakers = 0;
    if (sentences.empty()) {
        error = "diarize: 没有可标注的句子";
        return false;
    }
#if !IRIS_HAS_CRISPASR
    (void)wav16k_path;
    (void)options;
    error = "diarize: CrispASR 运行时 DLL 未就绪（tools/runtimes/crispasr）";
    return false;
#else
    if (options.method == 4 && options.embedder.empty()) {
        error = "diarize: method 4 需要说话人嵌入模型（--speaker-model，WeSpeaker GGUF）";
        return false;
    }
    if ((options.method == 3 || options.method == 5) && options.model.empty()) {
        error = "diarize: method 3/5 需要模型 GGUF（--speaker-model）";
        return false;
    }

    std::vector<float> pcm;
    std::string read_error;
    if (!read_wav_16k_mono(wav16k_path, pcm, read_error)) {
        error = "diarize: " + read_error;
        return false;
    }
    if (pcm.empty()) {
        error = "diarize: 音频为空";
        return false;
    }

    const std::int64_t audio_ms =
        static_cast<std::int64_t>(static_cast<double>(pcm.size()) / 16.0); // 16 kHz → ms
    if (audio_ms <= 0) {
        error = "diarize: 音频太短";
        return false;
    }

    // 1) 细网格：把整段音频切成 grid_ms 的小片交给库（库需要调用方的片段网格）。
    //    纯静音的小片跳过 —— 否则静音会被聚成"另一个说话人"。
    const std::int64_t grid = std::max<std::int64_t>(200, options.grid_ms);
    constexpr float kSilencePeak = 0.005f; // ≈ -46 dBFS
    std::vector<std::pair<std::int64_t, std::int64_t>> grid_spans;
    int silent_slices = 0;
    for (std::int64_t t = 0; t < audio_ms; t += grid) {
        const std::int64_t t1 = std::min(audio_ms, t + grid);
        if (t1 - t < 200) continue;

        const auto s0 = static_cast<std::size_t>(t * 16);
        const auto s1 = std::min(pcm.size(), static_cast<std::size_t>(t1 * 16));
        float peak = 0.0f;
        for (std::size_t k = s0; k < s1; ++k) peak = std::max(peak, std::abs(pcm[k]));
        if (peak < kSilencePeak) {
            ++silent_slices;
            continue;
        }
        grid_spans.emplace_back(t, t1);
    }
    if (grid_spans.empty()) {
        error = "diarize: 整段音频都是静音（没有可分割的语音）";
        return false;
    }

    std::vector<crispasr_diarize_seg_abi> segs;
    segs.reserve(grid_spans.size());
    for (const auto& [t0, t1] : grid_spans) {
        crispasr_diarize_seg_abi seg{};
        seg.t0_cs = t0 / 10;
        seg.t1_cs = t1 / 10;
        if (seg.t1_cs <= seg.t0_cs) seg.t1_cs = seg.t0_cs + 1;
        seg.speaker = -1;
        segs.push_back(seg);
    }

    crispasr_diarize_opts_abi opts{};
    opts.method = options.method;
    opts.n_threads = infra::resolve_threads(options.n_threads);
    opts.slice_t0_cs = 0;
    opts.pyannote_model_path = (options.method == 3 || options.method == 5) && !options.model.empty()
                                   ? options.model.c_str()
                                   : nullptr;
    opts.foxnose_embedder_path =
        options.method == 4 && !options.embedder.empty() ? options.embedder.c_str() : nullptr;
    opts.min_speakers = options.min_speakers;
    opts.max_speakers = options.max_speakers;
    opts.num_speakers = options.num_speakers;

    // 2) 先问 turns 数量，再按容量取回（ABI 无状态，需要两次调用）。
    std::int32_t n_turns = 0;
    const int rc_query = crispasr_diarize_segments_turns_abi(
        pcm.data(), nullptr, static_cast<std::int32_t>(pcm.size()), /*is_stereo=*/0, segs.data(),
        static_cast<std::int32_t>(segs.size()), &opts, nullptr, 0, &n_turns);
    std::vector<crispasr_diarize_turn_abi> lib_turns;
    int rc = rc_query;
    if (rc_query == 0 && n_turns > 0) {
        lib_turns.resize(static_cast<std::size_t>(n_turns));
        rc = crispasr_diarize_segments_turns_abi(
            pcm.data(), nullptr, static_cast<std::int32_t>(pcm.size()), /*is_stereo=*/0, segs.data(),
            static_cast<std::int32_t>(segs.size()), &opts, lib_turns.data(), n_turns, &n_turns);
        lib_turns.resize(static_cast<std::size_t>(std::max(0, n_turns)));
    }
    if (rc != 0) {
        error = "diarize: 分割失败（rc=" + std::to_string(rc) + "，method=" +
                std::to_string(options.method) + "）";
        return false;
    }

    // 3) 优先用库推导的 turns；库不产出 turns 的方法（0..3）退回「网格 + 同标签合并」。
    std::vector<SpeakerSpan> turns;
    std::string source_desc;
    if (!lib_turns.empty()) {
        for (const auto& t : lib_turns) {
            turns.push_back({static_cast<std::int64_t>(t.t0_cs) * 10,
                             static_cast<std::int64_t>(t.t1_cs) * 10, t.speaker});
        }
        source_desc = "库推导 turns";
    } else {
        for (std::size_t i = 0; i < segs.size(); ++i) {
            const int spk = segs[i].speaker;
            if (!turns.empty() && turns.back().speaker == spk) {
                turns.back().t1_ms = grid_spans[i].second;
            } else {
                turns.push_back({grid_spans[i].first, grid_spans[i].second, spk});
            }
        }
        source_desc = "网格合并";
    }

    // 4) 静音过滤：某段里大部分是静音时不占说话人编号（交界处的空白常被聚成一类）。
    auto voiced_ratio = [&](const SpeakerSpan& t) {
        const auto s0 = static_cast<std::size_t>(std::max<std::int64_t>(0, t.t0_ms) * 16);
        const auto s1 = std::min(pcm.size(), static_cast<std::size_t>(t.t1_ms * 16));
        if (s1 <= s0) return 0.0;
        const std::size_t step = 160; // 10 ms 一窗
        std::size_t windows = 0, voiced = 0;
        for (std::size_t k = s0; k < s1; k += step) {
            const std::size_t end = std::min(s1, k + step);
            float peak = 0.0f;
            for (std::size_t j = k; j < end; ++j) peak = std::max(peak, std::abs(pcm[j]));
            ++windows;
            if (peak >= kSilencePeak) ++voiced;
        }
        return windows == 0 ? 0.0 : static_cast<double>(voiced) / static_cast<double>(windows);
    };
    for (auto& t : turns) {
        if (t.speaker >= 0 && voiced_ratio(t) < 0.4) t.speaker = -1;
    }
    turns.erase(std::remove_if(turns.begin(), turns.end(),
                               [](const SpeakerSpan& t) { return t.speaker < 0; }),
                turns.end());
    if (turns.empty()) {
        error = "diarize: 没有检测到语音段（全是静音）";
        return false;
    }

    // 5) 归并过短片段（边界片常被单独聚成一类），再合并相邻同说话人的片段。
    constexpr std::int64_t kMinTurnMs = 500;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        if (turns[i].t1_ms - turns[i].t0_ms >= kMinTurnMs || turns[i].speaker < 0) continue;
        if (i > 0) {
            turns[i].speaker = turns[i - 1].speaker;
        } else if (i + 1 < turns.size()) {
            turns[i].speaker = turns[i + 1].speaker;
        }
    }
    {
        std::vector<SpeakerSpan> compacted;
        for (const auto& t : turns) {
            if (!compacted.empty() && compacted.back().speaker == t.speaker) {
                compacted.back().t1_ms = std::max(compacted.back().t1_ms, t.t1_ms);
            } else {
                compacted.push_back(t);
            }
        }
        turns = std::move(compacted);
    }

    // 5) 聚类 id 是任意的；按「首次出现」重编号成 0..N-1。
    std::map<int, int> remap;
    for (const auto& turn : turns) {
        if (turn.speaker >= 0 && remap.find(turn.speaker) == remap.end()) {
            remap.emplace(turn.speaker, static_cast<int>(remap.size()));
        }
    }
    for (auto& turn : turns) {
        const auto it = remap.find(turn.speaker);
        turn.speaker = it == remap.end() ? -1 : it->second;
    }

    // 6) 每句按「与说话人片段的最大重叠」定标签；无重叠则用时间中点。
    std::map<int, int> counts;
    for (auto& s : sentences) {
        std::map<int, std::int64_t> overlap;
        for (const auto& turn : turns) {
            if (turn.speaker < 0) continue;
            const auto lo = std::max(s.start_ms, turn.t0_ms);
            const auto hi = std::min(s.end_ms, turn.t1_ms);
            if (hi > lo) overlap[turn.speaker] += hi - lo;
        }
        int best = -1;
        std::int64_t best_overlap = 0;
        for (const auto& [spk, ms] : overlap) {
            if (ms > best_overlap) {
                best = spk;
                best_overlap = ms;
            }
        }
        if (best < 0) best = speaker_at((s.start_ms + s.end_ms) / 2, turns);

        s.speaker = speaker_label(best);
        if (best >= 0) ++counts[best];

        // 词级：按词中点找（Word.start/end 是绝对秒）
        for (auto& w : s.words) {
            const auto mid = static_cast<std::int64_t>((w.start + w.end) * 500.0);
            w.speaker = speaker_label(speaker_at(mid, turns));
        }
    }

    out_n_speakers = static_cast<int>(counts.size());

    std::string turn_desc;
    for (const auto& t : turns) {
        turn_desc += "[" + std::to_string(t.t0_ms) + "-" + std::to_string(t.t1_ms) + "ms S" +
                     std::to_string(t.speaker) + "] ";
    }
    spdlog::info("diarize: {}（{} 片，跳过 {} 段静音）→ {} 人 / {} 段：{}", source_desc,
                 segs.size(), silent_slices, out_n_speakers, turns.size(), turn_desc);
    return true;
#endif
}

} // namespace iris::asr
