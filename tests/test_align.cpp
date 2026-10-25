#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "iris/asr/align.hpp"
#include "iris/model/ir_document.hpp"

using namespace iris;

TEST_CASE("sentence range pads and clamps to the audio") {
    auto r = asr::sentence_range(1000, 3000, 10000, 0.2);
    REQUIRE(r.start == 0.8);
    REQUIRE(r.end == 3.2);

    r = asr::sentence_range(0, 1000, 10000, 0.2);
    REQUIRE(r.start == 0.0);

    r = asr::sentence_range(9500, 10000, 10000, 0.2);
    REQUIRE(r.end == 10.0);

    r = asr::sentence_range(1000, 3000, 10000, 0.0);
    REQUIRE(r.start == 1.0);
    REQUIRE(r.end == 3.0);
}

TEST_CASE("dry-run counts sentences without touching words") {
    model::Track track;
    model::Sentence a;
    a.start_ms = 0;
    a.end_ms = 2000;
    a.text = "你好";
    model::Sentence b;
    b.start_ms = 2000;
    b.end_ms = 4000;
    b.text = "世界";
    model::Sentence note;
    note.comment = true;
    note.text = "note";
    track.sentences = {a, b, note};

    std::vector<float> pcm(16000, 0.0f);
    asr::AlignOptions options;
    options.dry_run = true;
    asr::AlignStats stats;
    std::string error;
    REQUIRE(asr::align_track(track, pcm, 16000, options, stats, error));
    REQUIRE(stats.sentences == 2);
    REQUIRE(stats.aligned == 0);
    REQUIRE(track.sentences[0].words.empty());
}

TEST_CASE("alignment needs a model when not dry-running") {
    model::Track track;
    model::Sentence a;
    a.start_ms = 0;
    a.end_ms = 1000;
    a.text = "hi";
    track.sentences.push_back(a);

    std::vector<float> pcm(16000, 0.0f);
    asr::AlignOptions options;
    asr::AlignStats stats;
    std::string error;
    const bool ok = asr::align_track(track, pcm, 16000, options, stats, error);
    REQUIRE_FALSE(ok);
    REQUIRE(error.find("模型") != std::string::npos);
}
