#include "iris/controller/commands.hpp"

#include <filesystem>
#include <memory>
#include <string>

#include "iris/asr/asr.hpp"
#include "iris/core/pipeline.hpp"
#include "iris/infra/log.hpp"
#include "iris/model/config.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/ops/audio_preprocess.hpp"
#include "iris/ops/ffmpeg_convert.hpp"
#include "iris/ops/diarize.hpp"
#include "iris/ops/vocal_separate.hpp"
#include "iris/text/punctuate.hpp"
#include "iris/text/sat.hpp"
#include "iris/ops/sentence_split.hpp"
#include "iris/ops/transcribe.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

int run_asr(const std::string& input, const std::string& language, const std::string& engine,
            const std::string& model, const std::string& output,
            const asr::DiarizeOptions* diarize, int n_threads,
            const asr::SeparateOptions* separate, const std::string& punc_model,
            const std::string& split_strategy, const std::string& split_model) {
    if (!fs::exists(input)) {
        spdlog::error("asr: 输入文件不存在: {}", input);
        return 1;
    }
    asr::AsrBackend backend{};
    if (!asr::parse_asr_backend(engine, backend)) {
        spdlog::error("asr: 未知引擎 '{}'（可选 whisper | qwen3-asr | whisper-ct2）", engine);
        return 1;
    }
    if (model.empty()) {
        spdlog::error("asr: 需要 --model 指定模型路径（whisper/qwen3-asr 用 ggml GGUF，"
                      "whisper-ct2 用 CTranslate2 模型目录）");
        return 1;
    }
    if (!asr::asr_backend_available(backend)) {
        spdlog::error("asr: 引擎 '{}' 的运行时 DLL 未就绪（tools/runtimes 下缺 "
                      "crispasr 或 ct2 包，用 `iris doctor` 查看）", engine);
        return 1;
    }
    if (!fs::exists(model)) {
        spdlog::error("asr: 模型路径不存在: {}", model);
        return 1;
    }

    // 工作目录
    const fs::path work = fs::temp_directory_path() / "iris-run";
    std::error_code ec;
    fs::create_directories(work, ec);
    if (ec) {
        spdlog::error("asr: 无法创建工作目录: {}", ec.message());
        return 1;
    }

    // IR 文档（Model）
    model::IrDocument doc;
    doc.generator.version = kVersion;
    doc.generator.command = "asr";
    doc.generator.generated_at = "now";
    doc.generator.input_file = input;
    doc.config.input_file_path = input;
    doc.config.command_name = "asr";
    doc.config.language = language;
    doc.config.transcriber_engine = engine;
    doc.config.transcriber_model = model;
    doc.config.split_strategy = split_strategy;

    core::PipelineContext ctx;
    ctx.doc = &doc;
    ctx.work_dir = work.string();
    ctx.on_progress = [](const std::string& stage, float) {
        spdlog::info("→ 阶段: {}", stage);
    };

    core::Pipeline pipeline;
    pipeline.add(std::make_shared<ops::FfmpegConvert>());
    if (separate != nullptr) {
        pipeline.add(std::make_shared<ops::VocalSeparate>(*separate));
    }
    pipeline.add(std::make_shared<ops::AudioPreprocess>());
    std::shared_ptr<text::Punctuator> punctuator;
    if (!punc_model.empty()) {
        punctuator = std::make_shared<text::Punctuator>();
        std::string punc_error;
        if (!punctuator->open(punc_model, punc_error)) {
            spdlog::error("{}", punc_error);
            return 1;
        }
    }
    pipeline.add(std::make_shared<ops::Transcribe>(
        std::make_shared<asr::AsrEngine>(backend, model, n_threads), punctuator));
    std::shared_ptr<text::SatSegmenter> sat;
    if (split_strategy == "sat") {
        sat = std::make_shared<text::SatSegmenter>();
        std::string sat_error;
        if (!sat->open(split_model.empty() ? "models/sat" : split_model, sat_error)) {
            spdlog::warn("{}（退回 rule 策略）", sat_error);
            sat.reset();
        }
    }
    pipeline.add(std::make_shared<ops::SentenceSplit>(sat));
    if (diarize != nullptr) {
        pipeline.add(std::make_shared<ops::Diarize>(*diarize));
        doc.config.command_name = "asr+diarize";
    }

    if (!pipeline.run(ctx)) {
        spdlog::error("asr: 管线执行失败");
        return 1;
    }

    const fs::path out_path =
        output.empty() ? fs::path(input + ".iris.json") : fs::path(output);
    doc.save(out_path.string());
    spdlog::info("asr: 已保存 IR -> {}", out_path.string());
    return 0;
}

} // namespace iris::controller
