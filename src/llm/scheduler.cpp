#include "iris/llm/scheduler.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#include <nlohmann/json.hpp>

#include "iris/infra/process.hpp"

namespace iris::llm {

bool Scheduler::refine_translation(const std::string& source,
                                   const std::string& machine_translation,
                                   const std::string& target_language, std::string& refined,
                                   std::string& error) const {
    if (!enabled()) {
        error = "llm: scheduler is disabled";
        return false;
    }
    const std::string prompt =
        "You are a translation editor. Preserve the meaning, names, tone, and subtitle "
        "brevity. Return only the polished translation, with no quotes or explanation.\n"
        "Target language: " +
        target_language + "\nOriginal: " + source + "\nMachine translation: " +
        machine_translation;
    nlohmann::json request = {
        {"model", options_.model},
        {"temperature", 0.2},
        {"messages",
         {{{"role", "system"},
           {"content", "Edit translations faithfully and output only the final text."}},
          {{"role", "user"}, {"content", prompt}}}}};

    const auto temp = std::filesystem::temp_directory_path() /
                      ("iris-llm-" + std::to_string(std::rand()) + ".json");
    {
        std::ofstream out(temp, std::ios::binary);
        if (!out) {
            error = "llm: cannot create request file";
            return false;
        }
        out << request.dump();
    }

    const std::string key = options_.api_key.empty()
                                ? (std::getenv("IRIS_LLM_API_KEY") ? std::getenv("IRIS_LLM_API_KEY")
                                                                    : "")
                                : options_.api_key;
    std::vector<std::string> args = {"curl", "-sS", "--fail-with-body", "--max-time",
                                     std::to_string(options_.timeout_seconds), "-X", "POST",
                                     options_.endpoint, "-H", "Content-Type: application/json",
                                     "--data-binary", "@" + temp.string()};
    if (!key.empty()) {
        args.insert(args.end(), {"-H", "Authorization: Bearer " + key});
    }

    std::string response;
    for (int attempt = 0; attempt <= options_.retries; ++attempt) {
        const auto result = infra::run_process_split(args, "", options_.timeout_seconds + 5);
        if (!result.failed()) {
            response = result.value().stdout_text;
            break;
        }
        if (attempt == options_.retries) {
            error = "llm: request failed: " + result.error();
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::remove(temp, ec);

    const auto parsed = nlohmann::json::parse(response, nullptr, false);
    if (parsed.is_discarded() || !parsed.contains("choices") || parsed["choices"].empty() ||
        !parsed["choices"][0].contains("message")) {
        error = "llm: invalid chat-completions response";
        return false;
    }
    refined = parsed["choices"][0]["message"].value("content", "");
    if (refined.empty()) {
        error = "llm: empty refined translation";
        return false;
    }
    return true;
}

} // namespace iris::llm
