#include "daisy/ops/transcribe.hpp"

#include "daisy/infra/log.hpp"

namespace daisy::ops {

bool Transcribe::execute(core::PipelineContext& ctx) {
    if (!ctx.doc || !provider_) return false;
    auto& st = ctx.doc->state;

    // 优先使用预处理音频，退回到转换音频。
    const std::string* audio = st.preprocessed_audio_path
                                   ? &*st.preprocessed_audio_path
                                   : (st.converted_audio_path ? &*st.converted_audio_path : nullptr);
    if (!audio) {
        spdlog::error("transcribe: 没有可用的音频产物");
        return false;
    }

    std::vector<model::Sentence> sentences;
    std::string error;
    if (!provider_->transcribe(*audio, ctx.doc->config.language, sentences, error)) {
        spdlog::error("transcribe: {}", error);
        return false;
    }

    model::Track t;
    t.type = "transcribe";
    t.language = ctx.doc->config.language;
    t.sentences = std::move(sentences);
    st.tracks.push_back(std::move(t));
    st.is_transcribed = true;
    ctx.record_provenance(name(), provider_->name());
    spdlog::info("transcribe: {} 句", st.tracks.back().sentences.size());
    return true;
}

} // namespace daisy::ops
