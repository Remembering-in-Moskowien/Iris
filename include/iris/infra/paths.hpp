#pragma once

#include <filesystem>
#include <string>

namespace iris::infra {

/// 当前可执行文件的绝对路径（Windows: GetModuleFileNameW；Linux: /proc/self/exe）。
std::string executable_path();

/// 可执行文件所在目录。
std::string executable_dir();

/// UTF-8 字符串 → 文件系统路径（Windows 上避免按本地代码页解释中文路径）。
std::filesystem::path utf8_path(const std::string& utf8);

/// 文件系统路径 → UTF-8 字符串。
std::string path_to_utf8(const std::filesystem::path& path);

} // namespace iris::infra
