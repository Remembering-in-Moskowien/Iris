#pragma once

#include <string>

#include "iris/asr/asr.hpp"
#include "iris/core/pipeline.hpp"

namespace iris::ops {

// ---------------------------------------------------------------------------
// asr 主链路组装（asr / dub 命令共用，避免两处重复拼装同一张 DAG）：
//   ffmpeg_convert → audio_preprocess → transcribe → sentence_split
// ---------------------------------------------------------------------------
core::Pipeline make_asr_pipeline(asr::AsrBackend backend, std::string model);

} // namespace iris::ops
