#include "iris/core/pipeline_manifest.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "iris/asr/asr.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/paths.hpp"
#include "iris/ops/audio_preprocess.hpp"
#include "iris/ops/ffmpeg_convert.hpp"
#include "iris/ops/sentence_split.hpp"
#include "iris/ops/transcribe.hpp"
#include "iris/quality/ass_lint.hpp"
#include "iris/view/ass_builder.hpp"
#include "iris/view/subtitle_renderer.hpp"

namespace iris::core {
namespace {

struct OperationContract {
    std::vector<Facet> consumes;
    std::vector<Facet> produces;
    std::vector<std::string> parameters;
    std::shared_ptr<Operator> (*create)(const nlohmann::json&,
                                         const model::WorkflowConfig&,
                                         std::string&);
};

class ManifestOperator final : public Operator {
public:
    ManifestOperator(std::string id, std::vector<std::string> dependencies,
                     std::shared_ptr<Operator> implementation)
        : id_(std::move(id)), dependencies_(std::move(dependencies)),
          implementation_(std::move(implementation)) {}

    [[nodiscard]] std::string name() const override { return id_; }
    [[nodiscard]] std::vector<std::string> dependencies() const override {
        return dependencies_;
    }
    bool execute(PipelineContext& context) override {
        return implementation_->execute(context);
    }

private:
    std::string id_;
    std::vector<std::string> dependencies_;
    std::shared_ptr<Operator> implementation_;
};

std::shared_ptr<Operator> create_ffmpeg_convert(const nlohmann::json&,
                                                const model::WorkflowConfig&,
                                                std::string&) {
    return std::make_shared<ops::FfmpegConvert>();
}

std::shared_ptr<Operator> create_audio_preprocess(const nlohmann::json&,
                                                  const model::WorkflowConfig&,
                                                  std::string&) {
    return std::make_shared<ops::AudioPreprocess>();
}

std::shared_ptr<Operator> create_transcribe(const nlohmann::json& parameters,
                                            const model::WorkflowConfig& config,
                                            std::string& error) {
    std::string engine = config.transcriber_engine;
    std::string model_path = config.transcriber_model.value_or("");
    if (parameters.contains("engine")) engine = parameters["engine"].get<std::string>();
    if (parameters.contains("model")) model_path = parameters["model"].get<std::string>();

    asr::AsrBackend backend{};
    if (!asr::parse_asr_backend(engine, backend)) {
        error = "unknown ASR engine: " + engine;
        return nullptr;
    }
    if (model_path.empty()) {
        error = "transcribe operation requires a model parameter or config.transcriber_model";
        return nullptr;
    }
    return std::make_shared<ops::Transcribe>(
        std::make_shared<asr::AsrEngine>(backend, std::move(model_path)));
}

std::shared_ptr<Operator> create_sentence_split(const nlohmann::json&,
                                                const model::WorkflowConfig&,
                                                std::string&) {
    return std::make_shared<ops::SentenceSplit>();
}

class BuildOperation final : public Operator {
public:
    BuildOperation(std::string format, view::AssBuildOptions options)
        : format_(std::move(format)), options_(std::move(options)) {}

    [[nodiscard]] std::string name() const override { return "build"; }

