#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "iris/model/ir_document.hpp"
#include "iris/subtitle/ass_script.hpp"
#include "iris/subtitle/ir_bridge.hpp"
#include "iris/view/ass_builder.hpp"

using namespace iris;

namespace {

const char* kFixture = R"ASS([Script Info]
Title: fixture
ScriptType: v4.00+
WrapStyle: 0
PlayResX: 1920
PlayResY: 1080
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: chi,Dream Han Sans CN W27,81,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,28,1
Style: eng,Source Sans 3 Black,84,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,100,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,chi,Twilight,0,0,0,,{\K100}你{\K100}好
Dialogue: 0,0:00:01.00,0:00:03.00,eng,Twilight,0,0,0,,Hello
Comment: 0,0:00:03.00,0:00:04.00,chi,,0,0,0,,note
)ASS";

model::IrDocument doc_from_fixture() {
    subtitle::AssScript script;
    std::string error;
    REQUIRE(subtitle::parse_ass_script(kFixture, script, error));

    model::IrDocument doc;
    doc.config.language = "en";
    doc.state.tracks.push_back(subtitle::track_from_script(script, "en"));
    subtitle::apply_presentation(script, doc.state);
    return doc;
}

} // namespace

TEST_CASE("ass script survives parse, render and re-parse") {
    subtitle::AssScript first;
    std::string error;
    REQUIRE(subtitle::parse_ass_script(kFixture, first, error));
    REQUIRE(first.styles.size() == 2);
    REQUIRE(first.events.size() == 3);

    const std::string rendered = subtitle::render_ass_script(first, true, false);
    REQUIRE(rendered.find("\r\n") != std::string::npos);

    subtitle::AssScript second;
    REQUIRE(subtitle::parse_ass_script(rendered, second, error));
    REQUIRE(second.styles.size() == first.styles.size());
    REQUIRE(second.events.size() == first.events.size());
    REQUIRE(second.events[0].text == first.events[0].text);
    REQUIRE(second.events[0].style == "chi");
    REQUIRE(second.events[0].actor == "Twilight");
    REQUIRE(second.events[0].start_ms == 1000);
    REQUIRE(second.events[2].comment);
}

TEST_CASE("build re-emits converted styles and tags (round-trip)") {
    auto doc = doc_from_fixture();
    view::AssBuildOptions options; // 无模板：用 IR 自带的样式表
    view::AssBuildResult result;
    std::string error;
    REQUIRE(view::build_ass(doc, options, result, error));

    subtitle::AssScript out;
    REQUIRE(subtitle::parse_ass_script(result.text, out, error));
    REQUIRE(out.styles.size() == 2);
    REQUIRE(out.events.size() == 3);
    REQUIRE(out.events[0].text == "{\\K100}你{\\K100}好");
    REQUIRE(out.events[0].style == "chi");
    REQUIRE(out.events[0].actor == "Twilight");
    REQUIRE(out.events[2].comment);
}

TEST_CASE("build --bilingual emits chi first then eng on the same timecode") {
    model::IrDocument doc;
    doc.config.language = "en";
    model::Track track;
    track.type = "transcribe";
    track.language = "en";

    model::Sentence a;
    a.start_ms = 1000;
    a.end_ms = 3000;
    a.text = "Hello";
    a.raw = "Hello";
    a.style = "eng";
    a.translated_text = "你好";
    track.sentences.push_back(a);

    model::Sentence b;
    b.start_ms = 3000;
    b.end_ms = 4500;
    b.text = "Bye";
    b.translated_text = "再见";
    track.sentences.push_back(b);
    doc.state.tracks.push_back(track);

    view::AssBuildOptions options;
    options.bilingual = true;
    view::AssBuildResult result;
    std::string error;
    REQUIRE(view::build_ass(doc, options, result, error));
    REQUIRE(result.stats.lines == 4);
    REQUIRE(result.stats.translated == 2);

    subtitle::AssScript out;
    REQUIRE(subtitle::parse_ass_script(result.text, out, error));
    REQUIRE(out.events.size() == 4);
    // 每对：中文在前（rank 0），英文在后（rank 1）
    REQUIRE(out.events[0].text == "你好");
    REQUIRE(out.events[0].style == "chi");
    REQUIRE(out.events[1].text == "Hello");
    REQUIRE(out.events[1].style == "eng");
    REQUIRE(out.events[0].start_ms == out.events[1].start_ms);
    // 第二条没有显式 style，按语言回落到 eng
    REQUIRE(out.events[2].style == "chi");
    REQUIRE(out.events[3].style == "eng");
}

