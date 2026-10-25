#include "iris/controller/commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/infra/log.hpp"
#include "iris/infra/paths.hpp"
#include "iris/plugin/plugin_manifest.hpp"
#include "iris/plugin/process_plugin.hpp"
#include "iris/ui/console.hpp"

namespace iris::controller {
namespace {

namespace fs = std::filesystem;

const plugin::PluginManifest* find_plugin(const std::vector<plugin::PluginManifest>& plugins,
                                          const std::string& id) {
    for (const auto& plugin : plugins) {
        if (plugin.id == id) return &plugin;
    }
    return nullptr;
}

bool read_json_file(const std::string& path, nlohmann::json& out, std::string& error) {
    std::ifstream input(infra::utf8_path(path), std::ios::binary);
    if (!input) {
        error = "cannot open JSON input: " + path;
        return false;
    }
    try {
        input >> out;
    } catch (const nlohmann::json::exception& exception) {
        error = "invalid JSON input '" + path + "': " + exception.what();
        return false;
    }
    return true;
}

bool write_text_file(const fs::path& path, const std::string& content, std::string& error) {
    std::ofstream output(infra::utf8_path(infra::path_to_utf8(path)), std::ios::binary);
    if (!output) {
        error = "cannot write " + infra::path_to_utf8(path);
        return false;
    }
    output << content;
    if (!output) {
        error = "failed writing " + infra::path_to_utf8(path);
        return false;
    }
    return true;
}

std::string python_command() {
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

std::string plugin_json_template(const std::string& id, const std::string& kind) {
    nlohmann::json manifest{
        {"id", id},
        {"kind", kind},
        {"version", "0.1.0"},
        {"description", "Generated T3 process plugin"},
        {"apiVersion", 1},
        {"protocol", "stdio-jsonl"},
        {"command", python_command()},
        {"args", {"main.py"}},
        {"timeoutMs", 10000},
        {"permissions", {"fs:read"}},
        {"enabled", true}};
    return manifest.dump(2) + "\n";
}

std::string main_py_template(const std::string& id, const std::string& kind) {
    std::string text = R"PY(#!/usr/bin/env python3
"""Generated Iris T3 process plugin.

Protocol: one JSON object per line on stdin/stdout.
"""
import json
import sys

PLUGIN = {
    "id": ")PY";
    text += id;
    text += R"PY(",
    "kind": ")PY";
    text += kind;
    text += R"PY(",
    "version": "0.1.0",
    "apiVersion": 1,
}


def invoke(input_data, params):
    """Return a JSON-serializable result.

    TODO: implement your plugin. `input_data` is the caller's `--input` JSON,
    `params` is the caller's `--params` JSON.
    """
    return {"echo": input_data, "params": params}


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            continue
        request_id = request.get("id")
        method = request.get("method")
        params = request.get("params") or {}
        if method == "describe":
            response = {"id": request_id, "result": PLUGIN}
        elif method == "invoke":
            try:
                result = invoke(params.get("input", {}), params.get("params", {}))
                response = {"id": request_id, "result": result}
            except Exception as exc:  # noqa: BLE001 - report plugin failures to host
                response = {
                    "id": request_id,
                    "error": {"code": "plugin_error", "message": str(exc)},
                }
        elif method == "shutdown":
            break
        else:
            response = {
                "id": request_id,
                "error": {"code": "unknown_method", "message": method},
            }
        sys.stdout.write(json.dumps(response, ensure_ascii=False) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
)PY";
    return text;
}

std::string readme_template(const std::string& id, const std::string& kind,
                            const std::string& directory) {
    return "# " + id + "\n\n"
           "Generated Iris T3 process plugin (`kind: " + kind + "`).\n\n"
           "```bash\n"
           "iris plugin validate \"" + directory + "\"\n"
           "iris plugin invoke " + id + " --method describe\n"
           "iris plugin invoke " + id + " --method invoke --input input.json\n"
           "```\n\n"
           "Edit `main.py` and keep the stdin/stdout JSON-lines protocol: `describe`\n"
           "returns the plugin identity, `invoke` receives `{kind, input, params}`.\n";
}

} // namespace

int run_plugin_invoke(const std::string& id, const std::string& method,
                      const std::string& kind, const std::string& input_path,
                      const std::string& params_json, const std::string& out_path,
                      int timeout_ms) {
    const auto discovered = plugin::discover_plugins();
    const auto* manifest = find_plugin(discovered.plugins, id);
    if (manifest == nullptr) {
        spdlog::error("plugin invoke: plugin '{}' was not found", id);
        for (const auto& issue : discovered.issues) {
            spdlog::warn("plugin invoke: {}: {}", issue.path, issue.error);
        }
        return 1;
    }
    if (!manifest->enabled) {
        spdlog::error("plugin invoke: plugin '{}' is disabled", id);
        return 1;
    }

    nlohmann::json input = nlohmann::json::object();
    if (!input_path.empty()) {
        std::string error;
        if (!read_json_file(input_path, input, error)) {
            spdlog::error("plugin invoke: {}", error);
            return 1;
        }
    }
    nlohmann::json params = nlohmann::json::object();
    if (!params_json.empty()) {
        params = nlohmann::json::parse(params_json, nullptr, false);
        if (params.is_discarded()) {
            spdlog::error("plugin invoke: --params is not valid JSON");
            return 1;
        }
    }

    plugin::ProcessPlugin process(*manifest);
    nlohmann::json result;
    std::string error;
    bool ok = false;
    if (method == "describe") {
        ok = process.describe(result, error);
    } else if (method == "invoke") {
        ok = process.invoke(input, params, result, error);
    } else {
        nlohmann::json request_params{{"kind", kind.empty() ? manifest->kind : kind},
                                      {"input", input},
                                      {"params", params}};
        ok = process.call(method, request_params, result, error, timeout_ms);
    }

    if (!ok) {
        spdlog::error("plugin invoke: {}", error);
        return 1;
    }

    if (!out_path.empty()) {
        std::string write_error;
        if (!write_text_file(infra::utf8_path(out_path), result.dump(2) + "\n", write_error)) {
            spdlog::error("plugin invoke: {}", write_error);
            return 1;
        }
    }
    if (ui::json_mode()) {
        ui::json_out(nlohmann::json{{"ok", true}, {"id", id}, {"result", result}}.dump(2));
    } else if (out_path.empty()) {
        std::cout << result.dump(2) << '\n';
    }
    return 0;
}

int run_plugin_check(const std::string& input_path, const std::string& params_json,
                     const std::string& only_id) {
    nlohmann::json input = nlohmann::json::object();
    std::string error;
    if (!read_json_file(input_path, input, error)) {
        spdlog::error("plugin check: {}", error);
        return 1;
    }
    nlohmann::json params = nlohmann::json::object();
    if (!params_json.empty()) {
        params = nlohmann::json::parse(params_json, nullptr, false);
        if (params.is_discarded()) {
            spdlog::error("plugin check: --params is not valid JSON");
            return 1;
        }
    }

    const auto discovered = plugin::discover_plugins();
    nlohmann::json output{{"ok", true}, {"checked", 0}, {"plugins", nlohmann::json::array()}};
    bool all_ok = true;
    bool found = false;
    for (const auto& manifest : discovered.plugins) {
        if (manifest.kind != "check" || !manifest.enabled) continue;
        if (!only_id.empty() && manifest.id != only_id) continue;
        found = true;

        plugin::ProcessPlugin process(manifest);
        nlohmann::json result;
        std::string plugin_error;
        const bool ok = process.invoke(input, params, result, plugin_error);
        output["checked"] = output["checked"].get<int>() + 1;
        if (ok) {
            output["plugins"].push_back(
                {{"id", manifest.id}, {"ok", true}, {"result", result}});
        } else {
            all_ok = false;
            output["plugins"].push_back(
                {{"id", manifest.id}, {"ok", false}, {"error", plugin_error}});
        }
    }
    if (!only_id.empty() && !found) {
        spdlog::error("plugin check: no enabled check plugin with id '{}'", only_id);
        return 1;
    }
    output["ok"] = all_ok;

    if (ui::json_mode()) {
        ui::json_out(output.dump(2));
    } else {
        std::cout << output.dump(2) << '\n';
    }
    return all_ok ? 0 : 1;
}

int run_plugin_new(const std::string& id, const std::string& kind, const std::string& dir,
                   bool force) {
    plugin::PluginManifest probe;
    probe.id = id;
    probe.kind = kind;
    probe.version = "0.1.0";
    probe.command = python_command();
    std::string error;
    if (!probe.validate(error)) {
        spdlog::error("plugin new: {}", error);
        return 1;
    }

    const fs::path root = infra::utf8_path(dir);
    const fs::path target = root / id;
    std::error_code ec;
    if (fs::exists(target, ec) && !force) {
        spdlog::error("plugin new: '{}' already exists (use --force to overwrite files)",
                      infra::path_to_utf8(target));
        return 1;
    }
    fs::create_directories(target, ec);
    if (ec) {
        spdlog::error("plugin new: cannot create '{}': {}",
                      infra::path_to_utf8(target), ec.message());
        return 1;
    }

    const std::string directory_text = infra::path_to_utf8(target);
    const std::pair<const char*, std::string> files[] = {
        {"plugin.json", plugin_json_template(id, kind)},
        {"main.py", main_py_template(id, kind)},
        {"README.md", readme_template(id, kind, directory_text)},
    };
    for (const auto& [name, content] : files) {
        std::string write_error;
        if (!write_text_file(target / name, content, write_error)) {
            spdlog::error("plugin new: {}", write_error);
            return 1;
        }
    }

    if (ui::json_mode()) {
        ui::json_out(nlohmann::json{{"ok", true},
                                    {"id", id},
                                    {"path", directory_text},
                                    {"manifest", directory_text + "/plugin.json"}}
                         .dump(2));
    } else {
        spdlog::info("plugin new: created '{}'", directory_text);
        std::cout << "iris plugin validate " << directory_text << '\n'
                  << "iris plugin invoke " << id << " --method describe\n";
    }
    return 0;
}

} // namespace iris::controller
