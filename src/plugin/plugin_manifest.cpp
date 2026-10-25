#include "iris/plugin/plugin_manifest.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "iris/infra/paths.hpp"
#include "iris/model/config.hpp"

namespace iris::plugin {
namespace {

namespace fs = std::filesystem;

const std::set<std::string>& supported_kinds() {
    static const std::set<std::string> kinds = {
        "check", "render", "textop", "parse", "asr", "tts", "mt", "report", "model-source"};
    return kinds;
}

const std::set<std::string>& supported_permissions() {
    static const std::set<std::string> permissions = {"fs:read", "fs:write", "net",
                                                      "exec", "env", "process"};
    return permissions;
}

bool is_valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    const auto first = static_cast<unsigned char>(id.front());
    if (!(std::isalpha(first) || first == '_')) return false;
    return std::all_of(id.begin() + 1, id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '_' || c == '-';
    });
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

#ifndef _WIN32
bool executable_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (path == nullptr) return false;
    std::string text = path;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(':', start);
        const auto piece = text.substr(start, end == std::string::npos ? std::string::npos
                                                                      : end - start);
        if (!piece.empty() && ::access((piece + "/" + name).c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}
#endif

} // namespace

bool PluginManifest::load(const std::string& path, PluginManifest& out, std::string& error) {
    error.clear();
    const fs::path requested = infra::utf8_path(path);
    std::error_code ec;
    fs::path manifest_path = requested;
    if (fs::is_directory(requested, ec)) {
        manifest_path = requested / "plugin.json";
    }
    if (!fs::is_regular_file(manifest_path, ec)) {
        error = "plugin manifest not found: " + path;
        return false;
    }

    std::ifstream input(infra::utf8_path(infra::path_to_utf8(manifest_path)),
                        std::ios::binary);
    if (!input) {
        error = "cannot open plugin manifest: " + path;
        return false;
    }
    nlohmann::json document;
    try {
        input >> document;
    } catch (const nlohmann::json::exception& exception) {
        error = "invalid plugin JSON: " + std::string(exception.what());
        return false;
    }

    const fs::path base = manifest_path.parent_path();
    if (!parse(document, base, out, error)) return false;
    out.manifest_path = manifest_path;
    return true;
}

bool PluginManifest::parse(const nlohmann::json& document, const std::filesystem::path& base_directory,
                           PluginManifest& out, std::string& error) {
    error.clear();
    if (!document.is_object()) {
        error = "plugin manifest must be a JSON object";
        return false;
    }

    static const std::set<std::string> fields = {
        "id",        "kind",        "version", "description", "command", "args",
        "cwd",       "permissions", "apiVersion", "timeoutMs", "enabled", "protocol"};
    for (auto it = document.begin(); it != document.end(); ++it) {
        if (fields.find(it.key()) == fields.end()) {
            error = "unknown plugin manifest field: " + it.key();
            return false;
        }
    }

    PluginManifest candidate;
    candidate.directory = fs::absolute(base_directory).lexically_normal();
    candidate.raw = document;
    candidate.manifest_path = candidate.directory / "plugin.json";

    const auto require_string = [&](const char* key, std::string& target, bool required) {
        if (!document.contains(key)) {
            if (required) {
                error = std::string("plugin manifest requires '") + key + "'";
                return false;
            }
            return true;
        }
        if (!document[key].is_string()) {
            error = std::string("plugin manifest field '") + key + "' must be a string";
            return false;
        }
        target = document[key].get<std::string>();
        return true;
    };

    if (!require_string("id", candidate.id, true)) return false;
    if (!require_string("kind", candidate.kind, true)) return false;
    if (!require_string("version", candidate.version, true)) return false;
    if (!require_string("description", candidate.description, false)) return false;
    if (!require_string("command", candidate.command, true)) return false;
    if (!require_string("cwd", candidate.cwd, false)) return false;
    if (!require_string("protocol", candidate.protocol, false)) return false;

    if (document.contains("apiVersion")) {
        if (!document["apiVersion"].is_number_integer()) {
            error = "plugin manifest field 'apiVersion' must be an integer";
            return false;
        }
        candidate.api_version = document["apiVersion"].get<int>();
    }
    if (document.contains("timeoutMs")) {
        if (!document["timeoutMs"].is_number_integer()) {
            error = "plugin manifest field 'timeoutMs' must be an integer";
            return false;
        }
        candidate.timeout_ms = document["timeoutMs"].get<int>();
    }
    if (document.contains("enabled")) {
        if (!document["enabled"].is_boolean()) {
            error = "plugin manifest field 'enabled' must be a boolean";
            return false;
        }
        candidate.enabled = document["enabled"].get<bool>();
    }
    if (document.contains("args")) {
        if (!document["args"].is_array()) {
            error = "plugin manifest field 'args' must be an array";
            return false;
        }
        for (const auto& value : document["args"]) {
            if (!value.is_string()) {
                error = "plugin manifest args must be strings";
                return false;
            }
            candidate.args.push_back(value.get<std::string>());
        }
    }
    if (document.contains("permissions")) {
        if (!document["permissions"].is_array()) {
            error = "plugin manifest field 'permissions' must be an array";
            return false;
        }
        for (const auto& value : document["permissions"]) {
            if (!value.is_string()) {
                error = "plugin manifest permissions must be strings";
                return false;
            }
            candidate.permissions.push_back(value.get<std::string>());
        }
    }

    if (!candidate.validate(error)) return false;
    out = std::move(candidate);
    return true;
}

bool PluginManifest::validate(std::string& error) const {
    error.clear();
    if (!is_valid_id(id)) {
        error = "plugin id must start with a letter/underscore and contain only letters, "
                "digits, '.', '_' or '-'";
        return false;
    }
    if (supported_kinds().find(kind) == supported_kinds().end()) {
        error = "unsupported plugin kind: " + kind;
        return false;
    }
    if (version.empty()) {
        error = "plugin version must not be empty";
        return false;
    }
    if (api_version != 1) {
        error = "unsupported plugin apiVersion (expected 1)";
        return false;
    }
    if (protocol != "stdio-jsonl") {
        error = "unsupported plugin protocol '" + protocol + "' (expected stdio-jsonl)";
        return false;
    }
    if (command.empty()) {
        error = "plugin command must not be empty";
        return false;
    }
    if (timeout_ms < 100 || timeout_ms > 3600000) {
        error = "plugin timeoutMs must be between 100 and 3600000";
        return false;
    }
    for (const auto& permission : permissions) {
        if (supported_permissions().find(permission) == supported_permissions().end()) {
            error = "unsupported plugin permission: " + permission;
            return false;
        }
    }
    return true;
}

std::string PluginManifest::display_name() const { return id + "@" + version; }

std::string PluginManifest::resolved_command() const {
    fs::path command_path = infra::utf8_path(command);
    if (command_path.is_absolute()) return infra::path_to_utf8(command_path);
    const std::string text = command;
    if (text.find('/') == std::string::npos && text.find('\\') == std::string::npos) {
#ifdef _WIN32
        // Windows 上 "python" 常常只是 Microsoft Store 的占位符；若 PATH 里的
        // python.exe 位于 WindowsApps，优先用官方 py launcher（如果存在）。
        if (text == "python") {
            wchar_t python_path[MAX_PATH];
            wchar_t py_path[MAX_PATH];
            const DWORD python_len =
                SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, python_path, nullptr);
            const DWORD py_len =
                SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, py_path, nullptr);
            const bool placeholder =
                python_len == 0 || std::wstring(python_path).find(L"WindowsApps") !=
                                       std::wstring::npos;
            if (placeholder && py_len > 0) return "py";
        }
#endif
#ifndef _WIN32
        if (text == "python" && !executable_on_path("python") &&
            executable_on_path("python3")) {
            return "python3";
        }
#endif
        return command; // PATH lookup
    }
    return infra::path_to_utf8((directory / command_path).lexically_normal());
}

