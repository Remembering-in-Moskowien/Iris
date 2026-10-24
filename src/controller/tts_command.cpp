#include "iris/controller/commands.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/tts/gguf.hpp"
#include "iris/tts/tts.hpp"

namespace iris::controller {

namespace {

/// 张量名前缀（去掉层号）——用于把 478 个张量归成可读的几组。
std::string tensor_group(const std::string& name) {
    // 例：blk.12.attn_q.weight / talker.blk.3.ffn_down.weight / codec.dec.5.conv.weight
    std::string out;
    std::size_t i = 0;
    while (i < name.size()) {
        const std::size_t dot = name.find('.', i);
        if (dot == std::string::npos) {
            if (!out.empty()) out += '.';
            out += name.substr(i);
            break;
        }
        const std::string part = name.substr(i, dot - i);
        const bool numeric = !part.empty() && std::all_of(part.begin(), part.end(), [](char c) {
                                 return c >= '0' && c <= '9';
                             });
        if (!numeric) {
            if (!out.empty()) out += '.';
            out += part;
        }
        i = dot + 1;
    }
    return out;
}

std::string shape_of(const tts::gguf::TensorInfo& t) {
    std::string s = "[";
    for (std::size_t i = 0; i < t.dims.size(); ++i) {
        if (i) s += ", ";
        s += std::to_string(t.dims[i]);
    }
    s += "] " + std::string(tts::gguf::type_name(t.type));
    return s;
}

} // namespace

int run_tts_inspect(const std::string& model_path) {
    tts::gguf::File f;
    std::string error;
    if (!tts::gguf::load(model_path, f, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "GGUF: " << model_path << '\n';
    std::cout << "version=" << f.version << " tensors=" << f.tensor_count << '\n';
    std::cout << "--- metadata ---\n";
    for (const auto& [key, v] : f.meta) {
        std::cout << "  " << key << " = ";
        switch (v.type) {
            case tts::gguf::ValueType::String: std::cout << v.as_string; break;
            case tts::gguf::ValueType::F32:
            case tts::gguf::ValueType::F64: std::cout << v.as_float; break;
            case tts::gguf::ValueType::Array:
                std::cout << "<array of " << static_cast<int>(v.array_type) << ", len "
                          << v.array_len << ">";
                break;
            default: std::cout << v.as_int; break;
        }
        std::cout << '\n';
    }

    std::map<std::string, std::pair<int, std::string>> groups; // 组 → (数量, 示例形状)
    for (const auto& t : f.tensors) {
        auto& g = groups[tensor_group(t.name)];
        if (g.first == 0) g.second = shape_of(t);
        ++g.first;
    }
    std::cout << "--- tensor groups (" << groups.size() << ") ---\n";
    for (const auto& [name, info] : groups) {
        std::cout << "  " << info.first << " x " << name << "  e.g. " << info.second << '\n';
    }
    return 0;
}

int run_tts(const TtsOptions& opts) {
    tts::TtsEngine engine{};
    if (!tts::parse_tts_engine(opts.engine, engine)) {
        std::cerr << _("tts: unknown engine '", "tts: 未知引擎 '") << opts.engine
                  << _("' (supported: qwen3-tts | indextts)\n",
                       "'（支持: qwen3-tts | indextts）\n");
        return 1;
    }
    if (!tts::tts_engine_available(engine)) {
        std::cerr << _("tts: engine '", "tts: 引擎 '") << opts.engine
                  << _("' runtime is not available in this build (see `iris doctor`)\n",
                       "' 的推理运行时未随本次构建提供（用 `iris doctor` 查看）\n");
        return 1;
    }

    tts::TtsRequest request;
    request.engine = engine;
    request.model = opts.model;
    request.codec_model = opts.codec;
    request.text = opts.text;
    request.ref_wav = opts.ref_wav;
    request.ref_text = opts.ref_text;
    request.language = opts.language;
    request.steps = opts.steps;
    request.cfg_scale = opts.cfg_scale;

    tts::TtsResult result;
    std::string error;
    if (!tts::synthesize(request, result, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    if (!tts::write_wav(opts.out_wav, result.pcm, result.sample_rate, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << _("tts: wrote ", "tts: 写出 ") << opts.out_wav << " (" << result.pcm.size()
              << " samples @ " << result.sample_rate << " Hz, " << result.duration_s() << " s)\n";
    return 0;
}

} // namespace iris::controller
