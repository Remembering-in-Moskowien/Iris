#pragma once

#include <functional>
#include <string>

#include "daisy/model/ir_document.hpp"

namespace daisy::core {

/// 管线执行上下文：当前 IR 文档、工作目录、进度回调。
/// 对应 C# 版 PipelineContext / 工作流上下文。
struct PipelineContext {
    model::IrDocument* doc = nullptr;
    std::string work_dir; ///< 本次运行专属临时目录
    std::function<void(const std::string& stage, float progress)> on_progress;

    /// 记录一条溯源（operator + model）。
    void record_provenance(const std::string& op,
                           const std::optional<std::string>& model = std::nullopt) {
        if (!doc) return;
        model::ProvenanceEntry entry;
        entry.op = op;
        entry.model = model;
        doc->provenance.push_back(std::move(entry));
    }
};

} // namespace daisy::core
