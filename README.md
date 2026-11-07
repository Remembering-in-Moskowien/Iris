# Iris

**Speech in, subtitles out.**

Iris is a local-first media subtitle and dubbing toolkit. It turns media,
subtitle files, scripts, or hard subtitles
into versioned `*.iris.json` intermediate documents, then renders ASS, SRT, or
TXT. The same IR can feed correction, translation, karaoke timing, quality
checks, and dubbing.

## Status and scope

The production path currently covers:

- six ASR backends: `whisper`, `qwen3-asr`, `whisper-ct2`, `whisper-onnx`,
  `sensevoice-onnx`, and `paraformer-onnx`;
- subtitle import from SRT, ASS, VTT, and supported container text tracks;
- bilingual track reconciliation and bilingual ASS/SRT rendering;
- hard-subtitle OCR using a heuristic VSF segment detector and PP-OCRv5;
- forced alignment, karaoke timing, text cleanup, lint/quality reports,
  project ledgers, TTS, dubbing, and T3 process plugins.

Backend availability is build/runtime dependent. Run `iris doctor` and
`iris plugin list` on the target machine before selecting a model.

## Quick start

Requirements:

- CMake 3.24 or newer;
- a C++20 compiler;
- FFmpeg and FFprobe on `PATH` for media decoding and frame extraction;
- the model files required by the selected backend.

```

To reduce storage or transfer size without changing the IR schema, use the
lossless compact writer:

```powershell
iris compact episode.iris.json -o episode.compact.iris.json
iris validate episode.compact.iris.json
```bash
cmake -S . -B build -DIRIS_BUILD_TESTS=ON
cmake --build build --config Release

build/bin/iris doctor
build/bin/iris asr input.mp4 --language en \
  --engine qwen3-asr --model models/qwen3/qwen3-asr.gguf \
  --output input.iris.json
build/bin/iris build input.iris.json --format ass --output input.ass
```

On Windows, `powershell -ExecutionPolicy Bypass -File scripts/bootstrap.ps1`
provides the repository build setup. The executable is usually
`build\bin\Release\iris.exe`.

## Media-to-subtitle workflow

For a single media file, `scripts/process-video.ps1` runs ASR, validates the
IR, and renders ASS and SRT. It supports non-ASCII Windows paths, IR reuse,
and optional Qwen3 forced alignment.

```powershell
powershell -ExecutionPolicy Bypass -File scripts/process-video.ps1 `
  -Media "input.mp4" -OutputDirectory ".cache\media" `
  -Language en -AsrEngine qwen3-asr `
  -AsrModel "models\qwen3\qwen3-asr.gguf" -Align
```

Use `-ReuseIr` when ASR is already complete. Alignment only replaces sentence
boundaries when the word spans pass validity checks; otherwise the original
ASR timing is retained.

## Hard subtitles and OCR

Use `ocr` when the source video contains burned-in subtitles:

```powershell
iris ocr "video-with-hard-subs.mp4" `
  --model-dir models\ocr --roi 0,580,1280,140 --fps 1 `
  --output subtitles.zh.iris.json
iris build subtitles.zh.iris.json --format srt --output subtitles.zh.srt
```

The VSF detector is a heuristic timing-envelope detector, not an OCR engine
and not a replacement for ASR sentence boundaries. It combines brightness,
edge density, horizontal text span, and row concentration, then applies an
adaptive threshold and temporal hysteresis. Its output should be compared
with the ASR timeline before using it as a final cue list.

If VSF finds too few segments for a hard-subtitle video, OCR falls back to
fixed-rate sampling and lets OCR text changes determine cue boundaries.

## Bilingual subtitles

For two independent subtitle tracks:

```bash
iris merge primary.en.iris.json secondary.zh.iris.json \
  --primary-lang en --secondary-lang zh \
  --report merge.html -o bilingual.iris.json
iris build bilingual.iris.json --format ass --bilingual -o bilingual.ass
iris build bilingual.iris.json --format srt --bilingual -o bilingual.srt
```

For container tracks, inspect and import without manually extracting streams:

```bash
iris tracks episode.mkv
iris convert episode.mkv --track lang=eng -o episode.en.iris.json
iris convert episode.mkv --track lang=chi -o episode.zh.iris.json
```

`merge` uses the first input as the event skeleton and performs monotonic
global reconciliation. Candidate groups are filtered by temporal IoU, with
optional semantic similarity used to disambiguate ambiguous paths; uncertain
matches are left unpaired for review rather than forced. Use
`--min-iou` and `--min-alignment-confidence` to tighten the precision-first
gates. It supports 1:1, 1:n, n:1, single-language policies, speaker labels,
annotations, and glossary-assisted lines. See
[docs/bilingual-tracks.md](docs/bilingual-tracks.md).

### Optional LLM translation post-editing

The `translate` command always performs the configured OPUS-MT/Marian (or
other MT backend) translation first. An OpenAI-compatible chat endpoint can
then be enabled to compare the original and machine translation and return a
faithful, subtitle-friendly revision:

```powershell
iris translate input.iris.json --model models\translation\opus-mt-en-zh-f16.gguf `
  --engine opus-mt --from en --to zh `
  --llm-endpoint http://127.0.0.1:8080/v1/chat/completions `
  --llm-model local-model --out translated.iris.json
