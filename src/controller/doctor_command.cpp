#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>

#include "iris/asr/asr.hpp"
#include "iris/infra/process.hpp"
#include "iris/model/config.hpp"

namespace iris::controller {

int run_doctor() {
    std::cout << "iris doctor\n";
    std::cout << "------------\n";
    const auto ffmpeg = infra::run_process({"ffmpeg", "-version"});
    std::cout << "ffmpeg: "
              << (ffmpeg.ok() && ffmpeg.value().success() ? "可用" : "缺失")
              << "  (媒体转换依赖，保留的外部工具)\n";

    // ASR 推理代码已内嵌进本体（third_party 源码），不再检查外部 whisper-cli。
    std::cout << "asr backends (in-process):\n";
    constexpr std::pair<const char*, asr::AsrBackend> kBackends[] = {
        {"whisper", asr::AsrBackend::WhisperGgml},
        {"qwen3-asr", asr::AsrBackend::Qwen3AsrGgml},
        {"whisper-ct2", asr::AsrBackend::WhisperCt2},
    };
    for (const auto& [name, backend] : kBackends) {
        std::cout << "  " << name << ": "
                  << (asr::asr_backend_available(backend)
                          ? (backend == asr::AsrBackend::WhisperCt2 ? "已内嵌 (CTranslate2)"
                                                                    : "已内嵌 (CrispASR)")
                          : "未编译")
                  << '\n';
    }

    const auto cfg = model::AppConfig::load_or_default("iris.config.json");
    std::cout << "tools dir: " << cfg.tools_dir << '\n';
    std::cout << "models dir: " << cfg.models_dir << '\n';
    std::cout << "download proxy: " << (cfg.download_proxy.empty() ? "(none)" : cfg.download_proxy)
              << '\n';
    return 0;
}

} // namespace iris::controller
