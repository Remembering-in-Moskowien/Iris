#pragma once

#include <string>

#include "daisy/core/provider.hpp"

namespace daisy::providers {

/// whisper.cpp 提供者：调用 whisper-cli 子进程转写（对应 C# WhisperCppProvider）。
class WhisperCppAsr : public core::IAsrProvider {
public:
    WhisperCppAsr(std::string whisper_cli_path, std::string model_path)
        : cli_path_(std::move(whisper_cli_path)),
          model_path_(std::move(model_path)) {}

    [[nodiscard]] std::string name() const override { return "whisper.cpp"; }

    bool transcribe(const std::string& audio_path, const std::string& language,
                    std::vector<model::Sentence>& out, std::string& error) override;

private:
    std::string cli_path_;
    std::string model_path_;
};

} // namespace daisy::providers
