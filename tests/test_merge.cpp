#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "iris/model/ir_document.hpp"
#include "iris/subtitle/reconcile.hpp"
#include "iris/text/cleanup.hpp"
#include "iris/text/karaoke.hpp"

using namespace iris;

namespace {

model::Track make_track(std::initializer_list<std::tuple<std::int64_t, std::int64_t, std::string>> cues,
                        const std::string& language) {
    model::Track track;
    track.type = "transcribe";
    track.language = language;
    for (const auto& [start, end, text] : cues) {
        model::Sentence s;
        s.start_ms = start;
        s.end_ms = end;
        s.text = text;
        track.sentences.push_back(std::move(s));
    }
    return track;
}

subtitle::ReconcileOptions options() {
    subtitle::ReconcileOptions o;
    o.primary_language = "en";
    o.secondary_language = "zh";
    return o;
}

} // namespace

TEST_CASE("classify_cue separates sdh, lyric, interjection and annotation") {
    using subtitle::CueRole;
    using subtitle::classify_cue;

    CHECK(classify_cue("[drum roll]") == CueRole::sdh);
    CHECK(classify_cue("-[yelps]") == CueRole::sdh);
    CHECK(classify_cue("♪ Gangle and Zooble and Kinger, too ♪") == CueRole::lyric);
    CHECK(classify_cue("[Kinger] Gah! Oh!") == CueRole::dialogue);
    CHECK(classify_cue("（出口）") == CueRole::annotation);
    CHECK(classify_cue("(music)") == CueRole::annotation);
    CHECK(classify_cue("Ah!") == CueRole::interjection);
    CHECK(classify_cue("I'm your ringmaster") == CueRole::dialogue);
    CHECK(classify_cue("-Isn't that right, Bubble?") == CueRole::dialogue);
    CHECK(classify_cue("[ambient music playing") == CueRole::sdh);
}

