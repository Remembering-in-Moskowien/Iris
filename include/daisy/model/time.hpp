#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace daisy::model {

/// <summary>
/// 时间码。内部以毫秒存储，支持 ASS（百分秒 H:MM:SS.CC）与 SRT（毫秒 HH:MM:SS,mmm）格式的
/// 解析和格式化。对应 C# 版 SubtitleTime / 时间轴工具。
/// </summary>
class TimeCode {
public:
    constexpr TimeCode() = default;
    constexpr explicit TimeCode(std::int64_t ms) : ms_(ms) {}

    [[nodiscard]] std::int64_t ms() const noexcept { return ms_; }
    void set_ms(std::int64_t ms) noexcept { ms_ = ms; }

    /// 解析 ASS 时间码 "H:MM:SS.CC"（百分秒）。失败返回 nullopt。
    static std::optional<TimeCode> parse_ass(std::string_view s);
    /// 解析 SRT 时间码 "HH:MM:SS,mmm"（毫秒）。失败返回 nullopt。
    static std::optional<TimeCode> parse_srt(std::string_view s);
    /// 从秒（浮点）构造。
    static TimeCode from_seconds(double sec);

    /// 格式化为 ASS "H:MM:SS.CC"。
    [[nodiscard]] std::string to_ass() const;
    /// 格式化为 SRT "HH:MM:SS,mmm"。
    [[nodiscard]] std::string to_srt() const;

private:
    std::int64_t ms_ = 0;
};

} // namespace daisy::model
