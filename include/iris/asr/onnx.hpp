#pragma once

#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::asr {

/// whisper（ONNX）后端 —— 运行时引用 tools/runtimes/onnx 打包的 onnxruntime DLL。
///
/// 模型目录须为 whisperX / Xenova 风格：
///   encoder_model.onnx（mel → encoder_out）＋ decoder_model_merged.onnx
///   （自回归 + kv 缓存）＋ tokenizer.json。
/// 推理编排（mel/30s 窗口/token 解析/prompt 构造）由 Iris 自写。
/// GPU 可行：构建期定义 IRIS_ONNX_CUDA 时注册 CUDA EP（无 GPU 自动回落 CPU EP）。
bool transcribe_onnx(const std::string& model_dir, const std::string& audio_path,
                     const std::string& language, std::vector<model::Sentence>& out,
                     std::string& error);

} // namespace iris::asr
