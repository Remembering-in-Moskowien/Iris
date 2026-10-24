#include <string>
#include <vector>

#include "iris/cli/cli.hpp"
#include "iris/infra/log.hpp"
#include "iris/ui/console.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

#ifdef _WIN32
/// Windows 上 main(int, char**) 拿到的是 ANSI 参数（中文会变成 GBK 字节），
/// 与 IR / 文件名里的 UTF-8 不一致。这里用宽字符命令行重新拆一次并转成 UTF-8。
std::vector<std::string> utf8_argv(int fallback_argc, char** fallback_argv) {
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wide == nullptr || argc <= 0) {
        for (int i = 0; i < fallback_argc; ++i) out.emplace_back(fallback_argv[i]);
        return out;
    }
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        if (need <= 0) {
            out.emplace_back();
            continue;
        }
        std::string utf8(static_cast<std::size_t>(need), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), need, nullptr, nullptr);
        utf8.pop_back(); // 去掉结尾的 NUL
        out.push_back(std::move(utf8));
    }
    LocalFree(wide);
    return out;
}
#endif

} // namespace

int main(int argc, char** argv) {
    iris::infra::init_logger();
    iris::ui::init();

#ifdef _WIN32
    auto args = utf8_argv(argc, argv);
    std::vector<char*> ptrs;
    ptrs.reserve(args.size());
    for (auto& a : args) ptrs.push_back(a.data());
    return iris::cli::run(static_cast<int>(ptrs.size()), ptrs.data());
#else
    return iris::cli::run(argc, argv);
#endif
}
