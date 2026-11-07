#include "iris/ops/translate.hpp"

#include <string>
#include <utility>

#include "iris/infra/log.hpp"

namespace iris::ops {

namespace {

const model::Track* find_transcribe_track(const model::WorkflowState& st) {
    for (const auto& t : st.tracks) {
        if (t.type == "transcribe" && !t.sentences.empty()) return &t;
    }
    return nullptr;
}

} // namespace

bool Translate::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;

    const model::Track* source = find_transcribe_track(st);
    if (source == nullptr) {
        spdlog::error("translate: IR 里没有 transcribe 轨道（先跑 asr）");
        return false;
    }

    mt::Translator translator;
    llm::Scheduler scheduler(opts_.llm);
    std::string error;
    if (!translator.open(opts_.mt, error)) {
        spdlog::error("{}", error);
        return false;
    }

    model::Track out;
    out.type = "translation";
    out.language = mt::normalize_language(opts_.mt.target);
    out.sentences.reserve(source->sentences.size());

    int done = 0;
    const auto total = static_cast<int>(source->sentences.size());
    for (const auto& s : source->sentences) {
        if (s.text.empty()) continue;

        std::string translated;
        if (!translator.translate(s.text, translated, error)) {
            spdlog::error("translate: 第 {}/{} 句失败：{}", done + 1, total, error);
            return false;
        }
        translated = mt::normalize_output(translated, opts_.mt.target);
        if (scheduler.enabled()) {
            std::string refined;
            if (!scheduler.refine_translation(s.text, translated, opts_.mt.target, refined, error)) {
                spdlog::warn("translate: LLM 校对第 {}/{} 句失败，保留机器译文：{}",
                             done + 1, total, error);
            } else {
                translated = mt::normalize_output(refined, opts_.mt.target);
            }
        }
        model::Sentence t = s; // 保留时间码/说话人/词级信息
        t.translated_text = translated;
        out.sentences.push_back(std::move(t));

        ++done;
        spdlog::info("translate: [{}/{}] {}", done, total, translated);
    }

    if (out.sentences.empty()) {
        spdlog::error("translate: 没有可翻译的句子");
        return false;
    }

    st.tracks.push_back(std::move(out));
    ctx.record_provenance(name(), opts_.mt.model);
    spdlog::info("translate: {} 句 → {}（后端 {}）", done, st.tracks.back().language,
                 translator.backend());
    return true;
}

} // namespace iris::ops
