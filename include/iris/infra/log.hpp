#pragma once

#include <spdlog/spdlog.h>

namespace iris::infra {

/// 初始化全局日志（默认 info；verbose 时 debug）。
inline void init_logger(bool verbose = false) {
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
}

} // namespace iris::infra
