#include "iris/infra/i18n.hpp"

#include <cstdlib>

namespace iris::i18n {

Lang detect() {
    if (const char* p = std::getenv("IRIS_LANG")) {
        if (p[0] == 'z' && p[1] == 'h') return Lang::Chinese;
    }
    if (const char* p = std::getenv("LANG")) {
        std::string s(p);
        if (s.find("zh") != std::string::npos) return Lang::Chinese;
    }
    // Windows：GetUserDefaultUILanguage 简化检测
#if defined(_WIN32)
    if (const char* p = std::getenv("LANGUAGE")) {
        std::string s(p);
        if (s.find("zh") != std::string::npos) return Lang::Chinese;
    }
#endif
    return Lang::English;
}

std::string tr(const std::string& en, const std::string& zh) {
    return detect() == Lang::Chinese ? zh : en;
}

} // namespace iris::i18n
