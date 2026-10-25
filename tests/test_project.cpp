#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "iris/project/project.hpp"
#include "iris/quality/ass_lint.hpp"

using namespace iris;

namespace {

const char* kAss = R"ASS([Script Info]
Title: batch
ScriptType: v4.00+
WrapStyle: 0
PlayResX: 1920
PlayResY: 1080
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: chi,Dream Han Sans CN W27,81,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,28,1
Style: eng,Source Sans 3 Black,84,&H00FFFFFF,&H00FFFFFF,&H37000000,&H370E0807,-1,0,0,0,100,100,0,0,1,3.3,2.5,2,9,9,18,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,chi,,0,0,0,,{\K100}你{\K100}好
Dialogue: 0,0:00:01.00,0:00:03.00,eng,,0,0,0,,Hello
)ASS";

std::filesystem::path make_fixture() {
    const auto dir = std::filesystem::temp_directory_path() / "iris_project_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "in", ec);
    std::ofstream f(dir / "in" / "demo.ass", std::ios::binary);
    f << kAss;
    return dir;
}

} // namespace

TEST_CASE("project scan reports events and karaoke coverage") {
    const auto dir = make_fixture();
    project::ScanOptions options;
    const auto report = project::scan({(dir / "in").string()}, options);
    REQUIRE(report.files == 1);
    REQUIRE(report.entries.size() == 1);
    REQUIRE(report.entries[0].events == 2);
    REQUIRE(report.entries[0].karaoke == 1);
    REQUIRE(report.entries[0].errors == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("project run builds outputs and skips unchanged files") {
    const auto dir = make_fixture();
    project::RunOptions options;
    options.out_dir = (dir / "out").string();
    options.force = false;
    const auto manifest = (dir / "manifest.json").string();

    const auto first = project::run({(dir / "in").string()}, options, manifest);
    REQUIRE(first.error.empty());
    REQUIRE(first.files == 1);
    REQUIRE(first.ok == 1);
    REQUIRE(std::filesystem::exists(dir / "out" / "demo.ass"));
    REQUIRE(std::filesystem::exists(manifest));

    // 产物里英文行也应被同码投影补上 \K
    quality::LintFile linted;
    std::string error;
    REQUIRE(quality::load_ass_file((dir / "out" / "demo.ass").string(), linted, error));
    const auto lint = quality::lint_ass(linted, quality::LintOptions{}, {});
    REQUIRE(lint.karaoke_lines == 2);

    const auto second = project::run({(dir / "in").string()}, options, manifest);
    REQUIRE(second.skipped == 1);
    std::filesystem::remove_all(dir);
}
