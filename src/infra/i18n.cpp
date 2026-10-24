#include "iris/infra/i18n.hpp"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace iris::i18n {

namespace {

/// 语言标签是否中文（zh / zh-CN / zh_CN / cmn / Chinese 之类）。
bool looks_chinese(const std::string& tag) {
    return tag.find("zh") != std::string::npos || tag.find("cmn") != std::string::npos ||
           tag.find("Chinese") != std::string::npos;
}

} // namespace

Lang detect() {
    // 1) 显式覆盖：IRIS_LANG=zh|en（也接受 zh-CN、en-US 这类）
    if (const char* p = std::getenv("IRIS_LANG"); p != nullptr && *p != '\0') {
        return looks_chinese(p) ? Lang::Chinese : Lang::English;
    }
    // 2) POSIX 环境变量
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"}) {
        if (const char* p = std::getenv(name); p != nullptr && *p != '\0') {
            if (looks_chinese(p)) return Lang::Chinese;
        }
    }
#ifdef _WIN32
    // 3) Windows 用户界面语言（中文系统的 LANGID 主语言是 0x04）
    const LANGID langid = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(langid) == LANG_CHINESE) return Lang::Chinese;
#endif
    return Lang::English;
}

std::string tr(const std::string& en, const std::string& zh) {
    return detect() == Lang::Chinese ? zh : en;
}

} // namespace iris::i18n
