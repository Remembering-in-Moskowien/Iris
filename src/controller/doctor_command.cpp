#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>
#include <utility>

#include "iris/asr/asr.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/process.hpp"
#include "iris/model/config.hpp"
#include "iris/tts/tts.hpp"

namespace iris::controller {

int run_doctor() {
    std::cout << "iris doctor\n";
    std::cout << "------------\n";
    const auto ffmpeg = infra::run_process({"ffmpeg", "-version"});
    std::cout << "ffmpeg: "
              << (ffmpeg.ok() && ffmpeg.value().success() ? _("available", "可用") : _("missing", "缺失"))
              << _("  (media conversion dependency, kept external)\n", "  (媒体转换依赖，保留的外部工具)\n");

    // 推理代码已内嵌进本体，运行时只引用打包好的 DLL，不再检查外部 CLI 可执行文件。
    std::cout << "asr backends (in-process):\n";
    constexpr std::pair<const char*, asr::AsrBackend> kBackends[] = {
        {"whisper", asr::AsrBackend::WhisperGgml},
        {"qwen3-asr", asr::AsrBackend::Qwen3AsrGgml},
        {"whisper-ct2", asr::AsrBackend::WhisperCt2},
    };
    for (const auto& [name, backend] : kBackends) {
        std::cout << "  " << name << ": "
                  << (asr::asr_backend_available(backend)
                          ? (backend == asr::AsrBackend::WhisperCt2 ? _("embedded (CTranslate2)", "已内嵌 (CTranslate2)")
                                                                    : _("embedded (CrispASR)", "已内嵌 (CrispASR)"))
                          : _("not compiled", "未编译"))
                  << '\n';
    }

    std::cout << "tts backends (in-process):\n";
    constexpr std::pair<const char*, tts::TtsEngine> kTtsEngines[] = {
        {"qwen3-tts", tts::TtsEngine::Qwen3Tts},
        {"indextts", tts::TtsEngine::IndexTts},
    };
    for (const auto& [name, engine] : kTtsEngines) {
        std::cout << "  " << name << ": "
                  << (tts::tts_engine_available(engine)
                          ? (engine == tts::TtsEngine::Qwen3Tts ? _("embedded (CrispASR)", "已内嵌 (CrispASR)")
                                                                : _("embedded (onnxruntime)", "已内嵌 (onnxruntime)"))
                          : _("not compiled", "未编译"))
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
