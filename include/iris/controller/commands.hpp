#pragma once

#include <string>

namespace iris::controller {

/// 版本号（Model/IR generator.version 与 --version 共用）。
/// CI 通过 IRIS_BUILD_NUMBER 注入构建号（release tag = v{run_number}）；本地默认 0.1.0。
#ifndef IRIS_VERSION_STRING
#define IRIS_VERSION_STRING "0.1.0"
#endif
inline constexpr const char* kVersion = IRIS_VERSION_STRING;

// MVC Controller 层：每个子命令一个处理器，负责编排 Model 与 Service（ops），
// 并把结果交给 View（渲染/控制台）。返回进程退出码（0 成功，非 0 失败）。

/// asr：媒体 → IR（ffmpeg_convert → audio_preprocess → transcribe → sentence_split）。
int run_asr(const std::string& input, const std::string& language, const std::string& engine,
            const std::string& model, const std::string& output);

/// build：IR → ASS/SRT/TXT（View 层渲染）。
int run_build(const std::string& ir_path, const std::string& format, const std::string& output);

/// validate：校验 IR 文件。
int run_validate(const std::string& ir_path);

/// doctor：环境诊断（外部媒体工具 + 内嵌 ASR 后端可用性）。
int run_doctor();

/// models list：列出已注册提供者。
int run_models_list();

/// vsf：视频硬字幕帧定位（自实现，ROI 由用户传精准绝对坐标，无外部 VSF 依赖）。
int run_vsf(const std::string& input, const std::string& output,
             int roi_x, int roi_y, int roi_w, int roi_h, double sample_fps);

} // namespace iris::controller
