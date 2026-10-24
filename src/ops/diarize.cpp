#include "iris/ops/diarize.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "iris/infra/log.hpp"

namespace iris::ops {

namespace {

model::Track* find_track(model::WorkflowState& st, const std::string& type) {
    for (auto& t : st.tracks) {
        if (t.type == type && !t.sentences.empty()) return &t;
    }
    return nullptr;
}

} // namespace

bool Diarize::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;

    model::Track* source = find_track(st, "transcribe");
    if (source == nullptr) {
        // 只有译文轨道（IR 输入）时，也可以按时间标注
        source = find_track(st, "translation");
    }
    if (source == nullptr) {
        spdlog::error("diarize: IR 里没有可标注的句子（先跑 asr 或 translate）");
        return false;
    }

    const std::string* audio = st.preprocessed_audio_path
                                   ? &*st.preprocessed_audio_path
                                   : (st.converted_audio_path ? &*st.converted_audio_path : nullptr);
    if (audio == nullptr) {
        spdlog::error("diarize: 没有可用的 16 kHz 音频产物");
        return false;
    }

    int n_speakers = 0;
    std::string error;
    if (!asr::diarize_sentences(*audio, opts_, source->sentences, n_speakers, error)) {
        spdlog::error("{}", error);
        return false;
    }

    // 其它轨道（translation 等）按时间中点共享说话人标签，便于后续多音色配音。
    std::vector<asr::SpeakerSpan> spans;
    spans.reserve(source->sentences.size());
    for (const auto& s : source->sentences) {
        int id = -1;
        if (s.speaker && s.speaker->rfind("SPEAKER_", 0) == 0) {
            id = std::atoi(s.speaker->c_str() + 8);
        }
        spans.push_back({s.start_ms, s.end_ms, id});
    }
    for (auto& track : st.tracks) {
        if (&track == source) continue;
        for (auto& s : track.sentences) {
            const auto mid = (s.start_ms + s.end_ms) / 2;
            const int id = asr::speaker_at(mid, spans);
            if (id >= 0) s.speaker = asr::speaker_label(id);
        }
    }

    ctx.record_provenance(name(), opts_.method == 4 ? opts_.embedder : opts_.model);
    spdlog::info("diarize: {} 个说话人，已标注 {} 句", n_speakers, source->sentences.size());
    return true;
}

} // namespace iris::ops
