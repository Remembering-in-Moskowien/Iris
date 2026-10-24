#include "iris/ops/vocal_separate.hpp"

#include <string>

#include "iris/infra/log.hpp"

namespace iris::ops {

bool VocalSeparate::execute(core::PipelineContext& ctx) {
    if (!ctx.doc) return false;
    auto& st = ctx.doc->state;

    const std::string input = ctx.doc->config.input_file_path;
    if (input.empty()) {
        spdlog::error("vocal_separate: 缺少输入媒体路径");
        return false;
    }

    const std::string out = ctx.work_dir + "/vocals.wav";
    std::string error;
    if (!asr::separate_stem(input, opts_, out, error)) {
        spdlog::error("{}", error);
        return false;
    }

    st.vocals_path = out;
    st.is_vocals_separated = true;
    ctx.record_provenance(name(), opts_.model);
    spdlog::info("vocal_separate: 人声轨 → {}", out);
    return true;
}

} // namespace iris::ops
