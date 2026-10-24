#pragma once

#include <spdlog/spdlog.h>

namespace iris::infra {

/// 初始化全局日志：默认 info，verbose 时 debug。
/// 输出走 ui::write_utf8（Windows 控制台上是 WriteConsoleW），避免中文乱码。
void init_logger(bool verbose = false);

} // namespace iris::infra
