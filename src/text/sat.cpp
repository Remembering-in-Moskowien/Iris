#include "iris/text/sat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/infra/log.hpp"
#include "iris/infra/threads.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace iris::text {

namespace {

constexpr int kMaxWindowTokens = 512;  ///< 含 <s>/</s>
constexpr int kWindowStride = 256;     ///< content token 步进
constexpr const char* kMetaspace = "\xE2\x96\x81"; // ▁
constexpr float kDefaultThreshold = 0.5f;

/// UTF-8 拆字符（返回每段的字节长度）。
std::vector<std::size_t> utf8_char_lengths(const std::string& s) {
    std::vector<std::size_t> lens;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > s.size()) len = 1;
        lens.push_back(len);
        i += len;
    }
    return lens;
}

} // namespace

bool SatTokenizer::load(const std::string& tokenizer_json, std::string& error) {
    std::ifstream f(tokenizer_json);
    if (!f) {
        error = "无法打开 tokenizer.json：" + tokenizer_json;
        return false;
    }
    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        error = std::string("tokenizer.json 解析失败：") + e.what();
        return false;
    }

    const auto& model = doc.at("model");
    unk_ = model.value("unk_id", 3);
    const auto& vocab = model.at("vocab");
    ids_.reserve(vocab.size());
    scores_.reserve(vocab.size());
    for (const auto& entry : vocab) {
        const std::string piece = entry.at(0).get<std::string>();
        const float score = entry.at(1).get<float>();
        const int id = static_cast<int>(ids_.size());
        ids_.emplace(piece, id);
        scores_.emplace(piece, score);
    }
    // added_tokens 里带 <s>/</s> 的 id（找不到就用 0/2）
    if (doc.contains("added_tokens")) {
        for (const auto& tok : doc["added_tokens"]) {
            const std::string content = tok.value("content", "");
            const int id = tok.value("id", -1);
            if (content == "<s>") bos_ = id >= 0 ? id : bos_;
            if (content == "</s>") eos_ = id >= 0 ? id : eos_;
        }
    }
    spdlog::info("sat: 分词器就绪（{} 词表，unk={}）", ids_.size(), unk_);
    return true;
}

std::vector<SatToken> SatTokenizer::encode(const std::string& text) const {
    std::vector<SatToken> out;
    if (ids_.empty() || text.empty()) return out;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        // 取一个「词」（按 ASCII 空格切）
        std::size_t space = text.find(' ', pos);
        const bool last = (space == std::string::npos);
        const std::size_t word_end = last ? text.size() : space;
        const std::string word = text.substr(pos, word_end - pos);
        const std::size_t word_start = pos;

        if (!word.empty()) {
            // 初始符号：metaspace + 每个字符
            std::vector<std::string> symbols;
            symbols.emplace_back(kMetaspace);
            std::vector<std::size_t> offsets; // 每个符号对应的原文字节偏移（metaspace 为 npos）
            offsets.push_back(std::string::npos);
            std::size_t base = word_start;
            for (const auto len : utf8_char_lengths(word)) {
                symbols.push_back(word.substr(base - word_start, len));
                offsets.push_back(base);
                base += len;
            }

            // BPE：反复合并「合并后在词表里且分数最高」的相邻对
            while (symbols.size() > 1) {
                float best = -1e30f;
                std::size_t best_at = std::string::npos;
                std::string best_piece;
                for (std::size_t i = 0; i + 1 < symbols.size(); ++i) {
                    const std::string merged = symbols[i] + symbols[i + 1];
                    const auto it = scores_.find(merged);
                    if (it == scores_.end()) continue;
                    if (it->second > best) {
                        best = it->second;
                        best_at = i;
                        best_piece = merged;
                    }
                }
                if (best_at == std::string::npos) break;
                symbols[best_at] = best_piece;
                offsets[best_at] = offsets[best_at] == std::string::npos ? offsets[best_at + 1]
                                                                         : offsets[best_at];
                symbols.erase(symbols.begin() + static_cast<std::ptrdiff_t>(best_at) + 1);
                offsets.erase(offsets.begin() + static_cast<std::ptrdiff_t>(best_at) + 1);
            }

            // 落地成 token（含 id 与字符偏移）
            std::size_t cursor = word_start;
            for (std::size_t i = 0; i < symbols.size(); ++i) {
                SatToken tok;
                tok.piece = symbols[i];
                const auto it = ids_.find(tok.piece);
                tok.id = it == ids_.end() ? unk_ : it->second;
                tok.start = cursor;
                // metaspace 不占原文字符
                std::size_t chars = 0;
                const std::size_t meta_len = std::strlen(kMetaspace);
                if (tok.piece.size() >= meta_len && tok.piece.compare(0, meta_len, kMetaspace) == 0) {
                    chars = utf8_char_lengths(tok.piece.substr(meta_len)).size();
                } else {
                    chars = utf8_char_lengths(tok.piece).size();
                }
                // 字符数 → 字节数
                std::size_t bytes = 0;
                std::size_t seen = 0;
                for (std::size_t k = cursor; k < word_end && seen < chars; ++seen) {
                    const auto c = static_cast<unsigned char>(text[k]);
                    std::size_t len = 1;
                    if ((c & 0xE0) == 0xC0) {
                        len = 2;
                    } else if ((c & 0xF0) == 0xE0) {
                        len = 3;
                    } else if ((c & 0xF8) == 0xF0) {
                        len = 4;
                    }
                    bytes += len;
                    k += len;
                }
                tok.end = cursor + bytes;
                cursor = tok.end;
                out.push_back(std::move(tok));
            }
        }

        if (last) break;
        pos = word_end + 1;
    }
    return out;
}

