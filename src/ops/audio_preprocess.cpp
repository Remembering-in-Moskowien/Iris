#include "daisy/ops/audio_preprocess.hpp"

#include "daisy/infra/log.hpp"
#include "daisy/infra/process.hpp"

namespace daisy::ops {

bool AudioPreprocess::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;
    if (!st.converted_audio_path) {
        spdlog::error("audio_preprocess: 缺少已转换音频");
        return false;
    }

    // 简易预处理：speechnorm + 轻量降噪（对应 C# AudioPreprocessConfig 的默认轻量路径）。
    const std::string in = *st.converted_audio_path;
    const std::string out = ctx.work_dir + "/preprocessed.wav";
    const auto result = infra::run_process({
        "ffmpeg", "-y", "-i", in,
        "-af", "speechnorm=e=6:r=0.00003:l=1,highpass=f=80",
        "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", out});
    if (!result.ok()) {
        spdlog::error("audio_preprocess: {}", result.error());
        return false;
    }
    if (!result.value().success()) {
        spdlog::error("audio_preprocess: ffmpeg 退出码 {}", result.value().exit_code);
        return false;
    }

    st.preprocessed_audio_path = out;
    ctx.record_provenance(name());
    spdlog::info("audio_preprocess: 输出 {}", out);
    return true;
}

} // namespace daisy::ops
