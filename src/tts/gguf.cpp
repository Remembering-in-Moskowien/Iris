#include "iris/tts/gguf.hpp"

#include <cstring>
#include <fstream>
#include <string>

namespace iris::tts::gguf {

namespace {

constexpr std::uint32_t kMagic = 0x46554747; // "GGUF" little-endian
constexpr std::uint64_t kMaxStringBytes = 1ull << 20;
constexpr std::uint64_t kMaxDims = 8;

/// 顺序读取小端字段的辅助。
struct Reader {
    std::ifstream& f;

    bool read(void* dst, std::size_t n) { return static_cast<bool>(f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n))); }

    bool u32(std::uint32_t& v) { return read(&v, sizeof v); }
    bool u64(std::uint64_t& v) { return read(&v, sizeof v); }
    bool i64(std::int64_t& v) { return read(&v, sizeof v); }
    bool f64(double& v) { return read(&v, sizeof v); }
    bool u8(std::uint8_t& v) { return read(&v, sizeof v); }

    bool str(std::string& out) {
        std::uint64_t len = 0;
        if (!u64(len) || len > kMaxStringBytes) return false;
        out.resize(static_cast<std::size_t>(len));
        return len == 0 || read(out.data(), static_cast<std::size_t>(len));
    }
};

/// 跳过一个值（数组只前移文件指针）。
bool skip_value(Reader& r, ValueType type) {
    switch (type) {
        case ValueType::U8: case ValueType::I8: case ValueType::Bool: {
            std::uint8_t v = 0;
            return r.u8(v);
        }
        case ValueType::U16: case ValueType::I16: {
            std::uint16_t v = 0;
            return r.read(&v, sizeof v);
        }
        case ValueType::U32: case ValueType::I32: case ValueType::F32: {
            std::uint32_t v = 0;
            return r.u32(v);
        }
        case ValueType::U64: case ValueType::I64: case ValueType::F64: {
            std::uint64_t v = 0;
            return r.u64(v);
        }
        case ValueType::String: {
            std::string s;
            return r.str(s);
        }
        case ValueType::Array: {
            std::uint32_t elem_type = 0;
            std::uint64_t count = 0;
            if (!r.u32(elem_type) || !r.u64(count)) return false;
            const auto t = static_cast<ValueType>(elem_type);
            // 标量数组可整体跳过；字符串数组只能逐个读。
            if (t != ValueType::String && t != ValueType::Array) {
                static const std::size_t sizes[] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
                const auto idx = static_cast<std::size_t>(t);
                if (idx >= sizeof(sizes) / sizeof(sizes[0]) || sizes[idx] == 0) return false;
                return r.f.seekg(static_cast<std::streamoff>(count * sizes[idx]), std::ios::cur).good();
            }
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string s;
                if (t == ValueType::String) {
                    if (!r.str(s)) return false;
                } else if (!skip_value(r, t)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool read_value(Reader& r, ValueType type, MetaValue& v) {
    v.type = type;
    switch (type) {
        case ValueType::U8: case ValueType::I8: case ValueType::Bool: {
            std::uint8_t x = 0;
            if (!r.u8(x)) return false;
            v.as_int = type == ValueType::I8 ? static_cast<std::int8_t>(x) : static_cast<std::int64_t>(x);
            return true;
        }
        case ValueType::U16: case ValueType::I16: {
            std::uint16_t x = 0;
            if (!r.read(&x, sizeof x)) return false;
            v.as_int = type == ValueType::I16 ? static_cast<std::int16_t>(x) : static_cast<std::int64_t>(x);
            return true;
        }
        case ValueType::U32: case ValueType::I32: {
            std::uint32_t x = 0;
            if (!r.u32(x)) return false;
            v.as_int = type == ValueType::I32 ? static_cast<std::int32_t>(x) : static_cast<std::int64_t>(x);
            return true;
        }
        case ValueType::U64: case ValueType::I64: {
            std::uint64_t x = 0;
            if (!r.u64(x)) return false;
            v.as_int = static_cast<std::int64_t>(x);
            return true;
        }
        case ValueType::F32: {
            float x = 0.0f;
            if (!r.read(&x, sizeof x)) return false;
            v.as_float = x;
            return true;
        }
        case ValueType::F64: {
            double x = 0.0;
            if (!r.f64(x)) return false;
            v.as_float = x;
            return true;
        }
        case ValueType::String:
            return r.str(v.as_string);
        case ValueType::Array: {
            std::uint32_t elem_type = 0;
            std::uint64_t count = 0;
            if (!r.u32(elem_type) || !r.u64(count)) return false;
            v.array_type = static_cast<ValueType>(elem_type);
            v.array_len = count;
            for (std::uint64_t i = 0; i < count; ++i) {
                if (!skip_value(r, v.array_type)) return false;
            }
            return true;
        }
    }
    return false;
}

} // namespace

const MetaValue* File::find(const std::string& key) const {
    for (const auto& [k, v] : meta) {
        if (k == key) return &v;
    }
    return nullptr;
}

std::string File::get_string(const std::string& key, const std::string& fallback) const {
    const MetaValue* v = find(key);
    return v && v->type == ValueType::String ? v->as_string : fallback;
}

std::int64_t File::get_int(const std::string& key, std::int64_t fallback) const {
    const MetaValue* v = find(key);
    if (!v) return fallback;
    if (v->type == ValueType::F32 || v->type == ValueType::F64) {
        return static_cast<std::int64_t>(v->as_float);
    }
    return v->as_int;
}

double File::get_float(const std::string& key, double fallback) const {
    const MetaValue* v = find(key);
    if (!v) return fallback;
    if (v->type == ValueType::F32 || v->type == ValueType::F64) return v->as_float;
    return static_cast<double>(v->as_int);
}

std::vector<const TensorInfo*> File::find_tensors(const std::string& needle) const {
    std::vector<const TensorInfo*> hits;
    for (const auto& t : tensors) {
        if (t.name.find(needle) != std::string::npos) hits.push_back(&t);
    }
    return hits;
}

bool load(const std::string& path, File& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开 GGUF：" + path;
        return false;
    }
    Reader r{f};
    out = File{};

    std::uint32_t magic = 0;
    if (!r.u32(magic)) {
        error = "GGUF 头不完整：" + path;
        return false;
    }
    if (magic != kMagic) {
        error = "不是 GGUF 文件（magic 不匹配）：" + path;
        return false;
    }
    if (!r.u32(out.version) || out.version < 2 || out.version > 3) {
        error = "不支持的 GGUF 版本";
        return false;
    }
    std::uint64_t kv_count = 0;
    if (!r.u64(out.tensor_count) || !r.u64(kv_count)) {
        error = "GGUF 头不完整";
        return false;
    }

    out.meta.reserve(static_cast<std::size_t>(kv_count));
    for (std::uint64_t i = 0; i < kv_count; ++i) {
        std::string key;
        std::uint32_t type = 0;
        if (!r.str(key) || !r.u32(type)) {
            error = "GGUF 元数据损坏（第 " + std::to_string(i) + " 项）";
            return false;
        }
        MetaValue value;
        if (!read_value(r, static_cast<ValueType>(type), value)) {
            error = "GGUF 元数据值损坏：" + key;
            return false;
        }
        out.meta.emplace_back(std::move(key), std::move(value));
    }

    out.tensors.resize(static_cast<std::size_t>(out.tensor_count));
    for (auto& t : out.tensors) {
        std::uint32_t n_dims = 0;
        if (!r.str(t.name) || !r.u32(n_dims)) {
            error = "GGUF 张量表损坏";
            return false;
        }
        if (n_dims > kMaxDims) {
            error = "GGUF 张量维数异常：" + t.name;
            return false;
        }
        t.dims.resize(n_dims);
        for (auto& d : t.dims) {
            if (!r.u64(d)) {
                error = "GGUF 张量维度损坏：" + t.name;
                return false;
            }
        }
        if (!r.u32(t.type) || !r.u64(t.offset)) {
            error = "GGUF 张量描述损坏：" + t.name;
            return false;
        }
    }
    return true;
}

const char* type_name(std::uint32_t type) {
    switch (type) {
        case 0: return "F32";
        case 1: return "F16";
        case 2: return "Q4_0";
        case 3: return "Q4_1";
        case 6: return "Q5_0";
        case 7: return "Q5_1";
        case 8: return "Q8_0";
        case 9: return "Q8_1";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 15: return "Q8_K";
        case 16: return "IQ2_XXS";
        case 30: return "BF16";
        default: return "unknown";
    }
}

double bytes_per_element(std::uint32_t type) {
    switch (type) {
        case 0: return 4.0;    // F32
        case 1: return 2.0;    // F16
        case 30: return 2.0;   // BF16
        case 2: case 3: return 0.5625;          // Q4_0/Q4_1（4.5 bit）
        case 6: case 7: return 0.6875;          // Q5_0/Q5_1（5.5 bit）
        case 8: case 9: return 1.0625;          // Q8_0/Q8_1（8.5 bit）
        case 10: return 0.3125;                 // Q2_K
        case 11: return 0.4375;                 // Q3_K
        case 12: return 0.5625;                 // Q4_K
        case 13: return 0.6875;                 // Q5_K
        case 14: return 0.8125;                 // Q6_K
        default: return 0.0;
    }
}

} // namespace iris::tts::gguf
