#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/core/pipeline.hpp"
#include "iris/model/ir_document.hpp"

namespace iris::core {

enum class Facet {
    Media,
    Audio,
    Transcript,
    Words,
    Karaoke,
    Subtitle,
};

[[nodiscard]] const char* facet_name(Facet facet);
[[nodiscard]] bool parse_facet(const std::string& name, Facet& out);

struct PipelineNode {
    std::string id;
    std::string operation;
    std::vector<std::string> dependencies;
    nlohmann::json parameters = nlohmann::json::object();
};

struct PipelineManifest {
    int api_version = 1;
    std::vector<PipelineNode> nodes;

    [[nodiscard]] static bool load(const std::string& path, PipelineManifest& out,
                                   std::string& error);
    [[nodiscard]] static bool parse(const nlohmann::json& document,
                                    PipelineManifest& out, std::string& error);
    [[nodiscard]] bool validate(std::string& error) const;
    [[nodiscard]] bool build(Pipeline& pipeline, const model::WorkflowConfig& config,
                             std::string& error) const;
};

struct OperationInfo {
    std::string id;
    std::vector<Facet> consumes;
    std::vector<Facet> produces;
};

[[nodiscard]] std::vector<OperationInfo> registered_operations();

} // namespace iris::core
