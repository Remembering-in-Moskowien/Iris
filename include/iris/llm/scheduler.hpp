#pragma once

#include <string>
#include <utility>

namespace iris::llm {

struct Options {
    std::string endpoint; ///< OpenAI-compatible /chat/completions endpoint.
    std::string model;
    std::string api_key;
    int timeout_seconds = 60;
    int retries = 2;
};

class Scheduler {
public:
    explicit Scheduler(Options options) : options_(std::move(options)) {}

    bool enabled() const noexcept { return !options_.endpoint.empty(); }
    bool refine_translation(const std::string& source, const std::string& machine_translation,
                            const std::string& target_language, std::string& refined,
                            std::string& error) const;

private:
    Options options_;
};

} // namespace iris::llm
