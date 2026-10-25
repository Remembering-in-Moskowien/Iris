#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "iris/core/pipeline_manifest.hpp"

TEST_CASE("Pipeline manifest validates registered operations and facet flow") {
    const auto document = nlohmann::json::parse(R"({
      "apiVersion": 1,
      "nodes": [
        {"id": "convert", "operation": "ffmpeg_convert"},
        {"id": "preprocess", "operation": "audio_preprocess",
         "dependsOn": ["convert"]},
        {"id": "asr", "operation": "transcribe", "dependsOn": ["preprocess"],
         "parameters": {"engine": "whisper", "model": "models/base.gguf"}},
        {"id": "split", "operation": "sentence_split", "dependsOn": ["asr"]},
        {"id": "render", "operation": "build", "dependsOn": ["split"]},
        {"id": "lint", "operation": "lint", "dependsOn": ["render"]}
      ]
    })");

    iris::core::PipelineManifest manifest;
    std::string error;
    REQUIRE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.empty());
    REQUIRE(manifest.nodes.size() == 6);
}

TEST_CASE("Pipeline manifest rejects bad versions, unknown operations, missing facets and cycles") {
    std::string error;
    iris::core::PipelineManifest manifest;

    auto document = nlohmann::json::parse(
        R"({"apiVersion": 2, "nodes": [{"id":"a","operation":"ffmpeg_convert"}]})");
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("apiVersion") != std::string::npos);

    document = nlohmann::json::parse(
        R"({"apiVersion": 1, "nodes": [{"id":"a","operation":"not_registered"}]})");
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("unknown pipeline operation") != std::string::npos);

    document = nlohmann::json::parse(
        R"({"apiVersion": 1, "nodes": [{"id":"a","operation":"transcribe"}]})");
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("requires facet 'audio'") != std::string::npos);

    document = nlohmann::json::parse(R"({
      "apiVersion": 1,
      "nodes": [
        {"id":"a","operation":"ffmpeg_convert","dependsOn":["b"]},
        {"id":"b","operation":"audio_preprocess","dependsOn":["a"]}
      ]
    })");
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("cycle") != std::string::npos);
}

TEST_CASE("Pipeline manifest rejects undeclared fields and duplicate dependencies") {
    std::string error;
    iris::core::PipelineManifest manifest;

    auto document = nlohmann::json::parse(
        R"({"apiVersion":1,"nodes":[{"id":"a","operation":"ffmpeg_convert","extra":true}]})");
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("unknown pipeline node field") != std::string::npos);

    document = nlohmann::json::parse(R"({
      "apiVersion":1,
      "nodes":[
        {"id":"a","operation":"ffmpeg_convert"},
        {"id":"b","operation":"audio_preprocess","dependsOn":["a","a"]}
      ]
    })");
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("duplicate dependency") != std::string::npos);
}

TEST_CASE("Pipeline manifest validates build rendering parameters") {
    std::string error;
    iris::core::PipelineManifest manifest;
    auto document = nlohmann::json::parse(R"({
      "apiVersion":1,
      "nodes":[
        {"id":"media","operation":"ffmpeg_convert"},
        {"id":"asr","operation":"transcribe","dependsOn":["media"]},
        {"id":"render","operation":"build","dependsOn":["asr"],
         "parameters":{"format":"srt","bilingual":true,"shiftMs":125,"scale":2.0}}
      ]
    })");
    REQUIRE(iris::core::PipelineManifest::parse(document, manifest, error));

    document["nodes"][2]["parameters"]["format"] = "html";
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("format") != std::string::npos);

    document["nodes"][2]["parameters"]["format"] = "ass";
    document["nodes"][2]["parameters"]["scale"] = 0.0;
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("scale") != std::string::npos);
}

TEST_CASE("Pipeline manifest rejects unknown ASR engines") {
    std::string error;
    iris::core::PipelineManifest manifest;
    const auto document = nlohmann::json::parse(R"({
      "apiVersion":1,
      "nodes":[
        {"id":"media","operation":"ffmpeg_convert"},
        {"id":"asr","operation":"transcribe","dependsOn":["media"],
         "parameters":{"engine":"not-a-backend","model":"models/base.gguf"}}
      ]
    })");
    REQUIRE_FALSE(iris::core::PipelineManifest::parse(document, manifest, error));
    REQUIRE(error.find("unknown ASR engine") != std::string::npos);
}

TEST_CASE("Pipeline manifest build reports a missing transcribe model") {
    std::string error;
    iris::core::PipelineManifest manifest;
    const auto document = nlohmann::json::parse(R"({
      "apiVersion":1,
      "nodes":[
        {"id":"media","operation":"ffmpeg_convert"},
        {"id":"asr","operation":"transcribe","dependsOn":["media"],
         "parameters":{"engine":"whisper"}}
      ]
    })");
    REQUIRE(iris::core::PipelineManifest::parse(document, manifest, error));

    iris::core::Pipeline pipeline;
    iris::model::WorkflowConfig config;
    REQUIRE_FALSE(manifest.build(pipeline, config, error));
    REQUIRE(error.find("model") != std::string::npos);
}
