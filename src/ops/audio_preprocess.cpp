#include "iris/ops/audio_preprocess.hpp"

#include "iris/infra/log.hpp"
#include "iris/infra/process.hpp"

namespace iris::ops {

bool AudioPreprocess::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;

    // 有人声轨就先吃人声轨（对应 C# 的 VocalsPath ?? ConvertedAudioPath）。
    const std::string* source = st.vocals_path ? &*st.vocals_path
                                               : (st.converted_audio_path ? &*st.converted_audio_path
                                                                          : nullptr);
    if (source == nullptr) {
        spdlog::error("audio_preprocess: 缺少已转换音频");
        return false;
    }

    // 简易预处理：speechnorm + 轻量降噪（对应 C# AudioPreprocessConfig 的默认轻量路径）。
    const std::string in = *source;
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
    spdlog::info("audio_preprocess: {} → {}", st.vocals_path ? "人声轨" : "转换音频", out);
    return true;
}

} // namespace iris::ops
