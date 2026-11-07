# Iris VSF core

Iris includes an optional shared library, `iris_vsf`, for hard-subtitle frame
scoring. The CLI and OCR controller use this library through the stable C ABI
in [`include/iris/vsf/vsf_api.h`](../include/iris/vsf/vsf_api.h).

## Design and provenance

The detection goal and user workflow were informed by
[VideoSubFinder](https://github.com/SWHL/VideoSubFinder), an established
hard-subtitle extraction tool. VideoSubFinder is GPL-2.0 licensed. Iris does
**not** copy or link its source code; `iris_vsf` is an independent
implementation using Iris-owned code and a small C ABI.

The DLL scores a packed RGB24 ROI using visual evidence:

- high-contrast light or yellow text pixels;
- Sobel-style local edges;
- concentration into horizontal text rows;
- horizontal text span.

The host applies adaptive thresholds and temporal hysteresis to turn frame
scores into subtitle timing envelopes. OCR remains a separate stage.

## Build and distribution

The DLL is enabled by default:

```bash
cmake -S . -B build -DIRIS_BUILD_VSF_DLL=ON
cmake --build build --config Release --target iris iris_vsf
cmake --install build --config Release --prefix dist
```

Windows output:

```text
build/bin/Release/iris.exe
build/bin/Release/iris_vsf.dll
build/lib/Release/iris_vsf.lib
```

The runtime DLL is placed beside `iris.exe` so normal Windows DLL lookup
works. Installation places the runtime DLL in `bin`, its import library in
`lib`, and the public C header in `include/iris/vsf`.

`IRIS_BUILD_VSF_DLL` remains an explicit packaging option for build-system
visibility, but disabling it is rejected because `iris_core` requires the
stable VSF ABI. Production distributions must ship the DLL beside the CLI.

## C ABI

```c
int iris_vsf_abi_version(void);
double iris_vsf_score_rgb(const unsigned char* rgb, int width, int height);
```

`iris_vsf_score_rgb` accepts a tightly packed RGB24 buffer and returns a
normalized score in `[0, 1]`. The ABI intentionally exposes scoring rather
than video decoding, OCR, or C++ types, so the DLL can remain stable while
the host owns FFmpeg, timing policy, and output formats.

## Validation

```powershell
iris vsf input.mp4 --roi 0,580,1280,140 --fps 1 -o input.vsf.srt
iris ocr input.mp4 --model-dir models\ocr `
  --roi 0,580,1280,140 --fps 1 -o input.zh.iris.json
```

Compare VSF segment envelopes with ASR `startMs`/`endMs` before using them as
production cue boundaries. VSF is not ASR and does not establish spoken-word
timing.
