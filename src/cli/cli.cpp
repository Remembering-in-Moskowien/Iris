#include "iris/cli/cli.hpp"

#include <cstdio>
#include <iostream>
#include <string>

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
    auto* tts = app.add_subcommand("tts", "Text-to-speech (IndexTTS ONNX voice cloning)");
    std::string tts_engine = "indextts", tts_model, tts_text, tts_ref, tts_out;
    tts->add_option("--engine", tts_engine, "TTS engine (default indextts)");
    tts->add_option("--model,-m", tts_model, "Model dir (IndexTTS ONNX weights)")->required();
    tts->add_option("--text", tts_text, "Text to synthesize")->required();
    tts->add_option("--ref", tts_ref, "Reference WAV for voice cloning (<=15s)")->required();
    tts->add_option("--out,-o", tts_out, "Output WAV path");

    // ---- emotion ----
    auto* emo = app.add_subcommand("emotion", "Speech emotion analysis (A/D/V, Wav2Small ONNX)");
    std::string emo_onnx, emo_wav;
    emo->add_option("--model", emo_onnx, "Wav2Small ONNX path")->required();
    emo->add_option("input", emo_wav, "Input 16kHz WAV")->required();

    // ---- dub ----
    auto* dub = app.add_subcommand("dub", "Dub pipeline: asr -> emotion -> tts");
    std::string dub_input, dub_model, dub_emo_onnx, dub_ref, dub_out;
    dub->add_option("input", dub_input, "Input audio/video");
    dub->add_option("--model,-m", dub_model, "TTS model dir")->required();
    dub->add_option("--emotion-model", dub_emo_onnx, "Emotion ONNX path (optional)");
    dub->add_option("--ref", dub_ref, "Reference WAV for voice cloning");
    dub->add_option("--out,-o", dub_out, "Output dub WAV");

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
            std::cerr << "--roi 格式应为 x,y,w,h（像素）\n";
            return 1;
        }
        return controller::run_vsf(vsf_input, vsf_output, rx, ry, rw, rh, vsf_fps);
    }
    if (tts->parsed()) return controller::run_tts(tts_engine, tts_model, tts_text, tts_ref, tts_out);
    if (emo->parsed()) return controller::run_emotion(emo_onnx, emo_wav);
    if (dub->parsed()) return controller::run_dub(dub_input, dub_model, dub_emo_onnx, dub_ref, dub_out);
    if (app.get_subcommand("doctor")->parsed()) return controller::run_doctor();
    if (app.get_subcommand("init")->parsed()) {
        std::cout << "init: 交互式设置将在后续版本实现\n";
        return 0;
    }

    // 无子命令
    std::cout << app.help() << '\n';
    return 0;
}

} // namespace iris::cli
