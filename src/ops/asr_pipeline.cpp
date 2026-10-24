#include "iris/ops/asr_pipeline.hpp"

#include <memory>
#include <utility>

#include "iris/ops/audio_preprocess.hpp"
#include "iris/ops/ffmpeg_convert.hpp"
#include "iris/ops/sentence_split.hpp"
#include "iris/ops/transcribe.hpp"

namespace iris::ops {

core::Pipeline make_asr_pipeline(asr::AsrBackend backend, std::string model) {
    core::Pipeline pipeline;
    pipeline.add(std::make_shared<FfmpegConvert>());
    pipeline.add(std::make_shared<AudioPreprocess>());
    pipeline.add(std::make_shared<Transcribe>(
        std::make_shared<asr::AsrEngine>(backend, std::move(model))));
    pipeline.add(std::make_shared<SentenceSplit>());
    return pipeline;
}

} // namespace iris::ops
