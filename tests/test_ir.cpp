#include <catch2/catch_test_macros.hpp>

#include "iris/model/ir_document.hpp"

using iris::model::IrDocument;
using iris::model::Sentence;
using iris::model::Track;
using iris::model::validate_document;

TEST_CASE("IR document JSON round-trip") {
    IrDocument doc;
    doc.generator.tool = "iris";
    doc.generator.version = "0.1.0";
    doc.generator.command = "asr";
    doc.config.input_file_path = "in.mp4";
    doc.config.language = "zh";
    doc.config.transcriber_engine = "whisper";

    Sentence s;
    s.start_ms = 0;
    s.end_ms = 2500;
    s.text = "你好";
    Track t;
    t.type = "transcribe";
    t.language = "zh";
    t.sentences.push_back(s);
    doc.state.tracks.push_back(t);

    const auto json = doc.to_json();
    const auto back = IrDocument::from_json(json);

    REQUIRE(back.schema_version == doc.schema_version);
    REQUIRE(back.generator.tool == "iris");
    REQUIRE(back.generator.command == "asr");
    REQUIRE(back.config.language == "zh");
    REQUIRE(back.config.transcriber_engine == "whisper");
    REQUIRE(back.state.tracks.size() == 1);
    REQUIRE(back.state.tracks[0].sentences.size() == 1);
    REQUIRE(back.state.tracks[0].sentences[0].text == "你好");
    REQUIRE(back.state.tracks[0].sentences[0].end_ms == 2500);
}

TEST_CASE("IR document validation") {
    IrDocument doc;
    doc.schema_version = "1.0";
    doc.generator.tool = "iris";
    doc.config.input_file_path = "in.mp4";

    const auto ok = validate_document(doc);
    REQUIRE(ok.is_valid);

    IrDocument bad;
    bad.schema_version = "9.9";
    bad.generator.tool = "iris";
    const auto fail = validate_document(bad);
    REQUIRE_FALSE(fail.is_valid);
    REQUIRE(fail.issues.size() >= 1);
}

TEST_CASE("IR serialization keeps provenance") {
    IrDocument doc;
    doc.generator.tool = "iris";
    doc.config.input_file_path = "in.mp4";
    iris::model::ProvenanceEntry p;
    p.op = "transcribe";
    p.model = "whisper.cpp";
    doc.provenance.push_back(p);

    const auto back = IrDocument::from_json(doc.to_json());
    REQUIRE(back.provenance.size() == 1);
    REQUIRE(back.provenance[0].op == "transcribe");
    REQUIRE(back.provenance[0].model == "whisper.cpp");
}
