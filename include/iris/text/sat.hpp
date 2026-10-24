#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace iris::text {

// ---------------------------------------------------------------------------
// SaT 分句（Segment Any Text）：XLM-R 词表 + ONNX token 分类器。
//
// 对应 C# 的 SaTSplitStrategy / XlmRBpeTokenizer：
//   - 分词：XLM-R 的 SentencePiece BPE（词表与分数取自 tokenizer.json）；
//   - 推理：input_ids(int64) + attention_mask(float16/float32) → 每个 token 一个 logit，
//     sigmoid 得到「该 token 之后是句界」的概率；
//   - 窗口：最多 512 token（含 <s>/</s>），步进 256，重叠区概率取平均；
//   - 判定：概率 > threshold（默认 0.5）处切分，只切在词边界上。
// ---------------------------------------------------------------------------

/// 一个分词结果（含在原文中的字符偏移）。
struct SatToken {
    std::string piece;
    int id = 0;
    std::size_t start = 0;
    std::size_t end = 0;
};

/// XLM-R SentencePiece BPE 分词器。
class SatTokenizer {
public:
    bool load(const std::string& tokenizer_json, std::string& error);
    [[nodiscard]] bool is_loaded() const { return !ids_.empty(); }

    /// 按空格切词 → 每词加 metaspace 前缀（▁）→ BPE 合并（按分数取最优）。
    std::vector<SatToken> encode(const std::string& text) const;

    [[nodiscard]] int unk_id() const { return unk_; }
    [[nodiscard]] int bos_id() const { return bos_; }
    [[nodiscard]] int eos_id() const { return eos_; }
    [[nodiscard]] std::size_t vocab_size() const { return ids_.size(); }

private:
    std::unordered_map<std::string, int> ids_;
    std::unordered_map<std::string, float> scores_;
    int unk_ = 3;
    int bos_ = 0;
    int eos_ = 2;
};

/// SaT 分句器。
class SatSegmenter {
public:
    SatSegmenter() = default;
    ~SatSegmenter();
    SatSegmenter(const SatSegmenter&) = delete;
    SatSegmenter& operator=(const SatSegmenter&) = delete;

    /// model_dir 需含 tokenizer.json 与 model_optimized.onnx / model.onnx。
    bool open(const std::string& model_dir, std::string& error);
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] int n_threads() const { return n_threads_; }

    /// 计算切分点（字符偏移，升序，不含 0 与末尾）。
    bool split_points(const std::string& text, float threshold, std::vector<std::size_t>& points,
                      std::string& error);

    /// 便于单测：给定 token 与每个 token 的边界概率，算出切分偏移。
    static std::vector<std::size_t> boundaries_to_offsets(const std::vector<SatToken>& tokens,
                                                          const std::vector<float>& probs,
                                                          float threshold);

private:
    bool ready_ = false;
    int n_threads_ = 0;
    SatTokenizer tokenizer_;
    void* env_ = nullptr;
    void* session_ = nullptr;
    std::string model_path_;
};

} // namespace iris::text
