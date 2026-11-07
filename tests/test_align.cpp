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

TEST_CASE("sentence timing can be replaced from valid aligned words") {
    model::Sentence sentence;
    sentence.start_ms = 1000;
    sentence.end_ms = 4000;
    sentence.words = {
        model::Word{.text = "zero", .start = 1.25, .end = 1.25},
        model::Word{.text = "first", .start = 1.3, .end = 1.55},
        model::Word{.text = "last", .start = 2.1, .end = 2.6},
    };

    REQUIRE(asr::apply_word_timings_to_sentence(sentence, 10000));
    REQUIRE(sentence.start_ms == 1300);
    REQUIRE(sentence.end_ms == 2600);
}

TEST_CASE("sentence timing stays unchanged when all aligned words have zero duration") {
    model::Sentence sentence;
    sentence.start_ms = 1000;
    sentence.end_ms = 4000;
    sentence.words = {model::Word{.text = "collapsed", .start = 1.25, .end = 1.25}};

    REQUIRE_FALSE(asr::apply_word_timings_to_sentence(sentence, 10000));
    REQUIRE(sentence.start_ms == 1000);
    REQUIRE(sentence.end_ms == 4000);
}

TEST_CASE("sentence timing rejects implausibly compressed aligned words") {
    model::Sentence sentence;
    sentence.start_ms = 1000;
    sentence.end_ms = 4000;
    sentence.text = "This is a long subtitle line";
    sentence.words = {model::Word{.text = sentence.text, .start = 1.25, .end = 1.35}};

    REQUIRE_FALSE(asr::apply_word_timings_to_sentence(sentence, 10000));
    REQUIRE(sentence.start_ms == 1000);
    REQUIRE(sentence.end_ms == 4000);
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
