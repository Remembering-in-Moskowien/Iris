#pragma once

#include <string>

namespace iris::infra {

/// 当前可执行文件的绝对路径（Windows: GetModuleFileNameW；Linux: /proc/self/exe）。
std::string executable_path();

/// 可执行文件所在目录。
std::string executable_dir();

} // namespace iris::infra
