#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "iris/asr/asr.hpp"
#include "iris/core/extension_registry.hpp"
#include "iris/tts/tts.hpp"
#include "iris/view/subtitle_renderer.hpp"

TEST_CASE("Extension registry resolves ids and aliases") {
    iris::core::ExtensionRegistry<int> registry;
    std::string error;

    REQUIRE(registry.register_extension("speech.asr", {"asr", "transcribe"}, 7, error));
    REQUIRE(error.empty());

    const auto* canonical = registry.find("speech.asr");
    REQUIRE(canonical != nullptr);
    REQUIRE(canonical->id == "speech.asr");
    REQUIRE(canonical->extension == 7);
    REQUIRE(registry.find("asr") == canonical);
    REQUIRE(registry.find("transcribe") == canonical);
    REQUIRE(registry.find("missing") == nullptr);
}

TEST_CASE("Extension registry rejects ambiguous and empty identifiers") {
    iris::core::ExtensionRegistry<int> registry;
    std::string error;

    REQUIRE(registry.register_extension("speech.asr", {"asr"}, 7, error));
    REQUIRE_FALSE(registry.register_extension("speech.asr", {}, 8, error));
    REQUIRE(error.find("already registered") != std::string::npos);
    REQUIRE_FALSE(registry.register_extension("other", {"asr"}, 8, error));
    REQUIRE(error.find("already registered") != std::string::npos);
    REQUIRE_FALSE(registry.register_extension("other", {"other", "other"}, 8, error));
    REQUIRE(error.find("duplicate") != std::string::npos);
    REQUIRE_FALSE(registry.register_extension("", {}, 8, error));
    REQUIRE_FALSE(error.empty());
    REQUIRE_FALSE(registry.register_extension("other", {""}, 8, error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(registry.entries().size() == 1);
}

TEST_CASE("ASR backends resolve canonical ids and aliases through registration") {
    using iris::asr::AsrBackend;
    AsrBackend backend{};

    REQUIRE(iris::asr::parse_asr_backend("whisper", backend));
    REQUIRE(backend == AsrBackend::WhisperGgml);
    REQUIRE(iris::asr::parse_asr_backend("qwen3-asr", backend));
    REQUIRE(backend == AsrBackend::Qwen3AsrGgml);
    REQUIRE(iris::asr::parse_asr_backend("whisper-ct2", backend));
    REQUIRE(backend == AsrBackend::WhisperCt2);
    REQUIRE(iris::asr::parse_asr_backend("whisper-onnx", backend));
    REQUIRE(backend == AsrBackend::WhisperOnnx);
    REQUIRE(iris::asr::parse_asr_backend("sensevoice", backend));
    REQUIRE(backend == AsrBackend::SenseVoiceOnnx);
    REQUIRE(iris::asr::parse_asr_backend("paraformer", backend));
    REQUIRE(backend == AsrBackend::ParaformerOnnx);
    REQUIRE_FALSE(iris::asr::parse_asr_backend("unknown", backend));
    REQUIRE_FALSE(iris::asr::parse_asr_backend("", backend));
}

TEST_CASE("Backend availability matches bundled runtime configuration") {
    using iris::asr::AsrBackend;
    AsrBackend asr{};
    REQUIRE(iris::asr::parse_asr_backend("whisper", asr));
    REQUIRE(iris::asr::asr_backend_available(asr) == (IRIS_HAS_CRISPASR != 0));
    REQUIRE(iris::asr::parse_asr_backend("whisper-ct2", asr));
    REQUIRE(iris::asr::asr_backend_available(asr) == (IRIS_HAS_CT2 != 0));
    REQUIRE(iris::asr::parse_asr_backend("sensevoice", asr));
    REQUIRE(iris::asr::asr_backend_available(asr) == (IRIS_HAS_ONNX != 0));

    iris::tts::TtsEngine tts{};
    REQUIRE(iris::tts::parse_tts_engine("qwen3", tts));
    REQUIRE(iris::tts::tts_engine_available(tts) == (IRIS_HAS_CRISPASR != 0));
    REQUIRE(iris::tts::parse_tts_engine("index-tts", tts));
    REQUIRE(iris::tts::tts_engine_available(tts) == (IRIS_HAS_ONNX != 0));
}

TEST_CASE("Extension inventories expose ids, aliases and availability") {
    const auto backends = iris::asr::registered_asr_backends();
    REQUIRE(backends.size() == 6);
    const auto find_backend = [&](std::string_view id) {
        const auto it = std::find_if(backends.begin(), backends.end(),
                                     [&](const auto& entry) { return entry.id == id; });
        return it == backends.end() ? nullptr : &*it;
    };

    const auto* whisper = find_backend("whisper");
    REQUIRE(whisper != nullptr);
    REQUIRE(whisper->backend == iris::asr::AsrBackend::WhisperGgml);
    REQUIRE(whisper->aliases.empty());
    REQUIRE(whisper->available == (IRIS_HAS_CRISPASR != 0));

    const auto* qwen = find_backend("qwen3-asr");
    REQUIRE(qwen != nullptr);
    REQUIRE(qwen->available == (IRIS_HAS_CRISPASR != 0));

    const auto* ct2 = find_backend("whisper-ct2");
    REQUIRE(ct2 != nullptr);
    REQUIRE(ct2->available == (IRIS_HAS_CT2 != 0));

    const auto* sensevoice = find_backend("sensevoice-onnx");
    REQUIRE(sensevoice != nullptr);
    REQUIRE(sensevoice->aliases == std::vector<std::string>{"sensevoice"});
    REQUIRE(sensevoice->available == (IRIS_HAS_ONNX != 0));

    const auto engines = iris::tts::registered_tts_engines();
    REQUIRE(engines.size() == 2);
    const auto qwen_tts = std::find_if(engines.begin(), engines.end(), [](const auto& entry) {
        return entry.id == "qwen3-tts";
    });
    REQUIRE(qwen_tts != engines.end());
    REQUIRE(qwen_tts->engine == iris::tts::TtsEngine::Qwen3Tts);
    REQUIRE(qwen_tts->aliases.size() == 2);
    REQUIRE(qwen_tts->available == (IRIS_HAS_CRISPASR != 0));

    const auto formats = iris::view::registered_render_formats();
    REQUIRE(formats.size() == 3);
    const auto ass = std::find_if(formats.begin(), formats.end(), [](const auto& entry) {
        return entry.id == "ass";
    });
    REQUIRE(ass != formats.end());
    REQUIRE(ass->format == iris::view::SubtitleFormat::Ass);
}
