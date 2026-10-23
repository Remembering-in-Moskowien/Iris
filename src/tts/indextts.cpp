// ============================================================================
// IndexTTS 2.5 ONNX 后端 —— 推理代码全部由 Iris 自写，运行时引用打包好的
// onnxruntime DLL（tools/runtimes/onnx，gitignored，不内嵌源码）。
//
// 流水线：text frontend → w2v-bert → semantic codec → GPT 自回归 →
//         length regulator → flow-matching CFM (DiT) → BigVGAN vocoder → 22050Hz WAV
//
// 当前为骨架：模型文件存在性检查 + ONNX session 加载框架。完整编排
// （文本归一化/采样器/CFM solver/DSP）随模型权重就位后逐步补齐。
// ============================================================================

#include "iris/tts/indextts.hpp"

#include <filesystem>
#include <string>

#include "iris/infra/log.hpp"

#if IRIS_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#include <spdlog/fmt/fmt.h>
#endif

namespace iris::tts {

bool synth_indextts(const std::string& model_dir,
                    const std::string& text,
                    const std::string& ref_wav,
                    const std::string& out_wav,
                    std::string& error) {
#if !IRIS_HAS_ONNX
    error = "Iris 未以 ONNX 运行时构建（tools/runtimes/onnx 缺失）";
    return false;
#else
    namespace fs = std::filesystem;

    // 1. 模型文件检查
    const std::vector<std::string> required = {
        "gpt.onnx", "cfm.onnx", "bigvgan.onnx", "w2vbert.onnx", "codec.onnx"};
    for (const auto& f : required) {
        if (!fs::exists(fs::path(model_dir) / f)) {
            error = fmt::format("IndexTTS 模型缺失：{}/{}", model_dir, f);
            return false;
        }
    }

    // 2. 参考音频检查
    if (!fs::exists(ref_wav)) {
        error = "参考音频不存在：" + ref_wav;
        return false;
    }

    // 3. ONNX session 加载框架（完整编排待模型权重就位后补齐）
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "iris-tts");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(4);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
#ifdef IRIS_ONNX_CUDA
    OrtSessionOptionsAppendExecutionProvider_CUDA(opts, 0);
#endif

    spdlog::info("tts: IndexTTS 模型目录={} 文本长度={} 参考={}",
                 model_dir, text.size(), ref_wav);
    spdlog::warn("tts: IndexTTS 完整编排（GPT 采样/CFM solver/BigVGAN）尚未实现——"
                 "当前仅完成模型加载框架，待权重就位后补齐");

    // TODO: 文本前端（wetext 归一化 + tiktoken）
    // TODO: w2v-bert 参考音频特征提取
    // TODO: GPT 自回归声学 token 生成（greedy/sampling）
    // TODO: length regulator + CFM flow-matching solver
    // TODO: BigVGAN vocoder → 22050Hz int16 WAV 写出
    (void)out_wav;
    error = "IndexTTS 编排尚未实现（骨架已就位，待模型权重与 DSP 补齐）";
    return false;
#endif
}

} // namespace iris::tts
