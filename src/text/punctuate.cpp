#include "iris/text/punctuate.hpp"

#include <string>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "iris/infra/log.hpp"
#include "iris/infra/quiet.hpp"

#if IRIS_HAS_CRISPASR
#include "crispasr.h"
#include "crispasr_session.h"
#endif

namespace iris::text {

std::string default_punc_model() {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (const char* env = std::getenv("IRIS_PUNC_MODEL"); env != nullptr && *env != '\0') {
        if (fs::exists(env, ec)) return env;
    }

    std::vector<fs::path> dirs;
    if (const char* home = std::getenv("USERPROFILE"); home != nullptr && *home != '\0') {
        dirs.push_back(fs::path(home) / ".cache" / "crispasr");
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        dirs.push_back(fs::path(home) / ".cache" / "crispasr");
    }
    dirs.push_back(fs::path("models") / "punc");
    dirs.push_back(fs::path("models"));

    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            const bool punc_like = name.find("punc") != std::string::npos ||
                                   name.find("firered") != std::string::npos;
            if (punc_like && entry.path().extension() == ".gguf") return entry.path().string();
        }
        for (const auto& sub : fs::directory_iterator(dir, ec)) {
            if (ec || !sub.is_directory()) continue;
            std::error_code inner;
            for (const auto& entry : fs::directory_iterator(sub.path(), inner)) {
                if (inner) break;
                if (!entry.is_regular_file()) continue;
                const std::string name = entry.path().filename().string();
                const bool punc_like = name.find("punc") != std::string::npos ||
                                       name.find("firered") != std::string::npos;
                if (punc_like && entry.path().extension() == ".gguf") return entry.path().string();
            }
        }
    }
    return {};
}

Punctuator::~Punctuator() {
#if IRIS_HAS_CRISPASR
    if (ctx_ != nullptr) crispasr_punc_free(ctx_);
#endif
}

bool Punctuator::open(const std::string& model, std::string& error) {
#if !IRIS_HAS_CRISPASR
    (void)model;
    error = "punc: CrispASR 运行时 DLL 未就绪（tools/runtimes/crispasr）";
    return false;
#else
    if (ctx_ != nullptr) {
        crispasr_punc_free(ctx_);
        ctx_ = nullptr;
    }
    const std::string name = model.empty() ? "auto" : model;
    infra::StderrSilencer quiet;
    ctx_ = crispasr_punc_init(name.c_str());
    if (ctx_ == nullptr) {
        error = "punc: 无法加载标点模型 '" + name +
                "'（可用别名 auto|firered|fullstop|punctuate-all|pcs，或 .gguf 路径）";
        return false;
    }
    spdlog::info("punc: 标点模型就绪（{}）", name);
    return true;
#endif
}

std::string Punctuator::punctuate(const std::string& text) {
#if !IRIS_HAS_CRISPASR
    return text;
#else
    if (ctx_ == nullptr || text.empty()) return text;
    const char* out = crispasr_punc_process(ctx_, text.c_str());
    if (out == nullptr) return text;
    std::string result(out);
    crispasr_punc_free_text(out);
    return result;
#endif
}

} // namespace iris::text
