#pragma once

#include <optional>
#include <string>
#include <vector>

#include "daisy/model/ir_document.hpp"

namespace daisy::core {

/// 提供者能力（对应 C# IProvider capabilities）。
enum class Capability { Asr, Ocr, Llm, Tts, Diarization, VocalSeparation };

/// 提供者注册项。
struct ProviderInfo {
    std::string name;   ///< 如 "whisper.cpp"
    std::string domain; ///< 如 "asr"
    std::string kind;   ///< "local" | "cloud"
    std::vector<Capability> capabilities;
};

/// ASR 提供者接口（对应 C# IAsrProvider）。
class IAsrProvider {
public:
    virtual ~IAsrProvider() = default;
    [[nodiscard]] virtual std::string name() const = 0;

    /// 转写音频 → 带时间戳的句子。成功返回 true 并填充 out。
    virtual bool transcribe(const std::string& audio_path,
                            const std::string& language,
                            std::vector<model::Sentence>& out,
                            std::string& error) = 0;
};

/// 提供者注册表（对应 C# ProviderRegistry / models 命令）。
class ProviderRegistry {
public:
    void register_provider(ProviderInfo info);

    std::optional<ProviderInfo> find(const std::string& domain,
                                     const std::string& name) const;
    [[nodiscard]] std::vector<ProviderInfo> all() const;

private:
    std::vector<ProviderInfo> providers_;
};

} // namespace daisy::core
