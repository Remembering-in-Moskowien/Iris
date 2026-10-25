#include "iris/subtitle/ir_bridge.hpp"

#include <set>

namespace iris::subtitle {

model::Track track_from_script(const AssScript& script, const std::string& language) {
    model::Track track;
    track.type = "transcribe";
    track.language = language;
    track.sentences.reserve(script.events.size());
    for (const auto& ev : script.events) {
        model::Sentence s;
        s.start_ms = ev.start_ms;
        s.end_ms = ev.end_ms;
        s.text = strip_ass_text(ev.text);
        s.raw = ev.text;
        s.style = ev.style;
        s.layer = ev.layer;
        s.comment = ev.comment;
        if (!ev.actor.empty()) s.speaker = ev.actor;
        track.sentences.push_back(std::move(s));
    }
    return track;
}

void apply_presentation(const AssScript& script, model::WorkflowState& state) {
    state.styles = script.styles;
    state.script_info = script.info;
}

void merge_styles(const AssScript& first, const AssScript& second, model::WorkflowState& state) {
    state.styles = first.styles;
    state.script_info = first.info;
    std::set<std::string> have;
    for (const auto& s : state.styles) have.insert(s.name);
    for (const auto& s : second.styles) {
        if (have.insert(s.name).second) state.styles.push_back(s);
    }
}

} // namespace iris::subtitle
