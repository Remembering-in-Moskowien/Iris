#include "iris/cli/cli.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"

#include <CLI/CLI.hpp>

#include "iris/controller/commands.hpp"
#include "iris/text/punctuate.hpp"
#include "iris/infra/log.hpp"
#include "iris/ui/console.hpp"
#include <nlohmann/json.hpp>
#include "iris/model/config.hpp"

namespace iris::cli {

namespace {

/// --punc-model=auto 时优先用本机已有的 GGUF，找不到再交给库自己下载。
std::string resolve_punc_model(const std::string& requested) {
    if (!requested.empty() && requested != "auto") return requested;
    const std::string local = iris::text::default_punc_model();
    return local.empty() ? std::string("auto") : local;
}

/// 解析 --shift：`+1:10:27.30`、`-0:00:02.500` 或 `+4227.3`（秒）→ 毫秒。
bool parse_shift_ms(const std::string& text, std::int64_t& out) {
    std::string s = text;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (s.empty()) return false;

    double sign = 1.0;
    if (s.front() == '+') {
        s.erase(s.begin());
    } else if (s.front() == '-') {
        sign = -1.0;
        s.erase(s.begin());
    }

    double seconds = 0.0;
    const auto first = s.find(':');
    if (first == std::string::npos) {
        char* end = nullptr;
        seconds = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0') return false;
    } else {
        const auto second = s.find(':', first + 1);
        if (second == std::string::npos) return false;
        const double h = std::atof(s.substr(0, first).c_str());
        const double m = std::atof(s.substr(first + 1, second - first - 1).c_str());
        const double sec = std::atof(s.substr(second + 1).c_str());
        seconds = h * 3600.0 + m * 60.0 + sec;
    }
    out = static_cast<std::int64_t>(std::llround(sign * seconds * 1000.0));
    return true;
}

} // namespace

int run(int argc, char** argv) {
    CLI::App app{"iris — speech in, subtitles out (C++ migration of Centurion)"};
    app.set_version_flag("--version", std::string("iris ") + controller::kVersion);

    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Verbose logging");
    bool agent = false;
    bool json_output = false;
    app.add_flag("--json", json_output, "JSON mode: stdout carries only a JSON result document");
    app.add_flag("--agent", agent,
                 "Agent mode: plain INFO/OK/WARN/ERROR output, no colours, spinners or timestamps");
    std::string config_path;
    app.add_option("--config", config_path,
                   "Config file (default: <install drive>/iris/config.json; IRIS_HOME overrides)")
        ->envname("IRIS_CONFIG");

    // ---- asr ----
    auto* asr = app.add_subcommand("asr", "Transcribe media to an IR file (*.iris.json)");
    std::string asr_input, asr_lang = "en", asr_engine = "whisper", asr_model, asr_output;
    asr->add_option("input", asr_input, "Input media file")->required();
    asr->add_option("--language,-l", asr_lang, "Language code (e.g. en, zh)");
    asr->add_option("--engine,-e", asr_engine, "ASR engine (whisper | qwen3-asr | whisper-ct2)");
    asr->add_option("--model,-m", asr_model,
                    "Model path (whisper/qwen3-asr: ggml GGUF; whisper-ct2: CT2 model dir)");
    asr->add_option("--output,-o", asr_output, "Output IR path");
    bool asr_diarize = false;
    std::string asr_speaker_model;
    int asr_speakers = 0;
    int asr_diarize_method = 4;
    int asr_threads = 0;
    std::string asr_split = "rule";
    std::string asr_split_model = "models/sat";
    bool asr_punc = false;
    std::string asr_punc_model = "auto";
    bool asr_separate = false;
    std::string asr_separation_model;
    int asr_sep_chunk = 16;
    asr->add_flag("--diarize", asr_diarize,
                  "Run speaker diarization after sentence splitting (labels SPEAKER_0..)");
    asr->add_option("--speaker-model", asr_speaker_model,
                    "Speaker embedding GGUF for --diarize (default: ~/.cache/crispasr/wespeaker*.gguf)");
    asr->add_option("--speakers", asr_speakers,
                    "Exact number of speakers for --diarize (0 = auto 1..8)");
    asr->add_option("--split", asr_split, "Sentence split strategy: rule | sat")->capture_default_str();
    asr->add_option("--split-model", asr_split_model, "SaT model dir (tokenizer.json + model.onnx)")
        ->capture_default_str();
    asr->add_flag("--punc", asr_punc, "Restore punctuation on the transcript (FireRedPunc/PCS)");
    asr->add_option("--punc-model", asr_punc_model,
                    "Punctuation model: alias (auto|firered|fullstop|punctuate-all|pcs) or .gguf")
        ->capture_default_str();
    asr->add_flag("--separate", asr_separate,
                  "Separate the vocal stem before transcription (HTDemucs)");
    asr->add_option("--separation-chunk", asr_sep_chunk,
                    "Separation chunk seconds (more chunks = more parallelism)")
        ->capture_default_str();
    asr->add_option("--separation-model", asr_separation_model,
                    "HTDemucs GGUF for --separate (default: ~/.cache/crispasr/htdemucs*.gguf)");
    asr->add_option("--threads", asr_threads, "Inference threads (0 = auto, capped at 8)")
        ->capture_default_str();
    asr->add_option("--diarize-method", asr_diarize_method,
                    "Diarization method (4 = FoxNose/WeSpeaker, 5 = Sortformer, 3 = pyannote)")
        ->capture_default_str();

    // ---- build ----
    auto* build = app.add_subcommand("build", "Render an IR file to subtitles");
    std::string build_ir, build_fmt = "ass", build_out, build_shift;
    view::AssBuildOptions build_opts;
    bool build_lf = false;
    build->add_option("ir", build_ir, "IR file (*.iris.json)")->required();
    build->add_option("--format,-f", build_fmt, "ass|srt|txt")
        ->check(CLI::IsMember({"ass", "srt", "txt"}));
    build->add_option("--output,-o", build_out, "Output path");
    build->add_option("--template,-t", build_opts.template_path,
                      "Style template (.json, or an .ass/.ssa to extract from)");
    build->add_flag("--bilingual", build_opts.bilingual,
                    "Emit translated + original lines with the same timecode");
    build->add_option("--shift", build_shift, "Global shift: +1:10:27.30 or +4227.3 (seconds)");
    build->add_option("--scale", build_opts.scale, "Canvas/font scale (2 = 4K)")
        ->capture_default_str();
    build->add_flag("--bom", build_opts.bom, "Write UTF-8 BOM");
    build->add_flag("--lf", build_lf, "Write LF instead of CRLF");

    // ---- validate ----
    auto* validate = app.add_subcommand("validate", "Validate an IR file");
    std::string v_ir;
    validate->add_option("ir", v_ir, "IR file")->required();

    // ---- doctor ----
    app.add_subcommand("doctor", "Environment diagnostics");


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
    tts->add_option("--threads", tts_opts.n_threads, "Inference threads (0 = auto, capped at 8)")
        ->capture_default_str();
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
    dub->add_option("--voice", dub_opts.voice_refs,
                    "Per-speaker reference audio: SPEAKER_1=path.wav (repeatable)")
        ->expected(1, -1);
    dub->add_option("--voice-text", dub_opts.voice_texts,
                    "Per-speaker reference transcript: SPEAKER_1=text (repeatable)")
        ->expected(1, -1);
    dub->add_option("--voices", dub_opts.voices_json,
                    "Voice config JSON: {default:{ref,ref_text}, speakers:{SPEAKER_1:{...}}}");
    dub->add_option("--threads", dub_opts.n_threads, "Inference threads (0 = auto, capped at 8)")
        ->capture_default_str();
    dub->add_option("--cfg", dub_opts.cfg_scale, "TTS CFG scale (0 = backend default)");
    dub->add_option("--gap-ms", dub_opts.gap_ms,
                    "Minimum pause kept between cues (ms)")
        ->capture_default_str();
    dub->add_option("--max-speed", dub_opts.max_speed,
                    "Max time-compression factor when fitting a cue to its slot")
        ->capture_default_str();
    bool no_fit = false;
    dub->add_flag("--no-fit", no_fit, "Disable fitting cues to the original timing");

    // ---- translate ----
    auto* translate = app.add_subcommand(
        "translate", "Machine translation (non-LLM: OPUS-MT/Marian, M2M100)");
    controller::TranslateCommandOptions translate_opts;
    translate->add_option("input", translate_opts.input, "IR file (*.json) or media file")
        ->required();
    translate->add_option("--model,-m", translate_opts.model, "Translation model (GGUF)")
        ->required();
    translate->add_option("--engine", translate_opts.engine,
                          "Backend (auto | m2m100 | opus-mt | wmt21 | madlad)")
        ->capture_default_str();
    translate->add_option("--from", translate_opts.from, "Source language")
        ->capture_default_str();
    translate->add_option("--to", translate_opts.to, "Target language")->capture_default_str();
    translate->add_option("--out,-o", translate_opts.out_ir, "Output IR path");
    translate->add_option("--asr-engine", translate_opts.asr_engine,
                          "ASR engine for media input (whisper | qwen3-asr | whisper-ct2)")
        ->capture_default_str();
    translate->add_option("--asr-model", translate_opts.asr_model, "ASR model (media input)");
    translate->add_option("--beam", translate_opts.beam_size,
                          "Beam size (1 = greedy; MT checkpoints default to 5)")
        ->capture_default_str();
    translate->add_option("--max-tokens", translate_opts.max_tokens,
                          "Max generated tokens per sentence")
        ->capture_default_str();

    // ---- config ----
    auto* config = app.add_subcommand("config", "Show or create the user configuration");
    auto* config_show = config->add_subcommand("show", "Print the active config as JSON");
    auto* config_path_cmd = config->add_subcommand("path", "Print the active config file path");
    auto* config_init = config->add_subcommand("init", "Write the default config if missing");

    // ---- from-script ----
    auto* from_script = app.add_subcommand(
        "from-script", "Align a plain-text script to the media and produce timed subtitles");
    controller::FromScriptOptions from_script_opts;
    from_script->add_option("input", from_script_opts.input, "Media file")->required();
    from_script->add_option("--script,-s", from_script_opts.script, "Script file (one line = one cue)")
        ->required();
    from_script->add_option("--out,-o", from_script_opts.out_ir, "Output IR path");
    from_script->add_option("--language,-l", from_script_opts.language, "Language code")
        ->capture_default_str();
    from_script->add_option("--asr-engine", from_script_opts.asr_engine, "ASR engine for timing")
        ->capture_default_str();
    from_script->add_option("--asr-model", from_script_opts.asr_model, "ASR model for timing")
        ->capture_default_str();
    from_script->add_flag("--keep-speaker-in-text", from_script_opts.keep_speaker_labels_in_text,
                          "Keep the leading 'SPEAKER:' label inside the text");

    // ---- serve ----
    auto* serve = app.add_subcommand("serve", "Start the REST API (async job queue)");
    std::string serve_host = "127.0.0.1";
    std::string serve_models = "models";
    std::string serve_work = (std::filesystem::temp_directory_path() / "iris-serve").string();
    int serve_port = 8080;
    serve->add_option("--host", serve_host, "Bind address (default 127.0.0.1)")->capture_default_str();
    serve->add_option("--port,-p", serve_port, "Port")->capture_default_str();
    serve->add_option("--models", serve_models, "Models directory")->capture_default_str();
    serve->add_option("--work", serve_work, "Job work directory")->capture_default_str();

    // ---- models ----
    auto* models = app.add_subcommand("models", "List or install the models iris uses");
    auto* models_list = models->add_subcommand("list", "Show installed / missing models");
    std::string models_root = "models";
    std::string models_mirror;
    models_list->add_option("--root", models_root, "Models directory")->capture_default_str();

    auto* models_install = models->add_subcommand("install", "Download models into the models tree");
    std::vector<std::string> install_keys;
    std::string install_category;
    bool install_all = false;
    bool install_dry = false;
    models_install->add_option("keys", install_keys, "Model keys (e.g. ppocrv5 whisper-base)")
        ->expected(0, -1);
    models_install->add_option("--root", models_root, "Models directory")->capture_default_str();
    models_install->add_option("--mirror", models_mirror,
                               "Hugging Face mirror base (default: $IRIS_HF_MIRROR)");
    models_install->add_option("--category", install_category, "Install a whole category");
    models_install->add_flag("--all", install_all, "Install every registered model");
    models_install->add_flag("--dry-run", install_dry, "Show what would be downloaded");

    // ---- quality ----
    auto* quality = app.add_subcommand(
        "quality", "Analyse an IR and write a self-contained HTML quality report");
    std::string quality_ir, quality_out;
    quality->add_option("ir", quality_ir, "IR file (*.iris.json)")->required();
    quality->add_option("--out,-o", quality_out, "Output HTML path (default: <ir>.quality.html)");

    // ---- lint ----
    auto* lint = app.add_subcommand(
        "lint", "Read-only ASS/SSA checkup (styles, karaoke, CPS, overlaps, fonts) → HTML report");
    std::vector<std::string> lint_inputs;
    std::string lint_html;
    iris::quality::LintOptions lint_opts;
    bool lint_no_fonts = false;
    lint->add_option("paths", lint_inputs, "ASS/SSA files or directories")->required();
    lint->add_option("--out,-o", lint_html, "HTML report path (default: iris-lint.html)");
    lint->add_option("--fonts", lint_opts.font_dirs, "Extra font directory (repeatable)");
    lint->add_flag("--no-fonts", lint_no_fonts, "Skip font existence checks");
    lint->add_option("--zh-cps", lint_opts.zh_max_cps, "Chinese CPS limit (default 8)")
        ->capture_default_str();
    lint->add_option("--en-cps", lint_opts.en_max_cps, "Latin CPS limit (default 25)")
        ->capture_default_str();
    lint->add_option("--max-cjk", lint_opts.max_cjk_chars, "Max CJK chars per line (default 20)")
        ->capture_default_str();
    lint->add_option("--max-issues", lint_opts.max_issues_per_file,
                     "Issues listed per file in the report (default 60)")
        ->capture_default_str();

    // ---- ocr ----
    auto* ocr = app.add_subcommand("ocr", "Extract hard-coded subtitles from a video into an IR");
    controller::OcrCommandOptions ocr_opts;
    ocr->add_option("video", ocr_opts.input, "Video file")->required();
    ocr->add_option("--out,-o", ocr_opts.out_ir, "Output IR path");
    ocr->add_option("--language,-l", ocr_opts.language, "Language code")->capture_default_str();
    ocr->add_option("--roi", ocr_opts.roi, "Subtitle ROI: x,y,w,h (default: bottom 40%)");
    ocr->add_option("--model-dir", ocr_opts.model_dir, "PP-OCR model dir")->capture_default_str();
    ocr->add_option("--fps", ocr_opts.sample_fps, "Frame sampling rate for detection")
        ->capture_default_str();

    // ---- convert / combine ----
    auto* convert = app.add_subcommand("convert", "Import one subtitle file (SRT/ASS/VTT) into an IR");
    std::vector<std::string> convert_inputs, combine_inputs;
    std::string convert_out, combine_out, convert_lang = "en", combine_lang = "en";
    convert->add_option("subtitle", convert_inputs, "Subtitle file (.srt/.ass/.ssa/.vtt)")->required();
    convert->add_option("--out,-o", convert_out, "Output IR path");
    convert->add_option("--language,-l", convert_lang, "Language code")->capture_default_str();

    auto* combine = app.add_subcommand("combine", "Merge several subtitle files into one IR");
    combine->add_option("subtitles", combine_inputs, "Subtitle files (first = primary)")
        ->required();
    combine->add_option("--out,-o", combine_out, "Output IR path");
    combine->add_option("--language,-l", combine_lang, "Language code")->capture_default_str();
    bool combine_bilingual = false;
    bool combine_text = false;
    combine->add_flag("--bilingual", combine_bilingual,
                      "Treat the second file as the translation of the first");
    combine->add_flag("--text", combine_text,
                      "Second input is a plain-text translation (one line per cue, paired by order)");

    // ---- correct ----
    auto* correct = app.add_subcommand(
        "correct", "Clean up an IR: whitespace, punctuation, timings, rules and glossary");
    std::string correct_ir;
    controller::CorrectCommandOptions correct_opts;
    correct->add_option("ir", correct_ir, "IR file (*.iris.json)")->required();
    correct->add_option("--out,-o", correct_opts.out_path, "Output IR path (default: in place)");
    correct->add_option("--rules", correct_opts.rules_path,
                        "Rule file: `pattern => replacement`, or @macro (see docs/rules/mlp.rules)");
    correct->add_option("--glossary", correct_opts.glossary_path,
                        "Glossary: `<from><TAB><to>` per line");
    correct->add_flag("--dry-run", correct_opts.dry_run, "Show changes without writing");
    correct->add_option("--samples", correct_opts.samples, "How many changed lines to print")
        ->capture_default_str();

    // ---- align ----
    auto* align = app.add_subcommand(
        "align", "Forced alignment: add word/character timings to an IR (Qwen3-ForcedAligner)");
    std::string align_media, align_ir, align_out;
    asr::AlignOptions align_opts;
    bool align_dry = false;
    align->add_option("media", align_media, "Media file (audio/video)")->required();
    align->add_option("--ir", align_ir, "IR file with the transcript")->required();
    align->add_option("--model", align_opts.model,
                      "Aligner GGUF (qwen3-forced-aligner-0.6b-q4_k.gguf)");
    align->add_option("--out,-o", align_out, "Output IR (default: <ir>.aligned.iris.json)");
    align->add_option("--language", align_opts.language, "Language code (informational)")
        ->capture_default_str();
    align->add_option("--threads", align_opts.threads, "Inference threads (0 = auto)")
        ->capture_default_str();
    align->add_option("--pad", align_opts.pad_seconds, "Audio padding per sentence (default 0.2s)")
        ->capture_default_str();
    align->add_flag("--dry-run", align_dry, "Decode audio and report ranges only");

    // ---- diff ----
    auto* diff = app.add_subcommand("diff", "Compare an IR against a reference subtitle");
    std::string diff_ir, diff_ref, diff_html;
    double diff_min = 0.3;
    bool diff_cjk = false;
    diff->add_option("ir", diff_ir, "IR file (*.iris.json)")->required();
    diff->add_option("--reference,-r", diff_ref, "Reference subtitle (.ass/.srt/.vtt)")
        ->required();
    diff->add_option("--html,-o", diff_html, "Output HTML report (default: <ir>.diff.html)");
    diff->add_option("--min-overlap", diff_min, "Minimum time overlap ratio (default 0.3)")
        ->capture_default_str();
    diff->add_flag("--cjk-only", diff_cjk, "Only compare lines that contain Chinese");

    // ---- karaoke ----
    auto* karaoke = app.add_subcommand(
        "karaoke", "Generate \\K timing: project a translation onto the source line's rhythm");
    std::string karaoke_ir, karaoke_out;
    bool karaoke_even = false;
    bool karaoke_overwrite = false;
    int karaoke_tol = 30;
    karaoke->add_option("ir", karaoke_ir, "IR file (*.iris.json)")->required();
    karaoke->add_option("--out,-o", karaoke_out, "Output IR (default: <ir>.karaoke.iris.json)");
    karaoke->add_flag("--even", karaoke_even, "Even-split lines that have no timed source line");
    karaoke->add_flag("--overwrite", karaoke_overwrite, "Recompute existing translatedRaw");
    karaoke->add_option("--tolerance-ms", karaoke_tol,
                        "Time tolerance for matching the timed line (default 30)")
        ->capture_default_str();

    // ---- style ----
    auto* style = app.add_subcommand("style", "Style template tools");
    auto* style_extract =
        style->add_subcommand("extract", "Extract a style template from an ASS/SSA file");
    std::string style_in, style_out, style_name;
    style_extract->add_option("ass", style_in, "ASS/SSA file")->required();
    style_extract->add_option("--out,-o", style_out,
                              "Output template JSON (default: <ass>.theme.json)");
    style_extract->add_option("--name", style_name, "Template name");

    // ---- project ----
    auto* project_cmd = app.add_subcommand("project", "Batch tools: scan a library, run the pipeline");
    auto* p_scan = project_cmd->add_subcommand(
        "scan", "Read-only inventory (events / karaoke coverage / issues)");
    std::vector<std::string> scan_inputs;
    std::string scan_html, scan_json;
    bool scan_no_recursive = false;
    p_scan->add_option("paths", scan_inputs, "Files or directories")->required();
    p_scan->add_option("--html,-o", scan_html, "HTML dashboard");
    p_scan->add_option("--json-out", scan_json, "JSON manifest");
    p_scan->add_flag("--no-recursive", scan_no_recursive, "Do not recurse into subdirectories");

    auto* p_run = project_cmd->add_subcommand(
        "run", "Batch convert → rules → correct → karaoke → build → lint");
    std::vector<std::string> proj_inputs;
    iris::project::RunOptions proj_opts;
    std::string proj_manifest, proj_html;
    p_run->add_option("paths", proj_inputs, "Files or directories")->required();
    p_run->add_option("--out-dir", proj_opts.out_dir, "Output directory (default: iris-out)")
        ->capture_default_str();
    p_run->add_option("--template,-t", proj_opts.template_path, "Style template (.json/.ass)");
    p_run->add_option("--rules", proj_opts.rules_path, "Rule file");
    p_run->add_option("--glossary", proj_opts.glossary_path, "Glossary");
    p_run->add_flag("--even", proj_opts.fill_even, "Even-split lines without a rhythm source");
    p_run->add_flag("--force", proj_opts.force, "Rebuild unchanged files");
    p_run->add_option("--manifest", proj_manifest,
                      "Manifest path (default: <out-dir>/manifest.json)");
    p_run->add_option("--html", proj_html, "HTML report (default: <out-dir>/project.html)");

    // ---- extension registry ----
    auto* plugin_cmd = app.add_subcommand("plugin", "Inspect built-in extensions");
    auto* plugin_list = plugin_cmd->add_subcommand("list", "List registered operations");
    auto* plugin_validate = plugin_cmd->add_subcommand(
        "validate", "Validate a pipeline manifest against the registered operations");
    std::string plugin_manifest_path;
    plugin_validate->add_option("manifest", plugin_manifest_path,
                                "Pipeline JSON manifest")->required();

    // ---- pipeline manifest ----
    auto* run_pipeline_cmd = app.add_subcommand("run", "Execute a validated pipeline manifest");
    std::string pipeline_path, pipeline_input, pipeline_language = "en";
    std::string pipeline_engine = "whisper", pipeline_model, pipeline_output, pipeline_render_output;
    run_pipeline_cmd->add_option("pipeline", pipeline_path, "Pipeline JSON manifest")->required();
    run_pipeline_cmd->add_option("input", pipeline_input, "Input media file")->required();
    run_pipeline_cmd->add_option("--language,-l", pipeline_language, "Transcription language");
    run_pipeline_cmd->add_option("--engine,-e", pipeline_engine, "ASR backend ID");
    run_pipeline_cmd->add_option("--model,-m", pipeline_model, "ASR model path");
    run_pipeline_cmd->add_option("--output,-o", pipeline_output, "Output IR path");
    run_pipeline_cmd->add_option("--render-output", pipeline_render_output,
                                 "Copy the build node's subtitle artifact to this path");

    // ---- init ----
    app.add_subcommand("init", "Interactive setup (stub, planned)");

    CLI11_PARSE(app, argc, argv);

    // --agent 要在日志初始化之前设定，才能让日志格式也变成纯文本
    // IRIS_AGENT=1 让子进程（如 serve 派发的任务）也保持 agent 输出
    if (const char* agent_env = std::getenv("IRIS_AGENT");
        agent_env != nullptr && *agent_env != '0' && *agent_env != '\0') {
        agent = true;
    }
    if (json_output) iris::ui::set_json_mode(true);
    if (agent) iris::ui::set_agent_mode(true);
    infra::init_logger(verbose);

    // 配置：--config / IRIS_CONFIG / 用户目录（首次运行会生成）
    auto cfg = model::load_active_config(config_path);
    if (verbose) {
        spdlog::info("config: {}", model::active_config_path());
        spdlog::info("models: {}", cfg.models_dir);
    }

    // 分发到 MVC Controller 层（包成 lambda，便于统一兜底 JSON 输出）。
    const int rc = [&]() -> int {
    if (asr->parsed()) {
        // 配置里的默认值：--model 未给时用 asr.model，--split-model 仍是默认目录时用 sat.modelDir
        if (asr_model.empty()) asr_model = cfg.resolve(cfg.asr_model);
        if (asr_split_model == "models/sat") asr_split_model = cfg.resolve(cfg.sat_model_dir);
        asr::SeparateOptions separate_opts;
        const asr::SeparateOptions* separate_ptr = nullptr;
        if (asr_separate) {
            separate_opts.model = asr_separation_model.empty() ? asr::default_separation_model()
                                                               : asr_separation_model;
            separate_opts.chunk_seconds = asr_sep_chunk;
            if (separate_opts.model.empty()) {
                std::cerr << _("asr: --separate needs an HTDemucs model\n",
                               "asr: --separate 需要 HTDemucs 模型\n");
                return 1;
            }
            separate_ptr = &separate_opts;
        }
        if (!asr_diarize) {
            return controller::run_asr(asr_input, asr_lang, asr_engine, asr_model, asr_output,
                                      nullptr, asr_threads, separate_ptr,
                                      asr_punc ? resolve_punc_model(asr_punc_model) : std::string(),
                                      asr_split, asr_split_model);
        }
        asr::DiarizeOptions diarize;
        diarize.method = asr_diarize_method;
        diarize.embedder = asr_speaker_model.empty() ? asr::default_embedder_model()
                                                     : asr_speaker_model;
        diarize.model = diarize.method == 4 ? std::string() : diarize.embedder;
        diarize.num_speakers = asr_speakers;
        if (diarize.method == 4 && diarize.embedder.empty()) {
            std::cerr << _("asr: --diarize needs a speaker embedding model "
                           "(--speaker-model, or put wespeaker*.gguf in ~/.cache/crispasr)\n",
                           "asr: --diarize 需要说话人嵌入模型（--speaker-model，"
                           "或把 wespeaker*.gguf 放到 ~/.cache/crispasr）\n");
            return 1;
        }
        if (asr_separate) {
            separate_opts.model = asr_separation_model.empty() ? asr::default_separation_model()
                                                               : asr_separation_model;
            separate_opts.chunk_seconds = asr_sep_chunk;
            if (separate_opts.model.empty()) {
                std::cerr << _("asr: --separate needs an HTDemucs model "
                               "(--separation-model, or put htdemucs*.gguf in ~/.cache/crispasr)\n",
                               "asr: --separate 需要 HTDemucs 模型（--separation-model，"
                               "或把 htdemucs*.gguf 放到 ~/.cache/crispasr）\n");
                return 1;
            }
            separate_ptr = &separate_opts;
        }
        return controller::run_asr(asr_input, asr_lang, asr_engine, asr_model, asr_output, &diarize,
                                   asr_threads, separate_ptr,
                                   asr_punc ? resolve_punc_model(asr_punc_model) : std::string(),
                                   asr_split, asr_split_model);
    }
    if (build->parsed()) {
        if (!build_shift.empty()) {
            std::int64_t ms = 0;
            if (!parse_shift_ms(build_shift, ms)) {
                std::cerr << _("--shift format: +1:10:27.30 | -0:00:02.500 | +4227.3\n",
                               "--shift 格式应为 +1:10:27.30 | -0:00:02.500 | +4227.3\n");
                return 1;
            }
            build_opts.shift_ms = ms;
        }
        build_opts.crlf = !build_lf;
        return controller::run_build(build_ir, build_fmt, build_out, build_opts);
    }
    if (validate->parsed()) return controller::run_validate(v_ir);
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
        if (dub_opts.model.empty()) dub_opts.model = cfg.resolve(cfg.tts_model);
        if (dub_opts.codec.empty()) dub_opts.codec = cfg.resolve(cfg.tts_codec);
        if (dub_opts.ref_wav.empty() && !cfg.tts_ref_wav.empty()) dub_opts.ref_wav = cfg.tts_ref_wav;
        if (dub_opts.ref_text.empty()) dub_opts.ref_text = cfg.tts_ref_text;
        dub_opts.fit_timing = !no_fit;
        return controller::run_dub(dub_opts);
    }
    if (translate->parsed()) {
        if (translate_opts.model.empty()) translate_opts.model = cfg.resolve(cfg.translate_model);
        if (translate_opts.engine == "auto" && !cfg.translate_engine.empty()) {
            translate_opts.engine = cfg.translate_engine;
        }
        return controller::run_translate(translate_opts);
    }
    if (correct->parsed()) return controller::run_correct(correct_ir, correct_opts);
    if (diff->parsed())
        return controller::run_diff(diff_ir, diff_ref, diff_html, diff_min, diff_cjk);
    if (align->parsed()) {
        align_opts.dry_run = align_dry;
        if (!align_dry && align_opts.model.empty()) {
            std::cerr << _("align: --model is required (qwen3-forced-aligner-0.6b-q4_k.gguf)\n",
                           "align: 需要 --model（qwen3-forced-aligner-0.6b-q4_k.gguf）\n");
            return 1;
        }
        return controller::run_align(align_media, align_ir, align_out, align_opts);
    }
    if (p_scan->parsed()) {
        iris::project::ScanOptions scan_opts;
        scan_opts.recursive = !scan_no_recursive;
        return controller::run_project_scan(scan_inputs, scan_opts, scan_html, scan_json);
    }
    if (p_run->parsed())
        return controller::run_project_run(proj_inputs, proj_opts, proj_manifest, proj_html);
    if (plugin_list->parsed()) return controller::run_plugin_list();
    if (plugin_validate->parsed())
        return controller::run_plugin_validate(plugin_manifest_path);
    if (run_pipeline_cmd->parsed()) {
        if (pipeline_model.empty()) pipeline_model = cfg.resolve(cfg.asr_model);
        return controller::run_pipeline(pipeline_path, pipeline_input, pipeline_language,
                                        pipeline_engine, pipeline_model, pipeline_output,
                                        pipeline_render_output);
    }
    if (karaoke->parsed())
        return controller::run_karaoke(karaoke_ir, karaoke_out, karaoke_even, karaoke_overwrite,
                                       karaoke_tol);
    if (config_path_cmd->parsed()) {
        std::cout << model::active_config_path() << '\n';
        return 0;
    }
    if (config_show->parsed()) {
        std::cout << cfg.to_json().dump(2) << '\n';
        return 0;
    }
    if (config_init->parsed()) {
        std::cout << model::active_config_path() << '\n';
        cfg.save(model::active_config_path());
        return 0;
    }
    if (from_script->parsed()) return controller::run_from_script(from_script_opts);
    if (serve->parsed()) {
        return controller::run_serve(serve_host, serve_port, cfg.models_dir, serve_work, json_output);
    }
    if (models_list->parsed()) return controller::run_models_list(cfg.models_dir);
    if (models_mirror.empty() && !cfg.hf_mirror.empty()) models_mirror = cfg.hf_mirror;
    if (models_install->parsed())
        return controller::run_models_install(install_keys, cfg.models_dir, models_mirror, install_all,
                                              install_category, install_dry);
    if (quality->parsed()) return controller::run_quality(quality_ir, quality_out);
    if (style_extract->parsed())
        return controller::run_style_extract(style_in, style_out, style_name);
    if (lint->parsed()) {
        lint_opts.check_fonts = !lint_no_fonts;
        return controller::run_lint(lint_inputs, lint_opts, lint_html);
    }
    if (ocr->parsed()) {
        if (ocr_opts.model_dir == "models/ocr") ocr_opts.model_dir = cfg.resolve(cfg.ocr_model_dir);
        return controller::run_ocr(ocr_opts);
    }
    if (convert->parsed()) return controller::run_convert(convert_inputs, convert_lang, convert_out);
    if (combine->parsed()) {
        if (combine_text && !combine_bilingual) {
            std::cerr << _("combine: --text requires --bilingual\n",
                           "combine: --text 需要配合 --bilingual\n");
            return 1;
        }
        return controller::run_combine(combine_inputs, combine_lang, combine_out, combine_bilingual,
                                       combine_text);
    }
    if (app.get_subcommand("doctor")->parsed()) return controller::run_doctor();
    if (app.get_subcommand("init")->parsed()) {
        std::cout << _("init: interactive setup coming soon\n", "init: 交互式设置将在后续版本实现\n");
        return 0;
    }

    // 无子命令
    std::cout << app.help() << '\n';
    return 0;
    }();

    // JSON 模式兜底：确保 stdout 恰好一个 JSON 文档（命令自己没输出时补一个）
    if (iris::ui::json_mode() && !iris::ui::json_was_emitted()) {
        iris::ui::json_out(nlohmann::json({{"ok", rc == 0}, {"exitCode", rc}}).dump(2));
    }
    return rc;
}

} // namespace iris::cli
