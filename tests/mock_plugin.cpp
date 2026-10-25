// Minimal T3 process plugin used by tests/test_process_plugin.cpp.
// Speaks the same stdio JSON-lines protocol as real plugins.

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

void send(const nlohmann::json& document) {
    std::cout << document.dump() << '\n' << std::flush;
}

} // namespace

int main() {
    std::ios::sync_with_stdio(false);
    send({{"method", "log"},
          {"params", {{"level", "debug"}, {"message", "mock plugin started"}}}});
    std::cerr << "mock plugin stderr line\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object()) continue;
        const int id = request.value("id", 0);
        const std::string method = request.value("method", "");
        const nlohmann::json params =
            request.contains("params") && request["params"].is_object()
                ? request["params"]
                : nlohmann::json::object();

        if (method == "describe") {
            send({{"id", id},
                  {"result", {{"id", "mock.echo"},
                              {"kind", "check"},
                              {"version", "0.0.1"},
                              {"apiVersion", 1}}}});
        } else if (method == "invoke") {
            const nlohmann::json input =
                params.contains("input") ? params["input"] : nlohmann::json::object();
            if (input.is_object() && input.value("crash", false)) {
                return 3; // deliberate crash: host must mark the plugin degraded
            }
            if (input.is_object() && input.contains("sleepMs")) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(input["sleepMs"].get<int>()));
            }
            send({{"id", id},
                  {"result",
                   {{"echo", input},
                    {"params", params.contains("params") ? params["params"]
                                                         : nlohmann::json::object()}}}});
        } else if (method == "shutdown") {
            return 0;
        } else {
            send({{"id", id},
                  {"error", {{"code", "unknown_method"}, {"message", method}}}});
        }
    }
    return 0;
}
