#include "iris/cli/cli.hpp"

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

    // ---- init ----
    app.add_subcommand("init", "Interactive setup (stub, planned)");

    CLI11_PARSE(app, argc, argv);

    infra::init_logger(verbose);

    // 分发到 MVC Controller 层。
    if (asr->parsed()) return controller::run_asr(asr_input, asr_lang, asr_engine, asr_model, asr_output);
    if (build->parsed()) return controller::run_build(build_ir, build_fmt, build_out);
    if (validate->parsed()) return controller::run_validate(v_ir);
    if (models->parsed()) return controller::run_models_list();
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
