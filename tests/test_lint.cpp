#include <catch2/catch_test_macros.hpp>

#include <string>

#include "iris/quality/ass_lint.hpp"

using namespace iris::quality;

namespace {

const char* kAss = R"ASS([Script Info]
Title: test
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080
WarpStyle: 2

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: chi,Dream Han Sans CN W27,81,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,28,1
Style: eng,Source Sans 3 Black,84,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,18,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:05.00,chi,,0,0,0,,{\K100}你{\K100}好
Dialogue: 0,0:00:01.00,0:00:05.00,eng,,0,0,0,,Hello there
Dialogue: 0,0:00:05.00,0:00:09.00,missing,,0,0,0,,undefined style
Dialogue: 0,0:00:09.00,0:00:10.00,chi,,0,0,0,,How the Magic of Friendship grows
Dialogue: 0,0:00:09.50,0:00:11.00,chi,,0,0,0,,短行
Dialogue: 0,0:00:20.00,0:00:24.00,chi,,0,0,0,,啊
Dialogue: 0,0:00:24.20,0:00:28.20,chi,,0,0,0,,啊
Dialogue: 0,0:00:28.40,0:00:32.40,chi,,0,0,0,,啊
Dialogue: 0,0:00:32.60,0:00:36.60,chi,,0,0,0,,啊
)ASS";

bool has_issue(const LintFileResult& r, const std::string& code) {
    for (const auto& i : r.issues) {
        if (i.code == code) return true;
    }
    return false;
}

int count_code(const LintFileResult& r, const std::string& code) {
    int n = 0;
    for (const auto& i : r.issues) {
        if (i.code == code) ++n;
    }
    return n;
}

LintOptions no_fonts() {
    LintOptions o;
    o.check_fonts = false;
    return o;
}

} // namespace

TEST_CASE("ass lint parses styles, events and karaoke sums") {
    LintFile file;
    std::string error;
    REQUIRE(parse_ass_document(kAss, file, error));
    REQUIRE(file.play_res_x == 1920);
    REQUIRE(file.play_res_y == 1080);
    REQUIRE(file.styles.size() == 2);
    REQUIRE(file.styles[0].name == "chi");
    REQUIRE(file.styles[0].font == "Dream Han Sans CN W27");
    REQUIRE(file.styles[0].margin_v == 28);
    REQUIRE(file.styles[1].margin_v == 18);
    REQUIRE(file.wrapstyle_typo);
    REQUIRE(file.events.size() == 9);
    REQUIRE(file.events[0].karaoke_cs == 200);
    REQUIRE(file.events[0].text == "你好");
    REQUIRE_FALSE(file.events[1].has_karaoke);
}

TEST_CASE("ass lint detects BOM") {
    std::string with_bom = "\xEF\xBB\xBF";
    with_bom += kAss;
    LintFile file;
    std::string error;
    REQUIRE(parse_ass_document(with_bom, file, error));
    REQUIRE(file.has_bom);
}

TEST_CASE("ass lint reports structural and timing problems") {
    LintFile file;
    std::string error;
    REQUIRE(parse_ass_document(kAss, file, error));

    const auto r = lint_ass(file, no_fonts(), {});
    REQUIRE(r.parsed);
    REQUIRE(r.dialogues == 9);

    // 未定义样式 → error
    REQUIRE(has_issue(r, "style-undefined"));
    // 中文样式里放英文 → warning
    REQUIRE(has_issue(r, "lang-style-mismatch"));
    // 歌词只填了 2000 ms，行时长 4000 ms → warning
    REQUIRE(has_issue(r, "karaoke-under"));
    // 第 5 行与第 4 行重叠 500 ms → warning
    REQUIRE(has_issue(r, "overlap"));
    // WarpStyle 拼写
    REQUIRE(has_issue(r, "wrapstyle-typo"));
    // 连续 4 行低语速中文 → 歌词候选段
    REQUIRE(r.lyric_blocks == 1);
    REQUIRE(r.lyric_pending == 1);
    REQUIRE(has_issue(r, "lyric-candidate"));
    REQUIRE(r.errors >= 1);
}

TEST_CASE("ass lint skips font checks when disabled and flags unknown fonts") {
    LintFile file;
    std::string error;
    REQUIRE(parse_ass_document(kAss, file, error));

    // 关闭字体检查：不应出现 font-missing
    {
        const auto r = lint_ass(file, no_fonts(), {});
        REQUIRE_FALSE(has_issue(r, "font-missing"));
    }
    // 只认识一个字体：另一个样式应报警（且每个字体只报一次）
    {
        LintOptions o;
        o.check_fonts = true;
        const auto r = lint_ass(file, o, {"dreamhansanscnw27"});
        REQUIRE(count_code(r, "font-missing") == 1);
    }
}

TEST_CASE("ass lint ignores chi/eng pairs that share the same timecode") {
    const char* ass = R"ASS([Script Info]
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: chi,Dream Han Sans CN W27,81,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,28,1
Style: eng,Source Sans 3 Black,84,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,18,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,chi,,0,0,0,,你好
Dialogue: 0,0:00:01.00,0:00:03.00,eng,,0,0,0,,Hello
)ASS";
    LintFile file;
    std::string error;
    REQUIRE(parse_ass_document(ass, file, error));
    const auto r = lint_ass(file, no_fonts(), {});
    REQUIRE_FALSE(has_issue(r, "overlap"));
}

TEST_CASE("ass lint treats unknown styles by name when checking overlaps") {
    const char* ass = R"ASS([Script Info]
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: chi,Dream Han Sans CN W27,81,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,28,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,你好
Dialogue: 0,0:00:01.00,0:00:03.00,Default-L2,,0,0,0,,Hello
)ASS";
    LintFile file;
    std::string error;
    REQUIRE(parse_ass_document(ass, file, error));
    const auto r = lint_ass(file, no_fonts(), {});
    REQUIRE_FALSE(has_issue(r, "overlap"));
    REQUIRE(has_issue(r, "style-undefined"));
}

TEST_CASE("ass lint does not report overlaps for karaoke lines") {    const char* ass = R"ASS([Script Info]
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: chi,Dream Han Sans CN W27,81,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,28,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:09.00,chi,,0,0,0,,{\K400}你{\K400}好
Dialogue: 0,0:00:05.00,0:00:13.00,chi,,0,0,0,,{\K400}再{\K400}见
)ASS";
    LintFile file;
    std::string error;
    REQUIRE(parse_ass_document(ass, file, error));
    const auto r = lint_ass(file, no_fonts(), {});
    REQUIRE(r.karaoke_lines == 2);
    REQUIRE_FALSE(has_issue(r, "overlap"));
    REQUIRE_FALSE(has_issue(r, "karaoke-under"));
}