SatSegmenter::~SatSegmenter() {
#if IRIS_HAS_ONNX
    delete static_cast<Ort::Session*>(session_);
    delete static_cast<Ort::Env*>(env_);
#endif
}

bool SatSegmenter::open(const std::string& model_dir, std::string& error) {
#if !IRIS_HAS_ONNX
    (void)model_dir;
    error = "sat: Iris 未以 onnxruntime 构建（tools/runtimes/onnx 缺失）";
    return false;
#else
    namespace fs = std::filesystem;
    if (!fs::is_directory(model_dir)) {
        error = "sat: 模型目录不存在：" + model_dir;
        return false;
    }
    std::string model_path;
    for (const char* name : {"model_optimized.onnx", "model.onnx"}) {
        const auto p = fs::path(model_dir) / name;
        std::error_code ec;
        if (fs::exists(p, ec)) {
            model_path = p.string();
            break;
        }
    }
    if (model_path.empty()) {
        error = "sat: 目录里没有 model_optimized.onnx / model.onnx：" + model_dir;
        return false;
    }
    if (!tokenizer_.load((fs::path(model_dir) / "tokenizer.json").string(), error)) return false;

    n_threads_ = infra::resolve_threads(0);
    try {
        auto* env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "iris-sat");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(n_threads_);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        std::wstring wpath(model_path.begin(), model_path.end());
        auto* session = new Ort::Session(*env, wpath.c_str(), opts);
        env_ = env;
        session_ = session;
        model_path_ = model_path;
    } catch (const Ort::Exception& e) {
        error = std::string("sat: ONNX 加载失败：") + e.what();
        return false;
    }

    ready_ = true;
    spdlog::info("sat: 模型就绪（{}，{} 线程）", fs::path(model_path).filename().string(), n_threads_);
    return true;
#endif
}

std::vector<std::size_t> SatSegmenter::boundaries_to_offsets(const std::vector<SatToken>& tokens,
                                                             const std::vector<float>& probs,
                                                             float threshold) {
    std::vector<std::size_t> points;
    const std::size_t n = std::min(tokens.size(), probs.size());
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (probs[i] <= threshold) continue;
        const std::size_t at = tokens[i + 1].start;
        // 太短的片段不切（避免把词/短语切碎）
        if (at < 2) continue;
        if (!points.empty() && at <= points.back() + 1) continue;
        if (tokens.back().end > at && tokens.back().end - at < 2) continue;
        points.push_back(at);
    }
    return points;
}