std::string PluginManifest::working_directory() const {
    if (cwd.empty()) return infra::path_to_utf8(directory);
    fs::path cwd_path = infra::utf8_path(cwd);
    if (!cwd_path.is_absolute()) cwd_path = directory / cwd_path;
    return infra::path_to_utf8(cwd_path.lexically_normal());
}

std::vector<std::string> default_plugin_dirs() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("IRIS_PLUGIN_DIRS"); env != nullptr && *env != '\0') {
        const char separator =
#ifdef _WIN32
            ';';
#else
            ':';
#endif
        std::string text = env;
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto end = text.find(separator, start);
            const auto piece = text.substr(start, end == std::string::npos ? std::string::npos
                                                                           : end - start);
            if (!piece.empty()) dirs.push_back(piece);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    dirs.push_back(".iris/plugins");
    dirs.push_back("plugins");
    dirs.push_back(infra::path_to_utf8(infra::utf8_path(model::data_root()) / "plugins"));
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
        dirs.push_back(std::string(local) + "/iris/plugins");
    }
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        dirs.push_back(std::string(xdg) + "/iris/plugins");
    }
#endif
    return dirs;
}

PluginDiscoveryResult discover_plugins(const std::vector<std::string>& extra_dirs) {
    PluginDiscoveryResult result;

    std::vector<std::string> dirs = extra_dirs;
    const auto defaults = default_plugin_dirs();
    dirs.insert(dirs.end(), defaults.begin(), defaults.end());

    std::set<std::string> visited_dirs;
    std::set<std::string> seen_ids;

    const auto load_one = [&](const std::string& path) {
        PluginManifest manifest;
        std::string error;
        if (!PluginManifest::load(path, manifest, error)) {
            result.issues.push_back({path, error});
            return;
        }
        if (!seen_ids.insert(manifest.id).second) {
            result.issues.push_back(
                {path, "duplicate plugin id '" + manifest.id + "' (earlier path wins)"});
            return;
        }
        result.plugins.push_back(std::move(manifest));
    };

    for (const auto& dir_text : dirs) {
        if (dir_text.empty()) continue;
        std::error_code ec;
        const fs::path dir = fs::weakly_canonical(infra::utf8_path(dir_text), ec);
        if (ec || !fs::is_directory(dir, ec)) continue;
        const std::string key = infra::path_to_utf8(dir);
        if (!visited_dirs.insert(key).second) continue;

        // 目录本身就是一个插件。
        if (fs::is_regular_file(dir / "plugin.json", ec)) {
            load_one(key);
            continue;
        }

        fs::directory_iterator it(dir, ec);
        if (ec) {
            result.issues.push_back({key, "cannot scan plugin directory: " + ec.message()});
            continue;
        }
        for (const auto& entry : it) {
            std::error_code entry_ec;
            if (entry.is_directory(entry_ec)) {
                if (fs::is_regular_file(entry.path() / "plugin.json", entry_ec)) {
                    load_one(infra::path_to_utf8(entry.path()));
                }
            } else if (entry.is_regular_file(entry_ec)) {
                const std::string name = infra::path_to_utf8(entry.path().filename());
                if (ends_with(name, ".plugin.json")) {
                    load_one(infra::path_to_utf8(entry.path()));
                }
            }
        }
    }

    std::sort(result.plugins.begin(), result.plugins.end(),
              [](const PluginManifest& left, const PluginManifest& right) {
                  return left.id < right.id;
              });
    return result;
}

} // namespace iris::plugin
