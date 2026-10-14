#include "daisy/ops/ffmpeg_convert.hpp"

#include "daisy/infra/log.hpp"
#include "daisy/infra/process.hpp"

namespace daisy::ops {

bool FfmpegConvert::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;
    const auto& cfg = ctx.doc->config;

    if (cfg.input_file_path.empty()) {
        spdlog::error("ffmpeg_convert: 缺少输入文件");
        return false;
    }

    // 输出 16kHz 单声道 WAV（与 C# 版默认转换参数一致）。
    const std::string out_path = ctx.work_dir + "/converted.wav";
    const auto result = infra::run_process({
        "ffmpeg", "-y", "-i", cfg.input_file_path,
        "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", out_path});
    if (!result.ok()) {
        spdlog::error("ffmpeg_convert: {}", result.error());
        return false;
    }
    if (!result.value().success()) {
        spdlog::error("ffmpeg_convert: ffmpeg 退出码 {}", result.value().exit_code);
        spdlog::debug("ffmpeg 输出: {}", result.value().stdout_text);
        return false;
    }

    st.converted_audio_path = out_path;
    st.is_audio_converted = true;
    ctx.record_provenance(name());
    spdlog::info("ffmpeg_convert: 输出 {}", out_path);
    return true;
}

} // namespace daisy::ops
