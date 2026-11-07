# Iris operational handoff

This is a concise guide for the next contributor or automation session. The
root [README](../README.md) is the user-facing reference; this file records
implementation invariants and recovery steps.

## Project shape

```text
media / subtitle / script / hard subtitle
  -> convert / asr / from-script / ocr
  -> IR
  -> correct / translate / align / karaoke
  -> build / lint / quality / dub
```

Key locations:

| Path | Responsibility |
|---|---|
| `src/cli/cli.cpp` | CLI declarations and dispatch |
| `src/controller/` | Command orchestration |
| `src/ops/` | Pipeline operations, including VSF |
| `src/ui/console.cpp` | Console, agent, JSON, progress |
| `src/infra/log.cpp` | Console/file logging |
| `models/` | Local ignored model files |
| `tools/runtimes/` | Detected runtime DLLs |
| `docs/` | Project documentation and pipeline examples |

## Non-negotiable invariants

1. Keep inference thread counts bounded; use the existing thread resolver.
2. Preserve IR schema `1.0` and add fields rather than breaking old files.
3. Use UTF-8 path helpers on Windows; do not construct filesystem paths from
   UTF-8 `std::string` directly.
4. Keep stdout machine-readable in `--json` and plugin protocol modes.
5. Treat VSF timing as a visual envelope; do not silently replace ASR
   sentence boundaries with it.
6. Surface errors through logs and non-zero exits; do not create empty
   success-shaped artifacts.
7. Preserve existing uncommitted work unless the task explicitly asks for a
   conflict resolution.

## Build and test

```powershell
cmake -S . -B build -DIRIS_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

If direct Windows test execution reports `0xc0000135`, prepend
`build\bin\Release` to `PATH`. Model-dependent tests also need their models
and a repository-root working directory.

## Logging and diagnostics

Default logs are written to `.cache/iris/iris.log`. Use
`--log-file PATH` or `IRIS_LOG_FILE` for a persistent location. Console colors
are limited to white, gray, warning yellow, and error red. `--agent` removes
color and timestamps; `--json` reserves stdout for one result document.

For timing investigations, compare:

- ASR sentence `startMs`/`endMs`;
- forced-aligner word spans;
- VSF segment start/end and confidence;
- final rendered cue boundaries.

## Known limitations

- ONNX/CT2/CrispASR capabilities depend on configure-time runtimes.
- OCR quality depends on subtitle contrast, ROI, frame rate, and model output.
- T3 plugin permissions are audit metadata only; package signing,
  installation, T2 loading, and OS sandboxing are not provided.
- Full test discovery can fail before execution when runtime DLLs are absent.

## Recovery checklist

1. Run `iris doctor` and `iris plugin list`.
2. Validate the input with FFprobe.
3. Reuse an existing IR instead of rerunning ASR.
4. Run `iris lint` or `iris quality` before delivery.
5. Keep generated IR, reports, logs, and media outside Git.