    bool execute(PipelineContext& context) override {
        if (!context.doc || context.work_dir.empty()) {
            spdlog::error("build: pipeline document or work directory is unavailable");
            return false;
        }

        const auto& state = context.doc->state;
        const model::Track* source = nullptr;
        for (const auto& track : state.tracks) {
            if (track.type == "transcribe") {
                source = &track;
                break;
            }
        }
        if (!source) {
            spdlog::error("build: no transcribe track is available");
            return false;
        }

        const std::string extension = format_ == "ass" ? ".ass" : "." + format_;
        const std::string output_path = context.work_dir + "/output" + extension;
        std::string rendered;
        if (format_ == "ass") {
            view::AssBuildResult result;
            std::string error;
            if (!view::build_ass(*context.doc, options_, result, error)) {
                spdlog::error("build: {}", error);
                return false;
            }
            rendered = std::move(result.text);
        } else {
            model::SubtitleDocument subtitle;
            subtitle.title = context.doc->generator.tool + " " +
                             context.doc->generator.version;
            for (const auto& sentence : source->sentences) {
                model::SubtitleLine line;
                line.start = model::TimeCode(sentence.start_ms);
                line.end = model::TimeCode(sentence.end_ms);
                line.text = sentence.translated_text.value_or(sentence.text);
                subtitle.lines.push_back(std::move(line));
            }
            view::SubtitleFormat format{};
            if (!view::parse_subtitle_format(format_, format)) {
                spdlog::error("build: unsupported subtitle format '{}'", format_);
                return false;
            }
            rendered = view::render_subtitles(subtitle, format);
        }

        std::ofstream output(infra::utf8_path(output_path), std::ios::binary);
        if (!output) {
            spdlog::error("build: cannot write {}", output_path);
            return false;
        }
        output << rendered;
        if (!output) {
            spdlog::error("build: failed writing {}", output_path);
            return false;
        }

        context.subtitle_path = output_path;
        context.record_provenance(name());
        spdlog::info("build: generated {} ({} bytes)", output_path, rendered.size());
        return true;
    }

private:
    std::string format_;
    view::AssBuildOptions options_;
};

std::shared_ptr<Operator> create_build(const nlohmann::json& parameters,
                                       const model::WorkflowConfig&,
                                       std::string& error) {
    const std::string format = parameters.value("format", "ass");
    view::AssBuildOptions options;
    options.template_path = parameters.value("template", "");
    options.bilingual = parameters.value("bilingual", false);
    options.shift_ms = parameters.value("shiftMs", std::int64_t{0});
    options.scale = parameters.value("scale", 1.0);
    if (format != "ass" && format != "srt" && format != "txt") {
        error = "build parameter 'format' must be ass, srt, or txt";
        return nullptr;
    }
    if (options.scale <= 0.0) {
        error = "build parameter 'scale' must be greater than zero";
        return nullptr;
    }
    return std::make_shared<BuildOperation>(format, std::move(options));
}

class LintOperation final : public Operator {
public:
    [[nodiscard]] std::string name() const override { return "lint"; }

    bool execute(PipelineContext& context) override {
        if (context.work_dir.empty()) return false;
        quality::LintFile file;
        std::string error;
        const std::string path = context.subtitle_path.empty()
                                     ? context.work_dir + "/output.ass"
                                     : context.subtitle_path;
        if (infra::utf8_path(path).extension() != ".ass") {
            spdlog::error("lint: only ASS/SSA build outputs can be linted");
            return false;
        }
        if (!quality::load_ass_file(path, file, error)) {
            spdlog::error("lint: {}", error);
            return false;
        }
        const quality::LintOptions options;
        const auto result = quality::lint_ass(file, options, {});
        for (const auto& issue : result.issues) {
            if (issue.severity == "error") {
                spdlog::error("lint:{} [{}] {}", issue.line, issue.code, issue.message);
            }
        }
        context.record_provenance(name());
        return result.errors == 0;
    }
};

std::shared_ptr<Operator> create_lint(const nlohmann::json&,
                                      const model::WorkflowConfig&,
                                      std::string&) {
    return std::make_shared<LintOperation>();
}

const std::unordered_map<std::string, OperationContract>& operation_contracts() {
    static const std::unordered_map<std::string, OperationContract> contracts = {
        {"ffmpeg_convert", {{Facet::Media}, {Facet::Audio}, {}, create_ffmpeg_convert}},
        {"audio_preprocess", {{Facet::Audio}, {Facet::Audio}, {}, create_audio_preprocess}},
        {"transcribe", {{Facet::Audio}, {Facet::Transcript}, {"engine", "model"},
                        create_transcribe}},
        {"sentence_split", {{Facet::Transcript}, {Facet::Transcript}, {},
                            create_sentence_split}},
        {"build", {{Facet::Transcript}, {Facet::Subtitle},
                   {"format", "template", "bilingual", "shiftMs", "scale"}, create_build}},
        {"lint", {{Facet::Subtitle}, {}, {}, create_lint}},
    };
    return contracts;
}

} // namespace

const char* facet_name(Facet facet) {
    switch (facet) {
        case Facet::Media: return "media";
        case Facet::Audio: return "audio";
        case Facet::Transcript: return "transcript";
        case Facet::Words: return "words";
        case Facet::Karaoke: return "karaoke";
        case Facet::Subtitle: return "subtitle";
    }
    return "unknown";
}

bool parse_facet(const std::string& name, Facet& out) {
    for (const auto facet : {Facet::Media, Facet::Audio, Facet::Transcript, Facet::Words,
                             Facet::Karaoke, Facet::Subtitle}) {
        if (name == facet_name(facet)) {
            out = facet;
            return true;
        }
    }
    return false;
}

bool PipelineManifest::load(const std::string& path, PipelineManifest& out,
                            std::string& error) {
    std::ifstream file(infra::utf8_path(path), std::ios::binary);
    if (!file) {
        error = "cannot open pipeline manifest: " + path;
        return false;
    }

    nlohmann::json document;
    try {
        file >> document;
    } catch (const nlohmann::json::exception& exception) {
        error = "invalid pipeline JSON: " + std::string(exception.what());
        return false;
    }
    return parse(document, out, error);
}

bool PipelineManifest::parse(const nlohmann::json& document, PipelineManifest& out,
                             std::string& error) {
    error.clear();
    if (!document.is_object()) {
        error = "pipeline manifest must be a JSON object";
        return false;
    }
    if (document.size() != 2 || !document.contains("apiVersion") ||
        !document.contains("nodes")) {
        error = "pipeline manifest requires only apiVersion and nodes";
        return false;
    }
    if (!document["apiVersion"].is_number_integer() ||
        document["apiVersion"].get<int>() != 1) {
        error = "unsupported pipeline apiVersion (expected 1)";
        return false;
    }
    if (!document["nodes"].is_array() || document["nodes"].empty()) {
        error = "pipeline nodes must be a non-empty array";
        return false;
    }

    PipelineManifest candidate;
    candidate.api_version = 1;
    for (const auto& value : document["nodes"]) {
        if (!value.is_object() || !value.contains("id") || !value.contains("operation")) {
            error = "each pipeline node requires id and operation";
            return false;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() != "id" && it.key() != "operation" &&
                it.key() != "dependsOn" && it.key() != "parameters") {
                error = "unknown pipeline node field: " + it.key();
                return false;
            }
        }
        if (!value["id"].is_string() || !value["operation"].is_string()) {
            error = "pipeline node id and operation must be strings";
            return false;
        }
        PipelineNode node;
        node.id = value["id"].get<std::string>();
        node.operation = value["operation"].get<std::string>();
        if (value.contains("dependsOn")) {
            if (!value["dependsOn"].is_array()) {
                error = "pipeline node dependsOn must be an array";
                return false;
            }
            for (const auto& dependency : value["dependsOn"]) {
                if (!dependency.is_string()) {
                    error = "pipeline node dependencies must be strings";
                    return false;
                }
                node.dependencies.push_back(dependency.get<std::string>());
            }
        }
        if (value.contains("parameters")) {
            if (!value["parameters"].is_object()) {
                error = "pipeline node parameters must be an object";
                return false;
            }
            node.parameters = value["parameters"];
        }
        candidate.nodes.push_back(std::move(node));
    }

