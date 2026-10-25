#include <catch2/catch_test_macros.hpp>

#include <string>

#include "iris/model/ir_document.hpp"
#include "iris/subtitle/ass_script.hpp"
#include "iris/text/karaoke.hpp"
#include "iris/view/ass_builder.hpp"

using namespace iris;

namespace {

const char* kSource = "{\\K136}{\\K135}Once {\\K42}upon {\\K20}a {\\K124}time{\\K221}";
const char* kChinese = "景 眼前犹在";

} // namespace

TEST_CASE("lyric tokens split CJK per character and keep words whole") {
    const auto zh = text::lyric_tokens(kChinese);
    REQUIRE(zh.size() == 5);
    REQUIRE(zh[0] == "景 ");
    REQUIRE(zh[1] == "眼");
    REQUIRE(zh[4] == "在");

    const auto en = text::lyric_tokens("Once upon a time");
    REQUIRE(en.size() == 4);
    REQUIRE(en[0] == "Once ");
    REQUIRE(en[3] == "time");
}

TEST_CASE("karaoke segments parse durations and per-segment text") {
    const auto segs = text::parse_karaoke_segments(kSource);
    REQUIRE(segs.size() == 6);
    REQUIRE(segs[0].cs == 136);
    REQUIRE(segs[0].text.empty());
    REQUIRE(segs[1].text == "Once ");
    REQUIRE(segs[4].text == "time");
    REQUIRE(segs[5].cs == 221);
    REQUIRE(text::has_karaoke(kSource));
    REQUIRE_FALSE(text::has_karaoke("plain text"));
}

TEST_CASE("projection keeps the lead-in and hold and sums to the line duration") {
    text::KaraokeResult result;
    std::string error;
    REQUIRE(text::project_translation(kSource, kChinese, 678, result, &error));
    REQUIRE(result.tokens == 5);
    REQUIRE(result.total_cs == 678);
    REQUIRE(result.raw.rfind("{\\K136}", 0) == 0);

    const auto segs = text::parse_karaoke_segments(result.raw);
    int sum = 0;
    for (const auto& s : segs) sum += s.cs;
    REQUIRE(sum == 678);
    REQUIRE(segs.front().cs == 136);
    REQUIRE(segs.back().cs == 221);
    // 每个英文词按音节分配：Once(1) → 景，upon(2) → 眼/前，a(1) → 犹，time(1) → 在
    REQUIRE(segs[1].text == "景 ");
    REQUIRE(segs[1].cs == 135);
    REQUIRE(segs[2].text == "眼");
    REQUIRE(segs[2].cs == 21);
    REQUIRE(segs[3].text == "前");
    REQUIRE(segs[3].cs == 21);
    REQUIRE(segs[4].text == "犹");
    REQUIRE(segs[4].cs == 20);
    REQUIRE(segs[5].text == "在");
    REQUIRE(segs[5].cs == 124); // 长音节 "time" 对应一个长字
}

TEST_CASE("syllable count distributes characters across words") {
    const char* source = "{\\K100}My {\\K100}Little {\\K100}Pony";
    text::KaraokeResult result;
    std::string error;
    REQUIRE(text::project_translation(source, "我的小马驹", 300, result, &error));
    const auto segs = text::parse_karaoke_segments(result.raw);
    REQUIRE(segs.size() == 5);
    REQUIRE(segs[0].cs == 100); // My
    REQUIRE(segs[1].cs == 50);  // Lit-
    REQUIRE(segs[2].cs == 50);  // -tle
    REQUIRE(segs[3].cs == 50);  // Po-
    REQUIRE(segs[4].cs == 50);  // -ny
}

TEST_CASE("projection merges words that get no character") {
    const char* source = "{\\K100}One {\\K50}two {\\K50}three";
    text::KaraokeResult result;
    std::string error;
    REQUIRE(text::project_translation(source, "你好", 200, result, &error));
    const auto segs = text::parse_karaoke_segments(result.raw);
    REQUIRE(segs.size() == 2);
    REQUIRE(segs[0].cs == 100);
    REQUIRE(segs[1].cs == 100); // 没分到字的词，时长并入相邻字
    REQUIRE(result.total_cs == 200);
}

TEST_CASE("projection rescales when the target line is longer") {
    text::KaraokeResult result;
    std::string error;
    REQUIRE(text::project_translation(kSource, kChinese, 700, result, &error));
    REQUIRE(result.total_cs == 700);
}

TEST_CASE("even split divides the line across tokens") {
    const auto result = text::distribute_even("你好", 1000);
    REQUIRE(result.tokens == 2);
    REQUIRE(result.total_cs == 1000);
    const auto segs = text::parse_karaoke_segments(result.raw);
    REQUIRE(segs.size() == 2);
    REQUIRE(segs[0].cs == 500);
    REQUIRE(segs[1].cs == 500);
}

TEST_CASE("word timings become karaoke with gaps as pauses") {
    const std::vector<text::TimedToken> tokens = {{"你", 0.5, 0.8}, {"好", 1.0, 1.2}};
    const std::string raw = text::karaoke_from_timed_tokens(tokens, 0.0, 200);
    REQUIRE_FALSE(raw.empty());
    const auto segs = text::parse_karaoke_segments(raw);
    REQUIRE(segs.size() == 3);
    REQUIRE(segs[0].cs == 50);   // 引导
    REQUIRE(segs[1].cs == 50);   // 你 0.5–1.0
    REQUIRE(segs[2].cs == 100);  // 好 到行尾
    int sum = 0;
    for (const auto& s : segs) sum += s.cs;
    REQUIRE(sum == 200);
}

TEST_CASE("builder uses translatedRaw for the translated line") {
    model::IrDocument doc;
    doc.config.language = "en";
    model::Track track;
    track.type = "transcribe";
    track.language = "en";

    text::KaraokeResult result;
    std::string error;
    REQUIRE(text::project_translation(kSource, kChinese, 678, result, &error));

    model::Sentence s;
    s.start_ms = 0;
    s.end_ms = 6780;
    s.text = "Once upon a time";
    s.raw = kSource;
    s.translated_text = kChinese;
    s.translated_raw = result.raw;
    track.sentences.push_back(s);
    doc.state.tracks.push_back(track);

    view::AssBuildOptions options;
    options.bilingual = true;
    view::AssBuildResult built;
    REQUIRE(view::build_ass(doc, options, built, error));

    subtitle::AssScript out;
    REQUIRE(subtitle::parse_ass_script(built.text, out, error));
    REQUIRE(out.events.size() == 2);
    REQUIRE(out.events[0].style == "chi");
    REQUIRE(out.events[0].text.find("{\\K136}") != std::string::npos);
    REQUIRE(out.events[1].text == kSource);
}