bool SatSegmenter::split_points(const std::string& text, float threshold,
                                std::vector<std::size_t>& points, std::string& error) {
    points.clear();
#if !IRIS_HAS_ONNX
    (void)text;
    (void)threshold;
    error = "sat: 未构建 onnxruntime";
    return false;
#else
    if (!ready_) {
        error = "sat: 模型未加载";
        return false;
    }
    if (threshold <= 0.0f || threshold >= 1.0f) threshold = kDefaultThreshold;

    const auto tokens = tokenizer_.encode(text);
    if (tokens.size() < 3) return true; // 太短，不需要切

    // ids：[<s>] + content + [</s>]
    std::vector<std::int64_t> ids;
    ids.reserve(tokens.size() + 2);
    ids.push_back(tokenizer_.bos_id());
    for (const auto& t : tokens) ids.push_back(t.id);
    ids.push_back(tokenizer_.eos_id());

    const int content = static_cast<int>(tokens.size());
    std::vector<double> sums(static_cast<std::size_t>(content), 0.0);
    std::vector<int> counts(static_cast<std::size_t>(content), 0);

    Ort::Env& env = *static_cast<Ort::Env*>(env_);
    Ort::Session& session = *static_cast<Ort::Session*>(session_);
    (void)env;

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // 输入/输出类型从模型读：input 0 是 input_ids(int64)，input 1 才是 attention_mask；
    // 不同导出可能是 float32 或 float16 的 mask/logits。
    const auto mask_type =
        session.GetInputCount() > 1
            ? session.GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetElementType()
            : ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    const auto output_types =
        session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType();

    // 输入名只取一次，且用局部 string 存储（c_str() 必须活到 Run 调用结束）。
    std::vector<std::string> in_name_storage;
    {
        Ort::AllocatorWithDefaultOptions alloc;
        for (std::size_t i = 0; i < session.GetInputCount(); ++i) {
            in_name_storage.push_back(session.GetInputNameAllocated(i, alloc).get());
        }
    }
    std::vector<const char*> in_names;
    in_names.reserve(in_name_storage.size());
    for (const auto& name : in_name_storage) in_names.push_back(name.c_str());

    for (int start = 0; start < content; start += kWindowStride) {
        const int length = std::min(kMaxWindowTokens - 2, content - start);
        if (length <= 0) break;

        std::vector<std::int64_t> window;
        window.reserve(static_cast<std::size_t>(length) + 2);
        window.push_back(ids.front());
        window.insert(window.end(), ids.begin() + 1 + start, ids.begin() + 1 + start + length);
        window.push_back(ids.back());

        const std::vector<int64_t> shape = {1, static_cast<int64_t>(window.size())};
        auto ids_tensor = Ort::Value::CreateTensor<std::int64_t>(
            mem, window.data(), window.size(), shape.data(), shape.size());

        // attention mask：按模型要求的数据类型构造全 1
        std::vector<std::uint16_t> mask16(window.size(), 0x3C00); // float16 1.0
        std::vector<float> mask32(window.size(), 1.0f);
        Ort::Value mask_tensor{nullptr};
        if (mask_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            mask_tensor = Ort::Value::CreateTensor(mem, mask16.data(), mask16.size() * sizeof(std::uint16_t),
                                                   shape.data(), shape.size(),
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16);
        } else {
            mask_tensor = Ort::Value::CreateTensor(mem, mask32.data(), mask32.size() * sizeof(float),
                                                   shape.data(), shape.size(),
                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        }

        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(ids_tensor));
        inputs.push_back(std::move(mask_tensor));

        const char* out_name = "logits";
        std::vector<Ort::Value> outputs;
        try {
            outputs = session.Run(Ort::RunOptions{}, in_names.data(), inputs.data(), inputs.size(),
                                  &out_name, 1);
        } catch (const Ort::Exception& e) {
            error = std::string("sat: 推理失败：") + e.what();
            return false;
        }
        if (outputs.empty()) {
            error = "sat: 推理没有输出";
            return false;
        }

        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const auto out_shape = info.GetShape();
        const std::size_t total = info.GetElementCount();
        std::vector<float> logits(total);
        if (output_types == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            const auto* raw = outputs[0].GetTensorData<std::uint16_t>();
            for (std::size_t i = 0; i < total; ++i) {
                // half → float
                const std::uint32_t sign = static_cast<std::uint32_t>(raw[i] & 0x8000u) << 16;
                const int exp = (raw[i] >> 10) & 0x1F;
                const std::uint32_t mant = raw[i] & 0x3FFu;
                std::uint32_t bits = 0;
                if (exp == 0) {
                    bits = sign;
                } else if (exp == 31) {
                    bits = sign | 0x7F800000u | (mant << 13);
                } else {
                    bits = sign | (static_cast<std::uint32_t>(exp - 15 + 127) << 23) | (mant << 13);
                }
                float f = 0.0f;
                std::memcpy(&f, &bits, sizeof f);
                logits[i] = f;
            }
        } else {
            const auto* raw = outputs[0].GetTensorData<float>();
            std::copy(raw, raw + total, logits.begin());
        }
        (void)out_shape;

        // 概率：sigmoid(logit[i+1]) 对应 content token i
        for (int i = 0; i < length; ++i) {
            const auto idx = static_cast<std::size_t>(i + 1);
            if (idx >= logits.size()) break;
            const double p = 1.0 / (1.0 + std::exp(-static_cast<double>(logits[idx])));
            sums[static_cast<std::size_t>(start + i)] += p;
            ++counts[static_cast<std::size_t>(start + i)];
        }
    }

    std::vector<float> probs(static_cast<std::size_t>(content), 0.0f);
    for (int i = 0; i < content; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        probs[idx] = counts[idx] > 0 ? static_cast<float>(sums[idx] / counts[idx]) : 0.0f;
    }

    points = boundaries_to_offsets(tokens, probs, threshold);
    spdlog::info("sat: {} token → {} 个切分点", tokens.size(), points.size());
    return true;
#endif
}

} // namespace iris::text
