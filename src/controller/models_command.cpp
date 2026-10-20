#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>

#include "iris/core/provider.hpp"

namespace iris::controller {

int run_models_list() {
    core::ProviderRegistry registry;
    registry.register_provider({"asr-engine (whisper)", "asr", "local", {core::Capability::Asr}});
    registry.register_provider({"asr-engine (qwen3-asr)", "asr", "local", {core::Capability::Asr}});
    registry.register_provider({"asr-engine (whisper-ct2)", "asr", "local", {core::Capability::Asr}});
    registry.register_provider({"demucs", "vocal-separation", "local", {core::Capability::VocalSeparation}});
    registry.register_provider({"qwen3-tts", "tts", "local", {core::Capability::Tts}});

    std::cout << "已注册提供者:\n";
    for (const auto& p : registry.all()) {
        std::cout << "  " << p.name << " [" << p.domain << ", " << p.kind << "]\n";
    }
    return 0;
}

} // namespace iris::controller
