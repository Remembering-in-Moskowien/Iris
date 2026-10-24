#pragma once

#include <string>

#include "iris/tts/tts.hpp"

namespace iris::tts {

// ---------------------------------------------------------------------------
// qwen3-tts 的复用型会话（内部接口，供 TtsSession 使用）。
//
// 打开一次会做：模型/tokenizer 加载、参考音频重采样与音色编码、采样参数设置；
// 之后每句只做「文本 → 码流 → 波形」。这是 dub 逐句配音快慢的关键。
// ---------------------------------------------------------------------------

struct Qwen3Session {
    void* session = nullptr; ///< crispasr_session*
    int sample_rate = 0;
    std::string backend;

    Qwen3Session() = default;
    ~Qwen3Session();
    Qwen3Session(const Qwen3Session&) = delete;
    Qwen3Session& operator=(const Qwen3Session&) = delete;
};

bool qwen3_open(const TtsRequest& req, Qwen3Session& out, std::string& error);
bool qwen3_synthesize(Qwen3Session& session, const std::string& text, TtsResult& out,
                      std::string& error);

} // namespace iris::tts
