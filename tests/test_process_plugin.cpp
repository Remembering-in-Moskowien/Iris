#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "iris/infra/paths.hpp"
#include "iris/plugin/plugin_manifest.hpp"
#include "iris/plugin/process_plugin.hpp"

namespace {

namespace fs = std::filesystem;

fs::path mock_plugin_path() {
    fs::path path = iris::infra::utf8_path(iris::infra::executable_dir()) / "iris_mock_plugin";
#ifdef _WIN32
    path += ".exe";
#endif
    return path;
}

class TempDirectory {
public:
    TempDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / ("iris-plugin-test-" + std::to_string(nonce));
        std::error_code ec;
        fs::create_directories(path_, ec);
    }
    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream output(iris::infra::utf8_path(iris::infra::path_to_utf8(path)),
                         std::ios::binary);
    output << text;
}

iris::plugin::PluginManifest mock_manifest(int timeout_ms = 2000) {
    const nlohmann::json document{
        {"id", "mock.echo"},
        {"kind", "check"},
        {"version", "0.0.1"},
        {"apiVersion", 1},
        {"protocol", "stdio-jsonl"},
        {"command", iris::infra::path_to_utf8(mock_plugin_path())},
        {"timeoutMs", timeout_ms},
        {"permissions", {"process"}}};
    iris::plugin::PluginManifest manifest;
    std::string error;
    REQUIRE(iris::plugin::PluginManifest::parse(document, fs::temp_directory_path(), manifest,
                                                error));
    REQUIRE(error.empty());
    return manifest;
}

} // namespace

TEST_CASE("Plugin manifest validates identity, API version and permissions") {
    const nlohmann::json valid{{"id", "acme.check.cps"},
                               {"kind", "check"},
                               {"version", "1.2.3"},
                               {"apiVersion", 1},
                               {"command", "my-plugin"},
                               {"args", {"check.py"}},
                               {"permissions", {"fs:read"}},
                               {"timeoutMs", 5000}};
    iris::plugin::PluginManifest manifest;
    std::string error;
    REQUIRE(iris::plugin::PluginManifest::parse(valid, fs::temp_directory_path(), manifest,
                                                error));
    REQUIRE(manifest.id == "acme.check.cps");
    REQUIRE(manifest.resolved_command() == "my-plugin");
    REQUIRE(manifest.timeout_ms == 5000);

    auto invalid = valid;
    invalid["id"] = "bad id";
    REQUIRE_FALSE(iris::plugin::PluginManifest::parse(invalid, fs::temp_directory_path(),
                                                      manifest, error));
    REQUIRE(error.find("plugin id") != std::string::npos);

    invalid = valid;
    invalid["apiVersion"] = 2;
    REQUIRE_FALSE(iris::plugin::PluginManifest::parse(invalid, fs::temp_directory_path(),
                                                      manifest, error));
    REQUIRE(error.find("apiVersion") != std::string::npos);

    invalid = valid;
    invalid["permissions"] = {"network"}; // unsupported permission name
    REQUIRE_FALSE(iris::plugin::PluginManifest::parse(invalid, fs::temp_directory_path(),
                                                      manifest, error));
    REQUIRE(error.find("permission") != std::string::npos);

    invalid = valid;
    invalid["unknownField"] = true;
    REQUIRE_FALSE(iris::plugin::PluginManifest::parse(invalid, fs::temp_directory_path(),
                                                      manifest, error));
    REQUIRE(error.find("unknown plugin manifest field") != std::string::npos);
}

TEST_CASE("Plugin discovery respects precedence and reports bad manifests") {
    TempDirectory temp;
    const fs::path first = temp.path() / "first" / "same.id";
    const fs::path second = temp.path() / "second" / "same.id";
    const fs::path bad = temp.path() / "bad";
    fs::create_directories(first);
    fs::create_directories(second);
    fs::create_directories(bad);
    write_file(first / "plugin.json",
               nlohmann::json{{"id", "same.id"},
                              {"kind", "check"},
                              {"version", "1.0.0"},
                              {"apiVersion", 1},
                              {"command", "python"}}
                   .dump(2));
    write_file(second / "plugin.json",
               nlohmann::json{{"id", "same.id"},
                              {"kind", "check"},
                              {"version", "2.0.0"},
                              {"apiVersion", 1},
                              {"command", "python"}}
                   .dump(2));
    write_file(bad / "plugin.json", nlohmann::json{{"kind", "check"}}.dump(2));

    const auto result = iris::plugin::discover_plugins(
        {iris::infra::path_to_utf8(first.parent_path()),
         iris::infra::path_to_utf8(second.parent_path()),
         iris::infra::path_to_utf8(bad)});

    const auto found = std::find_if(result.plugins.begin(), result.plugins.end(),
                                    [](const auto& plugin) { return plugin.id == "same.id"; });
    REQUIRE(found != result.plugins.end());
    REQUIRE(found->version == "1.0.0"); // first directory wins
    REQUIRE_FALSE(result.issues.empty());
}

TEST_CASE("Process plugin starts, describes, invokes and restarts after a crash") {
    auto manifest = mock_manifest();
    iris::plugin::ProcessPlugin plugin(manifest);
    std::string error;
    nlohmann::json result;

    REQUIRE(plugin.start(error));
    REQUIRE(plugin.status() == iris::plugin::PluginStatus::Running);

    REQUIRE(plugin.describe(result, error));
    REQUIRE(result["id"] == "mock.echo");
    REQUIRE(result["kind"] == "check");

    REQUIRE(plugin.invoke({{"text", "hello"}}, {{"mode", "test"}}, result, error));
    REQUIRE(result["echo"]["text"] == "hello");
    REQUIRE(result["params"]["mode"] == "test");

    // Deliberate crash: this call fails and the plugin is marked degraded.
    REQUIRE_FALSE(plugin.invoke({{"crash", true}}, nlohmann::json::object(), result, error));
    REQUIRE(plugin.status() == iris::plugin::PluginStatus::Degraded);

    // The next call starts a fresh process automatically.
    REQUIRE(plugin.invoke({{"text", "again"}}, nlohmann::json::object(), result, error));
    REQUIRE(result["echo"]["text"] == "again");
    REQUIRE(plugin.restarts() >= 1);
    plugin.stop();
    REQUIRE(plugin.status() == iris::plugin::PluginStatus::Stopped);
}

TEST_CASE("Process plugin enforces per-call timeouts and recovers") {
    auto manifest = mock_manifest(100);
    iris::plugin::ProcessPlugin plugin(manifest);
    std::string error;
    nlohmann::json result;

    REQUIRE_FALSE(plugin.invoke({{"sleepMs", 500}}, nlohmann::json::object(), result, error));
    REQUIRE(error.find("timed out") != std::string::npos);
    REQUIRE(plugin.status() == iris::plugin::PluginStatus::Degraded);

    REQUIRE(plugin.invoke({{"text", "after timeout"}}, nlohmann::json::object(), result, error));
    REQUIRE(result["echo"]["text"] == "after timeout");
}
