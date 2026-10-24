#include "iris/tts/tensor.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace iris::tts {

namespace {

/// 6-bit 的 K-quant 子块尺度（ggml get_scale_min_k4）。
void scale_min_k4(int j, const std::uint8_t* q, std::uint8_t& d, std::uint8_t& m) {
    if (j < 4) {
        d = q[j] & 63;
        m = q[j + 4] & 63;
    } else {
        d = static_cast<std::uint8_t>((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        m = static_cast<std::uint8_t>((q[j + 4] >> 4) | ((q[j] >> 6) << 4));
    }
}

template <typename Fn>
bool per_block(const std::uint8_t* src, std::size_t n_elems, std::uint32_t type,
               std::vector<float>& out, Fn&& fn) {
    const std::uint32_t elems = gguf::block_elems(type);
    const std::size_t bytes = gguf::block_bytes(type);
    if (elems == 0 || bytes == 0) return false;
    if (n_elems % elems != 0) return false;

    const std::size_t n_blocks = n_elems / elems;
    out.resize(n_elems);
    for (std::size_t b = 0; b < n_blocks; ++b) {
        fn(src + b * bytes, out.data() + b * elems, elems);
    }
    return true;
}

} // namespace

std::size_t Tensor::nelements() const {
    std::size_t n = 1;
    for (auto d : dims) n *= static_cast<std::size_t>(d);
    return n;
}

float f16_to_f32(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    const int exp = (h >> 10) & 0x1F;
    const std::uint32_t mant = h & 0x3FFu;

    std::uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else { // 次正规数
            std::uint32_t m = mant;
            int e = -14;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                --e;
            }
            m &= 0x3FFu;
            bits = sign | static_cast<std::uint32_t>((e + 127) << 23) | (m << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | static_cast<std::uint32_t>((exp - 15 + 127) << 23) | (mant << 13);
    }

    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

bool dequantize(const std::uint8_t* src, std::size_t src_bytes, std::uint32_t type,
                std::size_t n_elems, std::vector<float>& out, std::string& error) {
    const std::size_t bytes = gguf::block_bytes(type);
    const std::uint32_t elems = gguf::block_elems(type);
    if (bytes == 0 || elems == 0) {
        error = std::string("不支持的张量类型：") + gguf::type_name(type);
        return false;
    }
    if (n_elems % elems != 0) {
        error = "元素数与量化块大小不整除";
        return false;
    }
    const std::size_t needed = n_elems / elems * bytes;
    if (src_bytes < needed) {
        error = "张量数据被截断";
        return false;
    }

    switch (type) {
        case 0: { // F32
            out.resize(n_elems);
            std::memcpy(out.data(), src, n_elems * sizeof(float));
            return true;
        }
        case 1: { // F16
            out.resize(n_elems);
            for (std::size_t i = 0; i < n_elems; ++i) {
                std::uint16_t h = 0;
                std::memcpy(&h, src + i * 2, 2);
                out[i] = f16_to_f32(h);
            }
            return true;
        }
        case 8: { // Q8_0: f16 d + 32 × int8
            const bool ok = per_block(src, n_elems, type, out,
                                      [](const std::uint8_t* blk, float* dst, std::uint32_t) {
                                          std::uint16_t h = 0;
                                          std::memcpy(&h, blk, 2);
                                          const float d = f16_to_f32(h);
                                          const auto* q = reinterpret_cast<const std::int8_t*>(blk + 2);
                                          for (int i = 0; i < 32; ++i) dst[i] = d * q[i];
                                      });
            if (!ok) error = "Q8_0 数据不完整";
            return ok;
        }
        case 2: { // Q4_0: f16 d + 16 B（低半字节=前 16 个，高半字节=后 16 个）
            const bool ok = per_block(src, n_elems, type, out,
                                      [](const std::uint8_t* blk, float* dst, std::uint32_t) {
                                          std::uint16_t h = 0;
                                          std::memcpy(&h, blk, 2);
                                          const float d = f16_to_f32(h);
                                          for (int i = 0; i < 16; ++i) {
                                              const std::uint8_t b = blk[2 + i];
                                              dst[i] = d * (static_cast<int>(b & 0xF) - 8);
                                              dst[i + 16] = d * (static_cast<int>(b >> 4) - 8);
                                          }
                                      });
            if (!ok) error = "Q4_0 数据不完整";
            return ok;
        }
        case 12: { // Q4_K: f16 d + f16 dmin + 12 B scales + 128 B quants（256 元素/块）
            const bool ok = per_block(src, n_elems, type, out,
                                      [](const std::uint8_t* blk, float* dst, std::uint32_t) {
                                          std::uint16_t hd = 0, hm = 0;
                                          std::memcpy(&hd, blk, 2);
                                          std::memcpy(&hm, blk + 2, 2);
                                          const float d = f16_to_f32(hd);
                                          const float dmin = f16_to_f32(hm);
                                          const std::uint8_t* scales = blk + 4;
                                          const std::uint8_t* q = blk + 16;
                                          int is = 0;
                                          for (int j = 0; j < 256; j += 64) {
                                              std::uint8_t sc = 0, m = 0;
                                              scale_min_k4(is, scales, sc, m);
                                              const float d1 = d * sc, m1 = dmin * m;
                                              scale_min_k4(is + 1, scales, sc, m);
                                              const float d2 = d * sc, m2 = dmin * m;
                                              for (int l = 0; l < 32; ++l)
                                                  dst[j + l] = d1 * (q[l] & 0xF) - m1;
                                              for (int l = 0; l < 32; ++l)
                                                  dst[j + l + 32] = d2 * (q[l] >> 4) - m2;
                                              q += 32;
                                              is += 2;
                                          }
                                      });
            if (!ok) error = "Q4_K 数据不完整";
            return ok;
        }
        default:
            error = std::string("暂不支持反量化的类型：") + gguf::type_name(type);
            return false;
    }
}

bool WeightFile::open(const std::string& path, std::string& error) {
    path_ = path;
    return gguf::load(path, index_, error);
}

bool WeightFile::load(const std::string& name, Tensor& out, std::string& error) {
    const gguf::TensorInfo* info = index_.tensor(name);
    if (!info) {
        error = "模型里没有张量：" + name;
        return false;
    }

    std::size_t n_elems = 1;
    for (auto d : info->dims) n_elems *= static_cast<std::size_t>(d);
    const std::size_t blocks = n_elems / std::max<std::uint32_t>(1, gguf::block_elems(info->type));
    const std::size_t bytes = blocks * gguf::block_bytes(info->type);
    if (bytes == 0) {
        error = std::string("张量类型不支持：") + name + " (" + gguf::type_name(info->type) + ")";
        return false;
    }

    std::ifstream f(path_, std::ios::binary);
    if (!f) {
        error = "无法打开权重文件：" + path_;
        return false;
    }
    const auto at = static_cast<std::streamoff>(index_.data_offset + info->offset);
    f.seekg(at);
    if (!f) {
        error = "张量偏移越界：" + name;
        return false;
    }
    std::vector<std::uint8_t> raw(bytes);
    if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(bytes))) {
        error = "张量数据读取失败：" + name;
        return false;
    }

    out.name = name;
    out.dims = info->dims;
    if (!dequantize(raw.data(), raw.size(), info->type, n_elems, out.data, error)) {
        error = name + "：" + error;
        return false;
    }
    return true;
}

bool WeightFile::try_load(const std::string& name, Tensor& out) {
    std::string ignored;
    if (!has(name)) return false;
    return load(name, out, ignored);
}

} // namespace iris::tts
