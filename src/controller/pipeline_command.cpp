#include "iris/controller/commands.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/asr/asr.hpp"
#include "iris/core/pipeline.hpp"
#include "iris/core/pipeline_manifest.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/plugin/plugin_manifest.hpp"
#include "iris/tts/tts.hpp"
#include "iris/ui/console.hpp"
#include "iris/view/subtitle_renderer.hpp"

namespace iris::controller {
namespace fs = std::filesystem;
namespace {

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(fs::path path) : path_(std::move(path)) {}
    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path_, ec);
        if (ec) spdlog::warn("run: cannot remove temporary work directory '{}': {}",
                             path_.string(), ec.message());
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string join_strings(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ", ";
        out += value;
    }
    return out;
}

std::string join_facets(const std::vector<core::Facet>& facets) {
    std::string out;
    for (const auto facet : facets) {
        if (!out.empty()) out += ", ";
        out += core::facet_name(facet);
    }
    return out;
}

const char* yes_no(bool value) { return value ? "yes" : "no"; }

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

int run_pipeline(const std::string& pipeline_path, const std::string& input,
                 const std::string& language, const std::string& engine,
                 const std::string& model_path, const std::string& output,
                 const std::string& render_output) {
    if (!fs::exists(infra::utf8_path(input)) ||
        !fs::is_regular_file(infra::utf8_path(input))) {
        spdlog::error("run: input file does not exist: {}", input);
        return 1;
    }

    core::PipelineManifest manifest;
    std::string error;
    if (!core::PipelineManifest::load(pipeline_path, manifest, error)) {
        spdlog::error("run: {}", error);
        return 1;
    }

    struct AsrBinding {
        std::string node_id;
        std::string engine;
        std::string model;
    };
    std::vector<AsrBinding> asr_bindings;
    for (const auto& node : manifest.nodes) {
        if (node.operation != "transcribe") continue;
        AsrBinding binding;
        binding.node_id = node.id;
        binding.engine = node.parameters.value("engine", engine);
        binding.model = node.parameters.value("model", model_path);
        if (binding.engine.empty()) {
            spdlog::error("run: transcribe node '{}' has no engine "
                          "(set parameters.engine or --engine)", node.id);
            return 1;
        }
        asr::AsrBackend backend{};
        if (!asr::parse_asr_backend(binding.engine, backend)) {
            spdlog::error("run: unknown ASR engine '{}' for node '{}'",
                          binding.engine, node.id);
            return 1;
        }
        if (!asr::asr_backend_available(backend)) {
            spdlog::error("run: ASR engine '{}' for node '{}' is unavailable in this build",
                          binding.engine, node.id);
            return 1;
        }
        if (binding.model.empty() || !fs::exists(infra::utf8_path(binding.model))) {
            spdlog::error("run: ASR model path for node '{}' does not exist: {}",
                          node.id, binding.model);
            return 1;
        }
        asr_bindings.push_back(std::move(binding));
    }

    const std::string effective_engine =
        asr_bindings.empty() ? engine : asr_bindings.front().engine;
    const std::string effective_model =
        asr_bindings.empty() ? model_path : asr_bindings.front().model;

    model::IrDocument document;
    document.generator.version = kVersion;
    document.generator.command = "run";
    document.generator.generated_at = "now";
    document.generator.input_file = input;
    document.config.input_file_path = input;
    document.config.command_name = "run";
    document.config.language = language;
    document.config.transcriber_engine = effective_engine;
    document.config.transcriber_model = effective_model;

    core::Pipeline pipeline;
    if (!manifest.build(pipeline, document.config, error)) {
        spdlog::error("run: {}", error);
        return 1;
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path work = fs::temp_directory_path() /
                          ("iris-pipeline-" + std::to_string(nonce));
    std::error_code ec;
    const bool created = fs::create_directory(work, ec);
    if (ec || !created) {
        spdlog::error("run: cannot create unique pipeline work directory{}",
                      ec ? ": " + ec.message() : "");
        return 1;
    }
    TemporaryDirectory work_cleanup(work);

    core::PipelineContext context;
    context.doc = &document;
    context.work_dir = infra::path_to_utf8(work_cleanup.path());
    if (!pipeline.run(context, &error)) {
        spdlog::error("run: {}", error);
        return 1;
    }
    if (!render_output.empty()) {
        if (context.subtitle_path.empty()) {
            spdlog::error("run: --render-output was specified but the pipeline has no build node");
            return 1;
        }
        ec.clear();
        fs::copy_file(infra::utf8_path(context.subtitle_path), infra::utf8_path(render_output),
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::error("run: cannot copy rendered subtitle to '{}': {}",
                          render_output, ec.message());
            return 1;
        }
    }

    const std::string out_path = output.empty() ? input + ".iris.json" : output;
    try {
        document.save(out_path);
    } catch (const std::exception& exception) {
        spdlog::error("run: cannot save IR '{}': {}", out_path, exception.what());
        return 1;
    }
    spdlog::info("run: pipeline complete; IR saved to {}", out_path);
    if (!render_output.empty()) {
        spdlog::info("run: rendered subtitle saved to {}", render_output);
    }
    if (ui::json_mode()) {
        nlohmann::json result{
            {"ok", true}, {"ir", out_path}, {"nodes", manifest.nodes.size()}};
        if (!render_output.empty()) result["render"] = render_output;
        ui::json_out(result.dump(2));
    }
    return 0;
}

int run_plugin_list() {
    const auto operations = core::registered_operations();
    const auto asr_backends = asr::registered_asr_backends();
    const auto tts_engines = tts::registered_tts_engines();
    const auto renderers = view::registered_render_formats();
    const auto discovered = plugin::discover_plugins();

    if (ui::json_mode()) {
        nlohmann::json document;
        document["apiVersion"] = 1;
        document["operations"] = nlohmann::json::array();
        for (const auto& operation : operations) {
            nlohmann::json consumes = nlohmann::json::array();
            for (const auto facet : operation.consumes) {
                consumes.push_back(core::facet_name(facet));
            }
            nlohmann::json produces = nlohmann::json::array();
            for (const auto facet : operation.produces) {
                produces.push_back(core::facet_name(facet));
            }
            document["operations"].push_back(
                {{"id", operation.id}, {"consumes", consumes}, {"produces", produces}});
        }
        document["asr"] = nlohmann::json::array();
        for (const auto& backend : asr_backends) {
            document["asr"].push_back({{"id", backend.id},
                                       {"aliases", backend.aliases},
                                       {"available", backend.available}});
        }
        document["tts"] = nlohmann::json::array();
        for (const auto& engine : tts_engines) {
            document["tts"].push_back({{"id", engine.id},
                                       {"aliases", engine.aliases},
                                       {"available", engine.available}});
        }
        document["renderers"] = nlohmann::json::array();
        for (const auto& renderer : renderers) {
            document["renderers"].push_back({{"id", renderer.id}});
        }
        document["process"] = nlohmann::json::array();
        for (const auto& item : discovered.plugins) {
            document["process"].push_back(
                {{"id", item.id},
                 {"kind", item.kind},
                 {"version", item.version},
                 {"enabled", item.enabled},
                 {"description", item.description},
                 {"manifest", infra::path_to_utf8(item.manifest_path)}});
        }
        document["issues"] = nlohmann::json::array();
        for (const auto& issue : discovered.issues) {
            document["issues"].push_back({{"path", issue.path}, {"error", issue.error}});
        }
        ui::json_out(document.dump(2));
        return 0;
    }

    std::vector<std::vector<std::string>> rows;
    for (const auto& operation : operations) {
        const std::string consumes = join_facets(operation.consumes);
        const std::string produces = join_facets(operation.produces);
        rows.push_back({"operation", operation.id,
                        "consumes: " + (consumes.empty() ? "-" : consumes) +
                            "; produces: " + (produces.empty() ? "-" : produces)});
    }
    for (const auto& backend : asr_backends) {
        const std::string aliases = join_strings(backend.aliases);
        rows.push_back({"asr", backend.id,
                        std::string("available: ") + yes_no(backend.available) +
                            "; aliases: " + (aliases.empty() ? "-" : aliases)});
    }
    for (const auto& engine : tts_engines) {
        const std::string aliases = join_strings(engine.aliases);
        rows.push_back({"tts", engine.id,
                        std::string("available: ") + yes_no(engine.available) +
                            "; aliases: " + (aliases.empty() ? "-" : aliases)});
    }
    for (const auto& renderer : renderers) {
        rows.push_back({"render", renderer.id, "format: " + renderer.id});
    }
    for (const auto& item : discovered.plugins) {
        rows.push_back({"process", item.id,
                        "kind: " + item.kind + "; version: " + item.version +
                            "; enabled: " + yes_no(item.enabled) +
                            "; manifest: " + infra::path_to_utf8(item.manifest_path)});
    }
    for (const auto& issue : discovered.issues) {
        rows.push_back({"issue", issue.path, issue.error});
    }
    ui::table({"kind", "id", "details"}, rows);
    return 0;
}

int run_plugin_validate(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path requested = infra::utf8_path(path);
    std::error_code ec;
    const std::string filename = infra::path_to_utf8(requested.filename());
    const bool plugin_candidate =
        fs::is_directory(requested, ec) || filename == "plugin.json" ||
        ends_with(filename, ".plugin.json");

    const auto report_plugin = [&](const plugin::PluginManifest& manifest) {
        if (ui::json_mode()) {
            nlohmann::json result{
                {"ok", true},
                {"kind", "plugin"},
                {"id", manifest.id},
                {"pluginKind", manifest.kind},
                {"version", manifest.version},
                {"apiVersion", manifest.api_version},
                {"enabled", manifest.enabled},
                {"manifest", infra::path_to_utf8(manifest.manifest_path)}};
            ui::json_out(result.dump(2));
        } else {
            spdlog::info("plugin validate: '{}' is a valid {} plugin (api {})",
                         manifest.id, manifest.kind, manifest.api_version);
        }
        return 0;
    };

    if (plugin_candidate) {
        plugin::PluginManifest manifest;
        std::string error;
        if (!plugin::PluginManifest::load(path, manifest, error)) {
            spdlog::error("plugin validate: {}", error);
            return 1;
        }
        return report_plugin(manifest);
    }

    core::PipelineManifest manifest;
    std::string error;
    if (!core::PipelineManifest::load(path, manifest, error)) {
        // 允许用户把插件清单存成任意 .json 名字：管线解析失败后再试一次。
        plugin::PluginManifest plugin_manifest;
        std::string plugin_error;
        if (plugin::PluginManifest::load(path, plugin_manifest, plugin_error)) {
            return report_plugin(plugin_manifest);
        }
        spdlog::error("plugin validate: {}", error);
        return 1;
    }

    // 结构校验之外再确认运行环境：manifest 可以跨机器传递，但这里验证的是
    // “本机构建能否执行它”，所以不可用的后端要在装配期就拒绝，而不是跑一半才报错。
    for (const auto& node : manifest.nodes) {
        if (node.operation != "transcribe" || !node.parameters.contains("engine")) {
            continue;
        }
        const auto engine = node.parameters["engine"].get<std::string>();
        asr::AsrBackend backend{};
        if (!asr::parse_asr_backend(engine, backend)) {
            spdlog::error("plugin validate: unknown ASR engine '{}' for node '{}'",
                          engine, node.id);
            return 1;
        }
        if (!asr::asr_backend_available(backend)) {
            spdlog::error("plugin validate: ASR engine '{}' for node '{}' is unavailable in this build",
                          engine, node.id);
            return 1;
        }
    }

    if (ui::json_mode()) {
        nlohmann::json result{{"ok", true},
                              {"kind", "pipeline"},
                              {"apiVersion", manifest.api_version},
                              {"nodes", manifest.nodes.size()}};
        ui::json_out(result.dump(2));
    } else {
        spdlog::info("plugin validate: pipeline manifest is valid ({} nodes)",
                     manifest.nodes.size());
    }
    return 0;
}

} // namespace iris::controller
