#include "daisy/model/time.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace daisy::model {

std::optional<TimeCode> TimeCode::parse_ass(std::string_view s) {
    // 格式 H:MM:SS.CC（百分秒）
    int h = 0, m = 0, sec = 0, cs = 0;
    char dot = 0;
    std::string str(s);
    if (std::sscanf(str.c_str(), "%d:%d:%d%c%d", &h, &m, &sec, &dot, &cs) != 5)
        return std::nullopt;
    if (m < 0 || m > 59 || sec < 0 || sec > 59 || cs < 0 || cs > 99)
        return std::nullopt;
    return TimeCode(static_cast<std::int64_t>(h) * 3600000 +
                    static_cast<std::int64_t>(m) * 60000 +
                    static_cast<std::int64_t>(sec) * 1000 +
                    static_cast<std::int64_t>(cs) * 10);
}

std::optional<TimeCode> TimeCode::parse_srt(std::string_view s) {
    // 格式 HH:MM:SS,mmm（毫秒）
    int h = 0, m = 0, sec = 0, ms = 0;
    char comma = 0;
    std::string str(s);
    if (std::sscanf(str.c_str(), "%d:%d:%d%c%d", &h, &m, &sec, &comma, &ms) != 5)
        return std::nullopt;
    if (m < 0 || m > 59 || sec < 0 || sec > 59 || ms < 0 || ms > 999)
        return std::nullopt;
    return TimeCode(static_cast<std::int64_t>(h) * 3600000 +
                    static_cast<std::int64_t>(m) * 60000 +
                    static_cast<std::int64_t>(sec) * 1000 + ms);
}

TimeCode TimeCode::from_seconds(double sec) {
    return TimeCode(static_cast<std::int64_t>(std::llround(sec * 1000.0)));
}

std::string TimeCode::to_ass() const {
    std::int64_t v = ms_ < 0 ? 0 : ms_;
    const std::int64_t h = v / 3600000;
    const std::int64_t m = (v % 3600000) / 60000;
    const std::int64_t s = (v % 60000) / 1000;
    const std::int64_t cs = (v % 1000) / 10;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld",
                  static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s), static_cast<long long>(cs));
    return buf;
}

std::string TimeCode::to_srt() const {
    std::int64_t v = ms_ < 0 ? 0 : ms_;
    const std::int64_t h = v / 3600000;
    const std::int64_t m = (v % 3600000) / 60000;
    const std::int64_t s = (v % 60000) / 1000;
    const std::int64_t ms = v % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                  static_cast<long long>(h), static_cast<long long>(m),
                  static_cast<long long>(s), static_cast<long long>(ms));
    return buf;
}

} // namespace daisy::model