    if (!candidate.validate(error)) return false;
    out = std::move(candidate);
    return true;
}

bool PipelineManifest::validate(std::string& error) const {
    error.clear();
    if (api_version != 1) {
        error = "unsupported pipeline apiVersion (expected 1)";
        return false;
    }
    if (nodes.empty()) {
        error = "pipeline nodes must not be empty";
        return false;
    }

    std::unordered_map<std::string, std::size_t> by_id;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.id.empty()) {
            error = "pipeline node id must not be empty";
            return false;
        }
        if (!by_id.emplace(node.id, i).second) {
            error = "duplicate pipeline node id: " + node.id;
            return false;
        }
        if (operation_contracts().find(node.operation) == operation_contracts().end()) {
            error = "unknown pipeline operation: " + node.operation;
            return false;
        }
        if (!node.parameters.is_object()) {
            error = "pipeline node parameters must be an object";
            return false;
        }
        const auto& contract = operation_contracts().at(node.operation);
        for (auto it = node.parameters.begin(); it != node.parameters.end(); ++it) {
            if (std::find(contract.parameters.begin(), contract.parameters.end(), it.key()) ==
                contract.parameters.end()) {
                error = "unknown parameter '" + it.key() + "' for operation '" +
                        node.operation + "'";
                return false;
            }
        }
        if (node.operation == "transcribe") {
            for (const auto* key : {"engine", "model"}) {
                if (node.parameters.contains(key) && !node.parameters[key].is_string()) {
                    error = "transcribe parameter '" + std::string(key) + "' must be a string";
                    return false;
                }
            }
            if (node.parameters.contains("engine")) {
                const auto engine = node.parameters["engine"].get<std::string>();
                asr::AsrBackend backend{};
                if (!asr::parse_asr_backend(engine, backend)) {
                    error = "unknown ASR engine '" + engine + "' for node '" + node.id + "'";
                    return false;
                }
            }
        }
        if (node.operation == "build") {
            for (const auto* key : {"format", "template"}) {
                if (node.parameters.contains(key) && !node.parameters[key].is_string()) {
                    error = "build parameter '" + std::string(key) + "' must be a string";
                    return false;
                }
            }
            if (node.parameters.contains("bilingual") &&
                !node.parameters["bilingual"].is_boolean()) {
                error = "build parameter 'bilingual' must be a boolean";
                return false;
            }
            if (node.parameters.contains("shiftMs") &&
                !node.parameters["shiftMs"].is_number_integer()) {
                error = "build parameter 'shiftMs' must be an integer";
                return false;
            }
            if (node.parameters.contains("scale") &&
                (!node.parameters["scale"].is_number() ||
                 !std::isfinite(node.parameters["scale"].get<double>()) ||
                 node.parameters["scale"].get<double>() <= 0.0)) {
                error = "build parameter 'scale' must be a finite number greater than zero";
                return false;
            }
            if (node.parameters.contains("format")) {
                const auto format = node.parameters["format"].get<std::string>();
                if (format != "ass" && format != "srt" && format != "txt") {
                    error = "build parameter 'format' must be ass, srt, or txt";
                    return false;
                }
            }
        }
    }

    std::vector<std::vector<std::size_t>> edges(nodes.size());
    std::vector<std::size_t> indegree(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        std::unordered_set<std::string> seen_dependencies;
        for (const auto& dependency : nodes[i].dependencies) {
            const auto found = by_id.find(dependency);
            if (found == by_id.end()) {
                error = "missing dependency '" + dependency + "' for node '" + nodes[i].id + "'";
                return false;
            }
            if (dependency == nodes[i].id) {
                error = "pipeline node cannot depend on itself: " + nodes[i].id;
                return false;
            }
            if (!seen_dependencies.insert(dependency).second) {
                error = "duplicate dependency '" + dependency + "' for node '" + nodes[i].id + "'";
                return false;
            }
            edges[found->second].push_back(i);
            ++indegree[i];
        }
    }

    std::queue<std::size_t> ready;
    for (std::size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) ready.push(i);
    }
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const auto current = ready.front();
        ready.pop();
        order.push_back(current);
        for (const auto next : edges[current]) {
            if (--indegree[next] == 0) ready.push(next);
        }
    }
    if (order.size() != nodes.size()) {
        error = "pipeline contains a dependency cycle";
        return false;
    }

    std::vector<std::unordered_set<Facet>> available_facets(nodes.size());
    for (const auto index : order) {
        const auto& node = nodes[index];
        const auto& contract = operation_contracts().at(node.operation);
        std::unordered_set<Facet> inherited;
        for (const auto& dependency : node.dependencies) {
            const auto dep_index = by_id.at(dependency);
            inherited.insert(available_facets[dep_index].begin(),
                             available_facets[dep_index].end());
        }
        for (const auto facet : contract.consumes) {
            if (facet == Facet::Media) continue;
            if (inherited.find(facet) == inherited.end()) {
                error = "node '" + node.id + "' (" + node.operation +
                        ") requires facet '" + facet_name(facet) +
                        "' from a dependency";
                return false;
            }
        }
        available_facets[index] = std::move(inherited);
        available_facets[index].insert(contract.produces.begin(), contract.produces.end());
    }
    return true;
}

bool PipelineManifest::build(Pipeline& pipeline, const model::WorkflowConfig& config,
                             std::string& error) const {
    if (!validate(error)) return false;

    std::vector<std::shared_ptr<Operator>> built;
    built.reserve(nodes.size());
    for (const auto& node : nodes) {
        const auto& contract = operation_contracts().at(node.operation);
        auto implementation = contract.create(node.parameters, config, error);
        if (!implementation) {
            if (error.empty()) error = "failed to create pipeline operation: " + node.operation;
            return false;
        }
        built.push_back(std::make_shared<ManifestOperator>(
            node.id, node.dependencies, std::move(implementation)));
    }
    for (auto& operation : built) pipeline.add(std::move(operation));
    return true;
}

std::vector<OperationInfo> registered_operations() {
    std::vector<OperationInfo> operations;
    operations.reserve(operation_contracts().size());
    for (const auto& [id, contract] : operation_contracts()) {
        operations.push_back({id, contract.consumes, contract.produces});
    }
    std::sort(operations.begin(), operations.end(),
              [](const OperationInfo& left, const OperationInfo& right) {
                  return left.id < right.id;
              });
    return operations;
}

} // namespace iris::core
