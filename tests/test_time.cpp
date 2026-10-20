#include <catch2/catch_test_macros.hpp>

#include "iris/model/time.hpp"

using iris::model::TimeCode;

TEST_CASE("ASS timecode parses to milliseconds") {
    auto t = TimeCode::parse_ass("0:00:01.50");
    REQUIRE(t.has_value());
    REQUIRE(t->ms() == 1500);

    auto t2 = TimeCode::parse_ass("1:02:03.99");
    REQUIRE(t2.has_value());
    REQUIRE(t2->ms() == 3723990);
}

TEST_CASE("ASS timecode formatting") {
    REQUIRE(TimeCode(1500).to_ass() == "0:00:01.50");
    REQUIRE(TimeCode(3723990).to_ass() == "1:02:03.99");
    REQUIRE(TimeCode(0).to_ass() == "0:00:00.00");
}

TEST_CASE("SRT timecode round-trip") {
    auto t = TimeCode::parse_srt("00:01:02,345");
    REQUIRE(t.has_value());
    REQUIRE(t->ms() == 62345);
    REQUIRE(t->to_srt() == "00:01:02,345");
}

TEST_CASE("Invalid timecode returns nullopt") {
    REQUIRE_FALSE(TimeCode::parse_ass("abc").has_value());
    REQUIRE_FALSE(TimeCode::parse_ass("0:60:00.00").has_value()); // 分超界
    REQUIRE_FALSE(TimeCode::parse_srt("00:00:00").has_value());   // 缺毫秒
    REQUIRE_FALSE(TimeCode::parse_srt("00:00:00,1000").has_value()); // 毫秒超界
}

TEST_CASE("Seconds to timecode") {
    REQUIRE(TimeCode::from_seconds(1.5).ms() == 1500);
    REQUIRE(TimeCode::from_seconds(0.0).ms() == 0);
}