TEST_CASE("build applies shift and scale") {
    model::IrDocument doc;
    doc.config.language = "zh";
    model::Track track;
    track.type = "transcribe";
    track.language = "zh";
    model::Sentence s;
    s.start_ms = 1000;
    s.end_ms = 2000;
    s.text = "你好";
    track.sentences.push_back(s);
    doc.state.tracks.push_back(track);

    view::AssBuildOptions options;
    options.shift_ms = 4227300; // +1:10:27.30
    options.scale = 2.0;
    view::AssBuildResult result;
    std::string error;
    REQUIRE(view::build_ass(doc, options, result, error));

    subtitle::AssScript out;
    REQUIRE(subtitle::parse_ass_script(result.text, out, error));
    REQUIRE(out.events[0].start_ms == 1000 + 4227300);
    REQUIRE(out.play_res_x() == 3840);
    const auto* chi = out.find_style("chi");
    REQUIRE(chi != nullptr);
    REQUIRE(chi->size == 162.0);
    REQUIRE(chi->margin_v == 56);
}

TEST_CASE("style language helpers map character styles") {
    REQUIRE(subtitle::style_language_rank("chi") == 0);
    REQUIRE(subtitle::style_language_rank("eng-Twilight") == 1);
    REQUIRE(subtitle::style_language_rank("Default") == 2);
    REQUIRE(subtitle::swap_style_language("chi-Twilight") == "eng-Twilight");
    REQUIRE(subtitle::swap_style_language("eng") == "chi");
    REQUIRE(subtitle::swap_style_language("Default").empty());
}

TEST_CASE("ass template contributes styles only, not events") {
    const auto path = std::filesystem::temp_directory_path() / "iris_build_template.ass";
    {
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f.good());
        f << kFixture;
    }

    model::IrDocument doc;
    doc.config.language = "en";
    model::Track track;
    track.type = "transcribe";
    track.language = "en";
    for (int i = 0; i < 2; ++i) {
        model::Sentence s;
        s.start_ms = 1000 * i;
        s.end_ms = 1000 * i + 900;
        s.text = "line";
        track.sentences.push_back(s);
    }
    doc.state.tracks.push_back(track);

    view::AssBuildOptions options;
    options.template_path = path.string();
    view::AssBuildResult result;
    std::string error;
    REQUIRE(view::build_ass(doc, options, result, error));

    subtitle::AssScript out;
    REQUIRE(subtitle::parse_ass_script(result.text, out, error));
    REQUIRE(out.events.size() == 2); // 模板里的 3 条事件不能被带出来
    REQUIRE(out.styles.size() == 2);
    std::filesystem::remove(path);
}

TEST_CASE("default template has the production geometry") {
    const auto tpl = view::default_ass_template();
    REQUIRE(tpl.play_res_x() == 1920);
    REQUIRE(tpl.styles.size() == 16);
    const auto* chi = tpl.find_style("chi");
    const auto* eng = tpl.find_style("eng");
    REQUIRE(chi != nullptr);
    REQUIRE(eng != nullptr);
    REQUIRE(chi->margin_v == 28);
    REQUIRE(eng->margin_v == 100);
    REQUIRE(tpl.find_style("eng-Twilight")->margin_v == 880);
}
