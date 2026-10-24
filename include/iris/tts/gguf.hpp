#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace iris::tts::gguf {

// ---------------------------------------------------------------------------
// GGUF 读取（自研 Qwen3-TTS 运行时的权重入口）。
//
// 只做「元数据 + 张量表」的解析：够用来校验模型结构、规划张量布局；
// 张量数据的反量化/前向计算由运行时按需读取（见 load_tensor 的后续实现）。
// 参考规范：ggml 的 GGUF v2/v3。
// ---------------------------------------------------------------------------

/// 元数据值类型（与 GGUF 规范一致）。
enum class ValueType : std::uint32_t {
    U8 = 0, I8 = 1, U16 = 2, I16 = 3, U32 = 4, I32 = 5, F32 = 6,
    Bool = 7, String = 8, Array = 9, U64 = 10, I64 = 11, F64 = 12,
};

/// 一条元数据（数组只记录长度与元素类型，不展开大数组）。
struct MetaValue {
    ValueType type = ValueType::U32;
    std::int64_t as_int = 0;
    double as_float = 0.0;
    std::string as_string;
    ValueType array_type = ValueType::U32;
    std::uint64_t array_len = 0;
};

/// 张量表中一项。
struct TensorInfo {
    std::string name;
    std::vector<std::uint64_t> dims;
    std::uint32_t type = 0;   ///< ggml 类型（0=F32, 8=Q8_0, 12=Q4_K …）
    std::uint64_t offset = 0; ///< 相对张量数据区起始的字节偏移
};

/// 解析后的 GGUF 文件目录。
struct File {
    std::uint32_t version = 0;
    std::uint64_t tensor_count = 0;
    std::vector<std::pair<std::string, MetaValue>> meta;
    std::vector<TensorInfo> tensors;

    [[nodiscard]] const MetaValue* find(const std::string& key) const;
    [[nodiscard]] std::string get_string(const std::string& key,
                                         const std::string& fallback = {}) const;
    [[nodiscard]] std::int64_t get_int(const std::string& key, std::int64_t fallback = 0) const;
    [[nodiscard]] double get_float(const std::string& key, double fallback = 0.0) const;
    /// 名字里包含 needle 的张量（用于按前缀统计 transformer 层）。
    [[nodiscard]] std::vector<const TensorInfo*> find_tensors(const std::string& needle) const;
};

/// 读取 GGUF 目录。失败时 error 说明原因（magic/版本/截断）。
bool load(const std::string& path, File& out, std::string& error);

/// ggml 类型名（"F32" / "Q8_0" / "Q4_K" …），未知返回 "unknown"。
const char* type_name(std::uint32_t type);

/// 每元素字节数估算（用于校验文件大小是否自洽）；未知类型返回 0。
double bytes_per_element(std::uint32_t type);

} // namespace iris::tts::gguf
