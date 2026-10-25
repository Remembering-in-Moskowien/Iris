#pragma once

#include <string>

#include "iris/model/ir_document.hpp"
#include "iris/subtitle/ass_script.hpp"

namespace iris::subtitle {

/// ASS 事件 → IR 轨道（保留样式名、原始标签、层、Comment 标记、actor）。
model::Track track_from_script(const AssScript& script, const std::string& language);

/// 把 Script Info 与样式表写进 IR 的成品字段（convert/combine 用）。
void apply_presentation(const AssScript& script, model::WorkflowState& state);

/// 合并两份样式表（同名以 first 为准），用于双语合并时保留两个来源的样式。
void merge_styles(const AssScript& first, const AssScript& second, model::WorkflowState& state);

} // namespace iris::subtitle
