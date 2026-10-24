#include "iris/infra/paths.hpp"

#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace iris::infra {

std::string executable_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return "iris.exe";
    const std::wstring w(buf, n);
    return std::string(w.begin(), w.end());
#else
    char buf[4096] = {};
    const auto n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "iris";
    return std::string(buf, static_cast<std::size_t>(n));
#endif
}

std::string executable_dir() {
    const std::filesystem::path p(executable_path());
    return p.has_parent_path() ? p.parent_path().string() : std::string(".");
}

} // namespace iris::infra
