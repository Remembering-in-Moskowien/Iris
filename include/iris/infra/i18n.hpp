#pragma once

#include <string>

namespace iris::i18n {

/// 语言枚举。
enum class Lang { English, Chinese };

/// 自动检测当前语言：环境变量 IRIS_LANG / LANG 含 zh → Chinese，否则 English。
Lang detect();

/// 翻译：默认英语，中文环境返回中文。
/// 用法：i18n::tr("asr: model not found", "asr: 模型未找到")
std::string tr(const std::string& en, const std::string& zh);

} // namespace iris::i18n

/// 翻译宏：_("english", "中文")
#define _(EN, ZH) ::iris::i18n::tr(EN, ZH)
