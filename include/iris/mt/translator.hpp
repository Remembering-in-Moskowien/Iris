#pragma once

#include <string>

namespace iris::mt {

// ---------------------------------------------------------------------------
// 机器翻译（非 LLM：MarianMT / OPUS-MT、M2M100、WMT21、MADLAD），
// 走 CrispASR 进程内推理（`crispasr_session_translate_text`）。
//
// 模型是 GGUF，后端由文件自动识别：
//   m2m100-418m-q8_0.gguf      → backend m2m100（100 语言，任意方向）
//   opus-mt-<src>-<tgt>.gguf   → backend marian（别名 opus-mt，单方向）
// ---------------------------------------------------------------------------

struct TranslateOptions {
    std::string model;        ///< GGUF 路径
    std::string engine;       ///< auto（默认，按 GGUF 自动识别）| m2m100 | opus-mt/marian | wmt21 | madlad
    std::string source = "en";
    std::string target = "zh";
    int beam_size = 5;        ///< m2m100/marian 的 config 声明 num_beams=5；1 = 贪心
    int max_tokens = 256;
    int n_threads = 0;        ///< 0 = 硬件并发
};

/// 一个常驻的翻译会话（一次打开、多句复用；打开模型很贵）。
class Translator {
public:
    Translator() = default;
    ~Translator();
    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    bool open(const TranslateOptions& options, std::string& error);
    void close();

    /// 翻译一句。失败时 error 说明原因。
    bool translate(const std::string& text, std::string& out, std::string& error);

    [[nodiscard]] const std::string& backend() const { return backend_; }
    [[nodiscard]] const TranslateOptions& options() const { return opts_; }

private:
    void* session_ = nullptr; ///< crispasr_session*
    TranslateOptions opts_;
    std::string backend_;
};

/// 语言名归一（en / en-US → en；zh / zh-CN / cmn → zh）。
std::string normalize_language(const std::string& code);

/// 译文后处理：目标语言是中文时把半角标点转全角、去掉汉字之间的 ASCII 空格。
/// 其它语言原样返回（只做连续空格折叠）。
std::string normalize_output(const std::string& text, const std::string& target);

} // namespace iris::mt
