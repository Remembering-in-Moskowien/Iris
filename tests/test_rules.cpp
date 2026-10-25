#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"
#include "iris/text/cleanup.hpp"
#include "iris/text/reference_diff.hpp"
#include "iris/text/rules.hpp"

using namespace iris;

namespace {

std::string write_temp(const std::string& name, const std::string& body) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream f(path, std::ios::binary);
    f << body;
    f.close();
    return path.string();
}

text::Glossary empty_glossary() { return {}; }

} // namespace

TEST_CASE("rule file loads macros and regex replacements") {
    const auto path = write_temp("iris_test_rules.txt",
                                 "# comment\n"
                                 "@drop-brackets\n"
                                 "@collapse-spaces\n"
                                 "@trim\n"
                                 "Hello => Hi\n"
                                 "(\\d+) => [$1]\n");
    text::RuleSet rules;
    std::string error;
    REQUIRE(text::load_rules(path, rules, error));

    std::string line = "  [Music] Hello 42  ";
    const auto hits = text::apply_rules(rules, empty_glossary(), line);
    REQUIRE(line == "Hi [42]");
    REQUIRE(hits.rules >= 3);
}

TEST_CASE("rule file rejects unknown macros and bad regex") {
    text::RuleSet rules;
    std::string error;
    const auto bad_macro = write_temp("iris_test_bad_macro.txt", "@nope\n");
    REQUIRE_FALSE(text::load_rules(bad_macro, rules, error));
    REQUIRE(error.find("未知宏") != std::string::npos);

    const auto bad_regex = write_temp("iris_test_bad_regex.txt", "([unclosed => x\n");
    REQUIRE_FALSE(text::load_rules(bad_regex, rules, error));
    REQUIRE(error.find("正则无效") != std::string::npos);
}

TEST_CASE("glossary fixes names on Chinese lines only") {
    const auto path = write_temp("iris_test_glossary.tsv",
                                 "赛蕾丝蒂亚\t塞拉斯蒂娅\n"
                                 "Celestia\t塞拉斯蒂娅\n");
    text::Glossary glossary;
    std::string error;
    REQUIRE(text::load_glossary(path, glossary, error));

    std::string zh = "赛蕾丝蒂亚 和 Celestia";
    const auto hits = text::apply_rules(text::RuleSet{}, glossary, zh);
    REQUIRE(zh == "塞拉斯蒂娅 和 塞拉斯蒂娅");
    REQUIRE(hits.glossary == 2);

    std::string en = "Celestia is here";
    text::apply_rules(text::RuleSet{}, glossary, en);
    REQUIRE(en == "Celestia is here"); // 英文行不动
}

TEST_CASE("document rules sync plain raw but leave karaoke raw locked") {
    model::IrDocument doc;
    model::Track track;
    model::Sentence a;
    a.text = "Hello !";
    a.raw = "Hello !";
    model::Sentence b;
    b.text = "Hi !";
    b.raw = "{\\K100}Hi";
    track.sentences.push_back(a);
    track.sentences.push_back(b);
    doc.state.tracks.push_back(track);

    const auto path = write_temp("iris_test_space.txt", "@space-before-punct\n");
    text::RuleSet rules;
    std::string error;
    REQUIRE(text::load_rules(path, rules, error));

    const auto stats = text::apply_rules_to_document(doc, rules, empty_glossary(), 10);
    REQUIRE(stats.changed == 2);
    REQUIRE(stats.raw_synced == 1);
    REQUIRE(stats.raw_locked == 1);
    REQUIRE(doc.state.tracks[0].sentences[0].text == "Hello!");
    REQUIRE(doc.state.tracks[0].sentences[0].raw.value() == "Hello!");
    REQUIRE(doc.state.tracks[0].sentences[1].text == "Hi!");
    REQUIRE(doc.state.tracks[0].sentences[1].raw.value() == "{\\K100}Hi");
}

TEST_CASE("cleanup keeps overlapping karaoke lines but resolves dialogue overlaps") {
    std::vector<model::Sentence> lyric;
    model::Sentence a;
    a.start_ms = 0;
    a.end_ms = 5000;
    a.text = "你";
    a.raw = "{\\K100}你";
    model::Sentence b;
    b.start_ms = 1000;
    b.end_ms = 6000;
    b.text = "好";
    b.raw = "{\\K100}好";
    lyric.push_back(a);
    lyric.push_back(b);

    text::CleanupReport report;
    text::clean_sentences(lyric, report);
    REQUIRE(report.overlap_fixed == 0);
    REQUIRE(lyric.size() == 2);
    REQUIRE(lyric[1].start_ms == 1000);

    std::vector<model::Sentence> dialogue;
    model::Sentence c;
    c.start_ms = 0;
    c.end_ms = 1000;
    c.text = "one";
    model::Sentence d;
    d.start_ms = 500;
    d.end_ms = 1500;
    d.text = "two";
    dialogue.push_back(c);
    dialogue.push_back(d);
    text::CleanupReport report2;
    text::clean_sentences(dialogue, report2);
    REQUIRE(report2.overlap_fixed == 1);
    REQUIRE(dialogue[1].start_ms == 1000);

    // 长时长 + 低语速（还没打轴的歌词行）同样保留叠行。
    std::vector<model::Sentence> slow;
    model::Sentence e;
    e.start_ms = 0;
    e.end_ms = 4000;
    e.text = "aa";
    model::Sentence f;
    f.start_ms = 1000;
    f.end_ms = 5000;
    f.text = "bb";
    slow.push_back(e);
    slow.push_back(f);
    text::CleanupReport report3;
    text::clean_sentences(slow, report3);
    REQUIRE(report3.overlap_fixed == 0);
    REQUIRE(slow[1].start_ms == 1000);
}

TEST_CASE("reference diff finds uncovered, missing and length anomalies") {
    model::IrDocument doc;
    model::Track track;
    track.type = "transcribe";
    track.language = "zh";
    model::Sentence s1;
    s1.start_ms = 0;
    s1.end_ms = 2000;
    s1.text = "hello";
    model::Sentence s2;
    s2.start_ms = 3000;
    s2.end_ms = 5000;
    s2.text = "world";
    track.sentences.push_back(s1);
    track.sentences.push_back(s2);
    doc.state.tracks.push_back(track);

    std::vector<subtitle::Cue> reference;
    subtitle::Cue c1;
    c1.start_ms = 0;
    c1.end_ms = 2000;
    c1.text = "hello";
    subtitle::Cue c2;
    c2.start_ms = 2500;
    c2.end_ms = 3500;
    c2.text = "extra";
    subtitle::Cue c3;
    c3.start_ms = 6000;
    c3.end_ms = 7000;
    c3.text = "tail";
    reference.push_back(c1);
    reference.push_back(c2);
    reference.push_back(c3);

    const auto report = text::diff_reference(doc, reference);
    REQUIRE(report.sentences == 2);
    REQUIRE(report.matched == 1);              // s1 ↔ c1 文本相同
    REQUIRE(report.missing_in_reference == 1); // s2 的同段参考是 "extra"，相似度不足
    REQUIRE(report.missing_in_ours == 2);      // c2 没被真正对上、c3 完全没覆盖
    REQUIRE(report.length_mismatch == 0);
}
