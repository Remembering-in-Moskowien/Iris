#pragma once

#include <string>
#include <vector>

namespace iris::tts {

/// IndexTTS 2.5 ONNX 后端 —— 运行时引用 tools/runtimes/onnx 打包的 onnxruntime DLL。
///
/// 模型目录（yunfengwang/IndexTTS-2.5-onnx，fp32 ~7GB）须包含：
///   gpt.onnx          自回归声学 token 生成
///   cfm.onnx          flow-matching DiT（mel 生成）
///   bigvgan.onnx      神经声码器（mel → 22050Hz PCM）
///   w2vbert.onnx      参考音频语义特征提取
///   codec.onnx        语义编解码
///
/// 编排（文本前端/采样器/CFM solver/DSP）由 Iris 自写；重网络走 ONNX 图。
/// GPU 可行：构建期定义 IRIS_ONNX_CUDA 时注册 CUDA EP（无 GPU 自动回落 CPU EP）。
///
/// ref_wav：≤15s 参考音频（音色克隆）。text：待合成文本（zh/en/ja/yue 混合）。
/// out_wav：输出 22050Hz int16 WAV。
bool synth_indextts(const std::string& model_dir,
                    const std::string& text,
                    const std::string& ref_wav,
                    const std::string& out_wav,
                    std::string& error);

} // namespace iris::tts