```

LLM failures are logged as warnings and retain the machine translation.
Omit `--llm-endpoint` to keep the original MT-only behavior. Use
`IRIS_LLM_API_KEY` or `--llm-api-key` for authentication; keys are not logged.

## Rendering, QA, and timing

```bash
iris build input.iris.json --format ass --style-sheet theme.json
iris build input.iris.json --format srt
iris lint subtitles/ --fonts fonts/ --output lint.html
iris quality input.iris.json --output quality.html
iris align video.mkv --ir input.iris.json \
  --model models/aligner/qwen3-forced-aligner-0.6b-q4_k.gguf
iris karaoke bilingual.iris.json --even --even-syllables -o karaoke.iris.json
iris correct input.iris.json --rules docs/rules/mlp.rules
iris diff input.iris.json --reference reference.ass
```

`--style-sheet` accepts a JSON style sheet path. `--template` remains
available for `.json`/`.ass`/`.ssa` templates. TADC character styles are
automatically loaded from the packaged `themes/tadc.json` when the IR
references a TADC character style; otherwise the built-in default styles are
used.

`lint` is read-only and targets ASS/SSA artifacts. It checks styles, fonts,
language/style mismatches, timing overlap, CPS, line length, duplicate/empty
events, and karaoke coverage. `quality` produces an IR report and returns a
non-zero status when configured error findings exist.

## Dubbing and TTS

```bash
iris tts --model models/qwen3-tts/talker.gguf \
  --codec models/qwen3-tts/tokenizer.gguf \
  --text "Hello there" --out hello.wav

iris dub input.mp4 --engine qwen3-tts \
  --model models/qwen3-tts/talker.gguf \
  --codec models/qwen3-tts/tokenizer.gguf \
  --ref reference.wav --ref-text "Exact reference transcript" \
  --out dub.wav --ir-out dub.iris.json
```

Reference audio and reference text must match exactly. Dubbing fits each
utterance into its source time slot, preserves a configured inter-sentence
gap, and records timing adjustments in the log. It does not remux or modify
the source video.

## CLI and output modes

Useful commands include `asr`, `ocr`, `convert`, `combine`, `merge`, `tracks`,
`build`, `lint`, `quality`, `align`, `karaoke`, `correct`, `diff`, `project`,
`models`, `plugin`, `run`, `serve`, `tts`, and `dub`.

The global options are:

```text
-v, --verbose              enable debug logging
--log-file PATH            write logs to PATH
--agent                    stable INFO/OK/WARN/ERROR text for automation
--json                     emit one machine-readable result document on stdout
```

Without `--log-file`, logs are written to `.cache/iris/iris.log`. Console
semantic colors are deliberately limited to white (normal/success), gray
(secondary/debug), warning yellow, and error red. `NO_COLOR` and
`IRIS_PLAIN=1` disable color. `--json` keeps stdout parseable and sends logs
to stderr. `--agent` removes colors, animation, and timestamps.

Windows paths are passed to child processes as UTF-8/UTF-16 safely. Prefer
`infra::utf8_path` and `infra::path_to_utf8` in new filesystem code.

## Models and runtimes

Models are not bundled. The selected engine determines the required format:

| Engine | Model format |
|---|---|
| `whisper`, `qwen3-asr` | ggml/GGUF |
| `whisper-ct2` | CTranslate2 directory |
| `whisper-onnx` | Whisper ONNX directory and tokenizer |
| `sensevoice-onnx`, `paraformer-onnx` | ONNX model plus token file |
| `qwen3-tts` | talker GGUF plus tokenizer GGUF |

`tools/runtimes/` is detected at configure time. Missing CrispASR,
CTranslate2, or ONNX Runtime components make only the dependent backends
unavailable; Iris does not silently fall back to another engine.

## Configuration and artifacts

The active configuration is stored under the Iris data directory
(`IRIS_HOME` can override it). `iris config show` prints the effective
configuration. Models, generated IR, rendered subtitles, HTML reports, and
logs should remain outside Git; `.cache/iris/` is the default scratch area.

IR is schema version `1.0` and stores generator metadata, provenance,
configuration, tracks, sentence timing, optional words, styles, and output
state. See [docs/architecture.md](docs/architecture.md).

## Tests and development

```bash
cmake -S . -B build -DIRIS_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

When running test executables directly on Windows, add
`build\bin\Release` to `PATH` so runtime DLLs can be found. Model-dependent
tests may require a repository-root working directory and installed models.

## Plugins and pipelines

Built-in ASR/TTS/render extensions use typed static registries. T3 process
plugins are separate programs speaking UTF-8 JSON Lines over stdin/stdout.
Discovery, validation, invocation, and local scaffold generation are
implemented:

```bash
iris plugin list
iris plugin validate docs/pipeline-asr.json
iris plugin invoke example.check.cps --method describe
iris plugin check input.json --params '{"maxCps":5}'
iris run docs/pipeline-asr.json input.wav -l en -o output.iris.json
```

`iris plugin new` generates a scaffold but does not install or sign it.
Process plugins run with the current user's permissions. Manifest permissions
are declarations for discovery and auditing, not an OS sandbox. Package
signing/verification, package installation, T2 C ABI loading, and OS
sandboxing are not provided. See
[docs/plugins.md](docs/plugins.md) and [plugins/README.md](plugins/README.md).

## Documentation map

- [Architecture and IR](docs/architecture.md)
- [Bilingual tracks](docs/bilingual-tracks.md)
- [ASR ONNX backends](docs/asr-onnx-sensevoice-paraformer.md)
- [VSF hard-subtitle core DLL](docs/vsf.md)
- [Plugins and pipelines](docs/plugins.md)
- [Gap analysis](docs/gap-analysis.md)
- [Handoff and operational notes](docs/HANDOFF.md)

## License

See the repository license files and third-party notices before redistributing
builds that include bundled runtime components.
