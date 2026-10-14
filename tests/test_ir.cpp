#include <catch2/catch_test_macros.hpp>

#include "daisy/model/ir_document.hpp"

using daisy::model::IrDocument;
using daisy::model::Sentence;
using daisy::model::Track;
using daisy::model::validate_document;

TEST_CASE("IR 文档 JSON 序列化往返") {
    IrDocument doc;
    doc.generator.tool = "daisy";
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
    REQUIRE(back.generator.tool == "daisy");
    REQUIRE(back.generator.command == "asr");
    REQUIRE(back.config.language == "zh");
    REQUIRE(back.config.transcriber_engine == "whisper");
    REQUIRE(back.state.tracks.size() == 1);
    REQUIRE(back.state.tracks[0].sentences.size() == 1);
    REQUIRE(back.state.tracks[0].sentences[0].text == "你好");
    REQUIRE(back.state.tracks[0].sentences[0].end_ms == 2500);
}

TEST_CASE("IR 文档校验") {
    IrDocument doc;
    doc.schema_version = "1.0";
    doc.generator.tool = "daisy";
    doc.config.input_file_path = "in.mp4";

    const auto ok = validate_document(doc);
    REQUIRE(ok.is_valid);

    IrDocument bad;
    bad.schema_version = "9.9";
    bad.generator.tool = "daisy";
    const auto fail = validate_document(bad);
    REQUIRE_FALSE(fail.is_valid);
    REQUIRE(fail.issues.size() >= 1);
}

TEST_CASE("IR 序列化保留溯源记录") {
    IrDocument doc;
    doc.generator.tool = "daisy";
    doc.config.input_file_path = "in.mp4";
    daisy::model::ProvenanceEntry p;
    p.op = "transcribe";
    p.model = "whisper.cpp";
    doc.provenance.push_back(p);

    const auto back = IrDocument::from_json(doc.to_json());
    REQUIRE(back.provenance.size() == 1);
    REQUIRE(back.provenance[0].op == "transcribe");
    REQUIRE(back.provenance[0].model == "whisper.cpp");
}
