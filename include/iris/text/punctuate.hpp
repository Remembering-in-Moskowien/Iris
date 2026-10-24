#pragma once

#include <string>

namespace iris::text {

// ---------------------------------------------------------------------------
// 标点恢复（punctuation restoration）。
//
// 走 CrispASR 的独立标点 API：`crispasr_punc_init/process`。
// 模型参数与 CLI 的 --punc-model 一致：
//   别名 auto | firered | fullstop | punctuate-all | pcs（首次使用时库内自动下载）
//   或某个 .gguf 路径（按 general.architecture 分派 fireredpunc / pcs）。
//
// 中文 ASR（Paraformer / SenseVoice 关掉 ITN 时）输出没有标点，字幕可读性差，
// 这一步就是补它；英文输出通常自带标点，可按需关闭。
// ---------------------------------------------------------------------------

/// 在本机找默认标点模型（~/.cache/crispasr、models/punc、models/*）；
/// 找不到返回空串（调用方可退回 "auto" 让库自行下载）。
std::string default_punc_model();

class Punctuator {
public:
    Punctuator() = default;
    ~Punctuator();
    Punctuator(const Punctuator&) = delete;
    Punctuator& operator=(const Punctuator&) = delete;

    /// model 为空时用 "auto"。
    bool open(const std::string& model, std::string& error);
    [[nodiscard]] bool is_open() const { return ctx_ != nullptr; }

    /// 给一句话补标点；失败或未打开时原样返回。
    std::string punctuate(const std::string& text);

private:
    void* ctx_ = nullptr; ///< CrispASR punc handle
};

} // namespace iris::text
