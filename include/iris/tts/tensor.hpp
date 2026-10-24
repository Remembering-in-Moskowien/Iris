#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/tts/gguf.hpp"

namespace iris::tts {

// ---------------------------------------------------------------------------
// 张量层：把 GGUF 里的量化权重读成 float32，供自研推理使用。
//
// 布局约定（与 ggml 一致）：dims[0] 是内层（连续）维，dims[1] 是外层。
// 一个 [cols, rows] 的线性层权重按行连续存放：W[row * cols + col]，
// 正好可以直接做 y[row] = dot(W + row*cols, x)。
// ---------------------------------------------------------------------------

/// 反量化后的张量（float32）。
struct Tensor {
    std::string name;
    std::vector<std::uint64_t> dims; ///< ggml 顺序：dims[0] 最内层
    std::vector<float> data;         ///< 行主序，长度 = 各维乘积

    [[nodiscard]] std::size_t nelements() const;
    [[nodiscard]] std::size_t cols() const { return dims.empty() ? 0 : dims[0]; }
    [[nodiscard]] std::size_t rows() const { return dims.size() < 2 ? 1 : dims[1]; }
    [[nodiscard]] bool empty() const { return data.empty(); }
};

/// 反量化一段连续数据：n_elems 必须能被该类型的块大小整除。
bool dequantize(const std::uint8_t* src, std::size_t src_bytes, std::uint32_t type,
                std::size_t n_elems, std::vector<float>& out, std::string& error);

/// IEEE 半精度 → float32。
float f16_to_f32(std::uint16_t h);

/// 单个权重文件：解析目录、按需读取并反量化张量（不把整份权重读进内存）。
class WeightFile {
public:
    bool open(const std::string& path, std::string& error);

    [[nodiscard]] const gguf::File& index() const { return index_; }
    [[nodiscard]] bool has(const std::string& name) const { return index_.tensor(name) != nullptr; }

    /// 读取并反量化一个张量。失败时 error 说明原因（找不到/类型不支持/文件损坏）。
    bool load(const std::string& name, Tensor& out, std::string& error);
    /// 同上，但找不到时返回 false 且不视为错误（方便可选张量）。
    bool try_load(const std::string& name, Tensor& out);

private:
    std::string path_;
    gguf::File index_;
};

} // namespace iris::tts
