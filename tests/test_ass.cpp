#include <catch2/catch_test_macros.hpp>

#include "daisy/model/subtitle.hpp"

using daisy::model::SubtitleDocument;
using daisy::model::SubtitleLine;
using daisy::model::TimeCode;

TEST_CASE("ASS render includes script header and dialogue line") {
    SubtitleDocument doc;
    doc.title = "test";
    SubtitleLine line;
    line.start = TimeCode(0);
    line.end = TimeCode(1000);
    line.text = "Hello";
    doc.lines.push_back(line);

    const auto ass = doc.render_ass();
    REQUIRE(ass.find("[Script Info]") != std::string::npos);
    REQUIRE(ass.find("ScriptType: v4.00+") != std::string::npos);
    REQUIRE(ass.find("[V4+ Styles]") != std::string::npos);
    REQUIRE(ass.find("[Events]") != std::string::npos);
    REQUIRE(ass.find("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,Hello") !=
            std::string::npos);
}

TEST_CASE("ASS braces in text are escaped") {
    SubtitleDocument doc;
    SubtitleLine line;
    line.start = TimeCode(0);
    line.end = TimeCode(1000);
    line.text = "a{b}c";
    doc.lines.push_back(line);

    const auto ass = doc.render_ass();
    REQUIRE(ass.find("a\\{b\\}c") != std::string::npos);
}

TEST_CASE("Style line contains font and size") {
    daisy::model::AssStyle style;
    style.name = "Sub";
    style.font_name = "Noto Sans";
    style.font_size = 24;
    const auto s = style.to_ass();
    REQUIRE(s.find("Style: Sub,Noto Sans,24") != std::string::npos);
    REQUIRE(s.find("&H00FFFFFF") != std::string::npos);
}

TEST_CASE("SRT render includes index and timeline") {
    SubtitleDocument doc;
    SubtitleLine line;
    line.start = TimeCode(0);
    line.end = TimeCode(1000);
    line.text = "Hi";
    doc.lines.push_back(line);

    const auto srt = doc.render_srt();
    REQUIRE(srt.find("00:00:00,000 --> 00:00:01,000") != std::string::npos);
    REQUIRE(srt.find("Hi") != std::string::npos);
}
