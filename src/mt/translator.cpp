#include "iris/mt/translator.hpp"

#include <algorithm>
#include <cctype>
#include <thread>

#include "iris/infra/log.hpp"

#if IRIS_HAS_CRISPASR
#include "crispasr.h"
#include "crispasr_session.h"
#endif

namespace iris::mt {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::string normalize_language(const std::string& code) {
    std::string s = lower(code);
    // 去掉地区/脚本后缀：en-US → en，zh-Hans → zh
    const auto cut = s.find_first_of("-_");
    if (cut != std::string::npos) s = s.substr(0, cut);
    if (s == "cmn" || s == "zho" || s == "chi") return "zh";
    if (s == "eng") return "en";
    return s;
}

namespace {

bool is_space_byte(char c) { return c == ' ' || c == '\t'; }

} // namespace

std::string normalize_output(const std::string& text, const std::string& target) {
    const std::string lang = normalize_language(target);
    const bool chinese = (lang == "zh" || lang == "ja");

    std::string out;
    out.reserve(text.size() + 8);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (chinese) {
            switch (c) {
                case ',': out += "，"; continue;
                case ';': out += "；"; continue;
                case ':': out += "："; continue;
                case '!': out += "！"; continue;
                case '?': out += "？"; continue;
                case '(': out += "（"; continue;
                case ')': out += "）"; continue;
                case '.': {
                    // 数字之间的小数点保留，其余句点转中文句号
                    const bool digit_before = i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1]));
                    const bool digit_after =
                        i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]));
                    out += (digit_before && digit_after) ? "." : "。";
                    continue;
                }
                default: break;
            }
        }
        out.push_back(c);
    }

    if (!chinese) {
        // 折叠连续空格
        std::string collapsed;
        collapsed.reserve(out.size());
        bool prev_space = false;
        for (char c : out) {
            const bool space = is_space_byte(c);
            if (space && prev_space) continue;
            collapsed.push_back(c);
            prev_space = space;
        }
        return collapsed;
    }

    // 去掉「汉字/中文标点」旁边的 ASCII 空格（UTF-8 多字节字节均 >= 0x80）
    std::string cleaned;
    cleaned.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!is_space_byte(out[i])) {
            cleaned.push_back(out[i]);
            continue;
        }
        const bool left_non_ascii =
            i > 0 && static_cast<unsigned char>(out[i - 1]) >= 0x80;
        const bool right_non_ascii =
            i + 1 < out.size() && static_cast<unsigned char>(out[i + 1]) >= 0x80;
        if (left_non_ascii || right_non_ascii) continue; // 丢掉这个空格
        cleaned.push_back(' ');
    }
    return cleaned;
}

Translator::~Translator() { close(); }

bool Translator::open(const TranslateOptions& options, std::string& error) {
#if !IRIS_HAS_CRISPASR
    (void)options;
    error = "translate: CrispASR 运行时 DLL 未就绪（tools/runtimes/crispasr）";
    return false;
#else
    close();
    opts_ = options;
    opts_.source = normalize_language(opts_.source);
    opts_.target = normalize_language(opts_.target);

    int threads = opts_.n_threads;
    if (threads <= 0) {
        threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    }

    // 后端：默认按 GGUF 自动识别；显式 --engine 时用映射后的名字（opus-mt → marian）。
    char backend[96] = {};
    std::string name = "m2m100";
    if (crispasr_detect_backend_from_gguf(opts_.model.c_str(), backend,
                                          static_cast<int>(sizeof backend)) > 0 &&
        backend[0] != '\0') {
        name = backend;
    }
    const std::string requested = lower(opts_.engine);
    if (!requested.empty() && requested != "auto") {
        name = (requested == "opus-mt" || requested == "opusmt" || requested == "marianmt")
                   ? "marian"
                   : requested;
    }

    auto* session = crispasr_session_open_explicit(opts_.model.c_str(), name.c_str(), threads);
    if (session == nullptr) {
        error = "translate: 无法加载模型 " + opts_.model + "（后端 " + name + "）";
        return false;
    }
    session_ = session;
    backend_ = name;

    if (opts_.beam_size > 0) crispasr_session_set_beam_size(session, opts_.beam_size);
    spdlog::info("translate: 模型就绪（后端 {}，{} → {}，beam {}）", backend_, opts_.source,
                 opts_.target, opts_.beam_size);
    return true;
#endif
}

void Translator::close() {
#if IRIS_HAS_CRISPASR
    if (session_ != nullptr) {
        crispasr_session_close(static_cast<crispasr_session*>(session_));
        session_ = nullptr;
    }
#endif
}

bool Translator::translate(const std::string& text, std::string& out, std::string& error) {
#if !IRIS_HAS_CRISPASR
    (void)text;
    (void)out;
    error = "translate: CrispASR 运行时 DLL 未就绪";
    return false;
#else
    if (session_ == nullptr) {
        error = "translate: 会话未打开";
        return false;
    }
    if (text.empty()) {
        out.clear();
        return true;
    }

    char* translated = crispasr_session_translate_text(
        static_cast<crispasr_session*>(session_), text.c_str(), opts_.source.c_str(),
        opts_.target.c_str(), opts_.max_tokens);
    if (translated == nullptr) {
        error = "translate: 翻译失败（" + text.substr(0, 40) + "…）";
        return false;
    }
    out = translated;
    crispasr_session_translate_text_free(translated);
    return true;
#endif
}

} // namespace iris::mt
