#include "iris/cli/cli.hpp"

#include <cstdio>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"

#include <CLI/CLI.hpp>

#include "iris/controller/commands.hpp"
#include "iris/infra/log.hpp"

namespace iris::cli {

int run(int argc, char** argv) {
    CLI::App app{"iris — speech in, subtitles out (C++ migration of Centurion)"};
    app.set_version_flag("--version", std::string("iris ") + controller::kVersion);

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Verbose logging");

    // ---- asr ----
    auto* asr = app.add_subcommand("asr", "Transcribe media to an IR file (*.iris.json)");
    std::string asr_input, asr_lang = "en", asr_engine = "whisper", asr_model, asr_output;
    asr->add_option("input", asr_input, "Input media file")->required();
    asr->add_option("--language,-l", asr_lang, "Language code (e.g. en, zh)");
    asr->add_option("--engine,-e", asr_engine, "ASR engine (whisper | qwen3-asr | whisper-ct2)");
    asr->add_option("--model,-m", asr_model,
                    "Model path (whisper/qwen3-asr: ggml GGUF; whisper-ct2: CT2 model dir)");
    asr->add_option("--output,-o", asr_output, "Output IR path");

    // ---- build ----
    auto* build = app.add_subcommand("build", "Render an IR file to subtitles");
    std::string build_ir, build_fmt = "ass", build_out;
    build->add_option("ir", build_ir, "IR file (*.iris.json)")->required();
    build->add_option("--format,-f", build_fmt, "ass|srt|txt")
        ->check(CLI::IsMember({"ass", "srt", "txt"}));
    build->add_option("--output,-o", build_out, "Output path");

    // ---- validate ----
    auto* validate = app.add_subcommand("validate", "Validate an IR file");
    std::string v_ir;
    validate->add_option("ir", v_ir, "IR file")->required();

    // ---- doctor ----
    app.add_subcommand("doctor", "Environment diagnostics");

    // ---- models ----
    auto* models = app.add_subcommand("models", "Model & provider registry");
    std::string models_sub = "list";
    models->add_option("sub", models_sub, "Subcommand (list)");

    // ---- vsf ----
    auto* vsf = app.add_subcommand("vsf", "Detect hard-subtitle frames in a fixed ROI (self-implemented, no OpenCV)");
    std::string vsf_input, vsf_output, vsf_roi_str;
    double vsf_fps = 0.5;
    vsf->add_option("input", vsf_input, "Input video file")->required();
    vsf->add_option("--roi", vsf_roi_str, "ROI as x,y,w,h in pixels (required)")->required();
    vsf->add_option("--output,-o", vsf_output, "Output SRT path");
    vsf->add_option("--fps", vsf_fps, "Frame sample rate (default 0.5 = one frame per 2s)")
        ->check(CLI::Range(0.1, 10.0));

    // ---- tts ----
    auto* tts = app.add_subcommand("tts", "Text-to-speech (qwen3-tts / indextts)");
    controller::TtsOptions tts_opts;
    tts->add_option("--engine", tts_opts.engine, "TTS engine (qwen3-tts | indextts)")
        ->capture_default_str();
    tts->add_option("--model,-m", tts_opts.model,
                    "talker GGUF (qwen3-tts) or ONNX model dir (indextts)")
        ->required();
    tts->add_option("--codec", tts_opts.codec,
                    "12Hz tokenizer/codec GGUF (qwen3-tts; required)");
    tts->add_option("--text", tts_opts.text, "Text to synthesize");
    bool tts_inspect = false;
    tts->add_flag("--inspect", tts_inspect,
                  "Print the GGUF structure (metadata + tensor groups) and exit");
    tts->add_option("--ref", tts_opts.ref_wav, "Reference WAV for voice cloning");
    tts->add_option("--ref-text", tts_opts.ref_text,
                    "Transcript of --ref (required when cloning from a WAV)");
    tts->add_option("--lang", tts_opts.language, "Language hint (zh, en, ...)");
    tts->add_option("--steps", tts_opts.steps, "Sampling steps (0 = backend default)");
    tts->add_option("--cfg", tts_opts.cfg_scale, "CFG scale (0 = backend default)");
    tts->add_option("--out,-o", tts_opts.out_wav, "Output WAV path")->capture_default_str();

    // ---- emotion ----
    auto* emo = app.add_subcommand("emotion", "Speech emotion analysis (6-class wav2vec2 ONNX)");
    std::string emo_onnx, emo_wav;
    emo->add_option("--model", emo_onnx, "Emotion ONNX path")->required();
    emo->add_option("input", emo_wav, "Input 16kHz WAV")->required();

    // ---- dub ----
    auto* dub = app.add_subcommand("dub", "Dub pipeline: asr -> emotion -> tts");
    controller::DubOptions dub_opts;
    dub->add_option("input", dub_opts.input, "Input media file or existing IR (*.json)")
        ->required();
    dub->add_option("--engine", dub_opts.engine, "TTS engine (qwen3-tts | indextts)")
        ->capture_default_str();
    dub->add_option("--model,-m", dub_opts.model,
                    "TTS model: talker GGUF (qwen3-tts) or ONNX dir (indextts)")
        ->required();
    dub->add_option("--codec", dub_opts.codec, "12Hz tokenizer/codec GGUF (qwen3-tts; required)");
    dub->add_option("--ref", dub_opts.ref_wav, "Reference WAV for voice cloning");
    dub->add_option("--ref-text", dub_opts.ref_text,
                    "Transcript of --ref (required when cloning from a WAV)");
    dub->add_option("--language,-l", dub_opts.language, "Language code (e.g. en, zh)")
        ->capture_default_str();
    dub->add_option("--asr-engine", dub_opts.asr_engine,
                    "ASR engine (whisper | qwen3-asr | whisper-ct2)")
        ->capture_default_str();
    dub->add_option("--asr-model", dub_opts.asr_model, "ASR model (required for media input)");
    dub->add_option("--emotion-model", dub_opts.emotion_model, "Emotion ONNX path (optional)");
    dub->add_option("--out,-o", dub_opts.out_wav, "Output dub WAV path")->capture_default_str();
    dub->add_option("--ir-out", dub_opts.ir_out, "Also save the IR to this path");
    dub->add_option("--steps", dub_opts.steps, "TTS sampling steps (0 = backend default)");
    dub->add_option("--cfg", dub_opts.cfg_scale, "TTS CFG scale (0 = backend default)");
    dub->add_option("--gap-ms", dub_opts.gap_ms,
                    "Minimum pause kept between cues (ms)")
        ->capture_default_str();
    dub->add_option("--max-speed", dub_opts.max_speed,
                    "Max time-compression factor when fitting a cue to its slot")
        ->capture_default_str();
    bool no_fit = false;
    dub->add_flag("--no-fit", no_fit, "Disable fitting cues to the original timing");

    // ---- init ----
    app.add_subcommand("init", "Interactive setup (stub, planned)");

    CLI11_PARSE(app, argc, argv);

    infra::init_logger(verbose);

    // 分发到 MVC Controller 层。
    if (asr->parsed()) return controller::run_asr(asr_input, asr_lang, asr_engine, asr_model, asr_output);
    if (build->parsed()) return controller::run_build(build_ir, build_fmt, build_out);
    if (validate->parsed()) return controller::run_validate(v_ir);
    if (models->parsed()) return controller::run_models_list();
    if (vsf->parsed()) {
        int rx=0, ry=0, rw=0, rh=0;
        if (std::sscanf(vsf_roi_str.c_str(), "%d,%d,%d,%d", &rx, &ry, &rw, &rh) != 4) {
            std::cerr << _("--roi format: x,y,w,h (pixels)\n", "--roi 格式应为 x,y,w,h（像素）\n");
            return 1;
        }
        return controller::run_vsf(vsf_input, vsf_output, rx, ry, rw, rh, vsf_fps);
    }
    if (tts->parsed()) {
        if (tts_inspect) return controller::run_tts_inspect(tts_opts.model);
        return controller::run_tts(tts_opts);
    }
    if (emo->parsed()) return controller::run_emotion(emo_onnx, emo_wav);
    if (dub->parsed()) {
        dub_opts.fit_timing = !no_fit;
        return controller::run_dub(dub_opts);
    }
    if (app.get_subcommand("doctor")->parsed()) return controller::run_doctor();
    if (app.get_subcommand("init")->parsed()) {
        std::cout << _("init: interactive setup coming soon\n", "init: 交互式设置将在后续版本实现\n");
        return 0;
    }

    // 无子命令
    std::cout << app.help() << '\n';
    return 0;
}

} // namespace iris::cli
