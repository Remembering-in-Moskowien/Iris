#pragma once

#include <optional>
#include <string>
#include <utility>

namespace iris {

/// 轻量结果包装：成功携带 T，失败携带错误信息。
/// 对应 C# 版异常/Result 风格的错误传播。
template <typename T>
class Result {
public:
    static Result Ok(T value) { return Result(std::move(value)); }
    static Result Err(std::string error) { return Result(std::move(error)); }

    [[nodiscard]] bool ok() const noexcept { return !error_; }
    [[nodiscard]] bool failed() const noexcept { return static_cast<bool>(error_); }

    T& value() { return *value_; }
    [[nodiscard]] const T& value() const { return *value_; }
    T take_value() { return std::move(*value_); }

    [[nodiscard]] const std::string& error() const noexcept { return *error_; }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(std::string error) : error_(std::move(error)) {}

    std::optional<T> value_;
    std::optional<std::string> error_;
};

} // namespace iris