TEST_CASE("reconcile strips inline SDH before pairing dialogue") {
    const auto en = make_track({{1000, 3000, "[groans] No, not at all. [air rushing]"}}, "en");
    const auto zh = make_track({{1000, 3000, "不，完全不是。"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 1);
    CHECK(out.sentences[0].text == "No, not at all.");
    CHECK(out.sentences[0].translated_text.value_or("") == "不，完全不是。");
}

TEST_CASE("reconcile splits tagged multi-speaker dialogue and assigns average timing") {
    const auto en = make_track({{1000, 5000, "-[Caine] Pomni. -[Ragatha] Pomni."}}, "en");
    const auto zh = make_track({{1000, 5000, "-凯恩 Pomni。 -拉格萨 Pomni。"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].speaker.value_or("") == "Caine");
    CHECK(out.sentences[1].speaker.value_or("") == "Ragatha");
    CHECK(out.sentences[0].text == "Pomni.");
    CHECK(out.sentences[1].text == "Pomni.");
    CHECK(out.sentences[0].start_ms == 1000);
    CHECK(out.sentences[0].end_ms == 3000);
    CHECK(out.sentences[1].start_ms == 3000);
    CHECK(out.sentences[1].end_ms == 5000);
}

TEST_CASE("reconcile accepts compact bare speaker labels") {
    const auto en = make_track({{0, 2000, "-A -B Hello"}}, "en");
    const auto zh = make_track({{0, 2000, "你好"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].speaker.value_or("") == "A");
    CHECK(out.sentences[1].speaker.value_or("") == "B");
    CHECK(out.sentences[0].text == "Hello");
    CHECK(out.sentences[1].text == "Hello");
}

TEST_CASE("reconcile drops pseudo-blank cues and switch dashes") {
    const auto en = make_track({{0, 500, "- -"}, {500, 1500, "- - Pomni."},
                                {1500, 2500, "— What?!"}}, "en");
    const auto zh = make_track({{500, 1500, "小丑波姆尼"}, {1500, 2500, "什么？"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].text == "Pomni.");
    CHECK(out.sentences[1].text == "What?!");
    CHECK(out.sentences[1].role.value_or("") == "interjection");
}

TEST_CASE("reconcile does not promote sound descriptions to speakers") {
    const auto en = make_track({{0, 2000, "-[voices echoing throughout] -[Jax] Pomni."}}, "en");
    const auto zh = make_track({{0, 2000, "[回声] 小丑"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 1);
    CHECK_FALSE(out.sentences[0].speaker.has_value());
}

TEST_CASE("reconcile pairs one to one and keeps the primary timing") {
    const auto en = make_track({{1000, 3000, "Hello there"}}, "en");
    const auto zh = make_track({{1100, 2900, "你好"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 1);
    CHECK(out.sentences[0].start_ms == 1000);
    CHECK(out.sentences[0].end_ms == 3000);
    CHECK(out.sentences[0].translated_text.value_or("") == "你好");
    CHECK(out.sentences[0].style.value_or("") == "eng");
    CHECK(stats.paired == 1);
    CHECK(review.empty());
}

TEST_CASE("reconcile joins two translation cues in time order") {
    const auto en = make_track({{1000, 5000, "A B C D"}}, "en");
    const auto zh = make_track({{3000, 5000, "第二句"}, {1000, 3000, "第一句"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 1);
    CHECK(out.sentences[0].translated_text.value_or("") == "第一句 第二句");
    CHECK(stats.merged_secondary == 1);
}

TEST_CASE("reconcile splits one translation across two primary cues") {
    const auto en = make_track({{1000, 2000, "First line"}, {2000, 3000, "Second line"}}, "en");
    const auto zh = make_track({{1000, 3000, "第一句 第二句"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].translated_text.value_or("") == "第一句");
    CHECK(out.sentences[1].translated_text.value_or("") == "第二句");
    CHECK(stats.split_secondary == 1);
}

TEST_CASE("reconcile keeps an unsplittable translation on the first primary cue") {
    subtitle::ReconcileOptions o = options();
    o.min_split_chars = 30; // 强制切不开
    const auto en = make_track({{1000, 2000, "First line"}, {2000, 3000, "Second line"}}, "en");
    const auto zh = make_track({{1000, 3000, "一整句没有分隔"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, o, out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].translated_text.value_or("") == "一整句没有分隔");
    CHECK_FALSE(out.sentences[1].translated_text.has_value());
    CHECK(stats.english_merged == 0);
    CHECK(stats.split_failed == 1);
}

TEST_CASE("reconcile normalizes leading and trailing speaker dashes") {
    const auto en = make_track({{0, 1000, "-Oog! Ack! -"}, {1000, 2000, "-AUGH--"},
                                {2000, 3000, "-Jax, you"}}, "en");
    const auto zh = make_track({{0, 1000, "呃！啊！"}, {1000, 2000, "啊——"},
                                {2000, 3000, "小丑，你"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 3);
    CHECK(out.sentences[0].text == "Oog! Ack!");
    CHECK(out.sentences[0].role.value_or("") == "interjection");
    CHECK(out.sentences[1].text == "AUGH");
    CHECK(out.sentences[1].role.value_or("") == "interjection");
    CHECK(out.sentences[2].text == "Jax, you");
    for (const auto& sentence : out.sentences) {
        CHECK((sentence.text.empty() ||
               (sentence.text.front() != '-' && sentence.text.back() != '-')));
    }
}

TEST_CASE("reconcile normalizes repeated-letter stutters") {
    const auto en = make_track({{0, 2000, "O-O-Oh, yeah."}}, "en");
    const auto zh = make_track({{0, 2000, "哦，是的。"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 1);
    CHECK(out.sentences[0].text == "Oh, yeah.");
    CHECK(out.sentences[0].translated_text.value_or("") == "哦，是的。");
}

TEST_CASE("single-line policy: keep, drop and glossary") {
    const auto en = make_track({{1000, 2000, "[door creaks]"}}, "en");
    const auto zh = make_track({{5000, 6000, "无关"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;

    subtitle::ReconcileOptions keep = options();
    REQUIRE(subtitle::reconcile(en, zh, keep, out, stats, review, error));
    REQUIRE(out.sentences.size() == 1);
    CHECK_FALSE(out.sentences[0].translated_text.has_value());
    CHECK(out.sentences[0].style.value_or("") == "eng-sdh");
    CHECK(stats.kept == 1);

    subtitle::ReconcileOptions drop = options();
    drop.policy.sdh = "drop";
    REQUIRE(subtitle::reconcile(en, zh, drop, out, stats, review, error));
    CHECK(out.sentences.empty());
    CHECK(stats.dropped == 1);

    const std::string glossary_path =
        (std::filesystem::temp_directory_path() / "iris-test-sfx.tsv").string();
    {
        std::ofstream f(glossary_path, std::ios::binary);
        f << "[door creaks]\t（门吱呀作响）\n";
    }
    subtitle::ReconcileOptions gloss = options();
    gloss.policy.sdh = "glossary";
    gloss.glossary_path = glossary_path;
    REQUIRE(subtitle::reconcile(en, zh, gloss, out, stats, review, error));
    REQUIRE(out.sentences.size() == 1);
    CHECK(out.sentences[0].translated_text.value_or("") == "（门吱呀作响）");
    CHECK(stats.glossed == 1);
    std::filesystem::remove(glossary_path);
}

TEST_CASE("annotations become their own event and never duplicate a translation") {
    const auto en = make_track({{1000, 2000, "[door creaks]"}, {3000, 4000, "Run!"}}, "en");
    const auto zh = make_track({{1000, 2000, "（出口）"}, {3000, 4000, "快跑！"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 3);
    int annotations = 0;
    int translated = 0;
    for (const auto& s : out.sentences) {
        if (s.style.value_or("") == "chi-annot") ++annotations;
        if (s.translated_text) ++translated;
    }
    CHECK(annotations == 1);
    CHECK(translated == 1);
    CHECK(stats.secondary_only == 1);
}

TEST_CASE("every translation cue is used at most once") {
    const auto en = make_track({{1000, 2000, "One"}, {2000, 3000, "Two"}, {3000, 4000, "Three"}},
                               "en");
    const auto zh = make_track({{1000, 2000, "一"}, {2000, 3000, "二"}, {3000, 4000, "三"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 3);
    CHECK(out.sentences[0].translated_text.value_or("") == "一");
    CHECK(out.sentences[1].translated_text.value_or("") == "二");
    CHECK(out.sentences[2].translated_text.value_or("") == "三");
}

TEST_CASE("global sequence alignment avoids greedy cross-boundary assignments") {
    const auto en = make_track({{0, 5100, "Long line"}, {4200, 5200, "Short line"}}, "en");
    const auto zh = make_track({{1000, 2000, "前一句"}, {3500, 5100, "后一句"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].translated_text.value_or("") == "前一句");
    CHECK(out.sentences[1].translated_text.value_or("") == "后一句");
    CHECK(stats.paired == 2);
    CHECK(stats.unlinked_secondary == 0);
}

TEST_CASE("global sequence alignment supports groups of four") {
    const auto en = make_track({{0, 1000, "One"}, {1000, 2000, "Two"},
                                {2000, 3000, "Three"}, {3000, 4000, "Four"}},
                               "en");
    const auto zh = make_track({{0, 4000, "第一句第二句第三句第四句"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 4);
    CHECK(stats.split_secondary == 1);
    for (const auto& sentence : out.sentences) {
        CHECK(sentence.translated_text.has_value());
        CHECK(subtitle::visible_length(sentence.translated_text.value_or("")) == 3);
    }
}

TEST_CASE("global sequence alignment skips distant subtitle cues") {
    const auto en = make_track({{1000, 2000, "Hello there"}}, "en");
    const auto zh = make_track({{6000, 7000, "完全无关"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 1);
    CHECK_FALSE(out.sentences[0].translated_text.has_value());
    CHECK(stats.unlinked_secondary == 1);
    REQUIRE(review.size() == 1);
    CHECK(review[0].kind == "unlinked");
}

TEST_CASE("syllable weighted even split keeps the total and favours long words") {
    const auto result = text::distribute_even_weighted("a beautiful day", 300);
    REQUIRE_FALSE(result.raw.empty());
    CHECK(result.total_cs == 300);

    int sum = 0;
    std::int64_t beautiful = 0;
    std::int64_t a = 0;
    for (const auto& seg : text::parse_karaoke_segments(result.raw)) {
        sum += seg.cs;
        if (seg.text.find("beautiful") != std::string::npos) beautiful = seg.cs;
        if (seg.text.find('a') != std::string::npos && seg.text.size() <= 2) a = seg.cs;
    }
    CHECK(sum == 300);
    CHECK(beautiful > a);
    CHECK(beautiful > 100);
}

TEST_CASE("multi-speaker cues are split into one event per speaker") {
    const auto en = make_track({{1000, 3000, "-Isn't that right, Bubble? -That's right, Caine!"}}, "en");
    const auto zh = make_track({{1000, 2000, "你说对不 泡泡？"}, {2000, 3000, "太对了 凯恩"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].text == "Isn't that right, Bubble?");
    CHECK(out.sentences[0].translated_text.value_or("") == "你说对不 泡泡？");
    CHECK(out.sentences[1].text == "That's right, Caine!");
    CHECK(out.sentences[1].translated_text.value_or("") == "太对了 凯恩");
}

TEST_CASE("a single translation cue with two speakers is split too") {
    const auto en = make_track({{1000, 3000, "-No! -Wait!"}}, "en");
    const auto zh = make_track({{1000, 3000, "-不！ -等等！"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].text == "No!");
    CHECK(out.sentences[1].text == "Wait!");
    CHECK(out.sentences[0].translated_text.value_or("") == "不！");
    CHECK(out.sentences[1].translated_text.value_or("") == "等等！");
}

TEST_CASE("speaker tags move to the event name and out of the text") {
    const auto en = make_track({{1000, 2000, "[Kinger] Gah! Oh!"}, {3000, 4000, "[music]"}}, "en");
    const auto zh = make_track({{1000, 2000, "嘎！哦！"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].text == "Gah! Oh!");
    CHECK(out.sentences[0].speaker.value_or("") == "Kinger");
    CHECK(out.sentences[0].translated_text.value_or("") == "嘎！哦！");
    // 纯音效不是说话人：文字保留、speaker 为空
    CHECK(out.sentences[1].text == "[music]");
    CHECK_FALSE(out.sentences[1].speaker.has_value());
}

TEST_CASE("every english line keeps a translation even without a clause boundary") {
    const auto en = make_track({{1000, 2000, "First part"}, {2000, 3000, "Second part"}}, "en");
    const auto zh = make_track({{1000, 3000, "一二三四五六七八九十"}}, "zh");

    model::Track out;
    subtitle::ReconcileStats stats;
    std::vector<subtitle::ReconcileRow> review;
    std::string error;
    REQUIRE(subtitle::reconcile(en, zh, options(), out, stats, review, error));

    REQUIRE(out.sentences.size() == 2);
    CHECK(out.sentences[0].translated_text.value_or("").size() > 0);
    CHECK(out.sentences[1].translated_text.value_or("").size() > 0);
    CHECK(stats.split_failed == 0);
}

TEST_CASE("control characters are stripped from subtitle text") {
    const std::string dirty = "\xE2\x80\xAA[zapping]\xE2\x80\x8B";
    const std::string clean = text::strip_control_chars(dirty);
    CHECK(clean == "[zapping]");
    CHECK(text::count_control_chars(dirty) == 2);
    CHECK(text::count_control_chars(clean) == 0);
}
