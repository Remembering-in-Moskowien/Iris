# Iris capability and gap analysis

This document separates implemented behavior from intentionally limited
behavior. It is not a release promise; verify the current binary with
`iris doctor`, `iris models list`, and `iris plugin list`.

## Implemented core

- C++20 CLI and IR 1.0 serialization;
- media conversion, preprocessing, ASR, sentence splitting, and optional
  diarization;
- six ASR backend IDs, subject to runtime availability;
- SRT/ASS/VTT/container text import;
- bilingual merge/reconciliation and ASS/SRT/TXT rendering;
- OCR hard-subtitle extraction with VSF frame detection;
- forced alignment, karaoke timing, correction, diff, lint, and quality;
- Qwen3 TTS, dubbing, project scan/run, REST serving, and T3 process plugins;
- model catalog/list/install workflows, including manual-preparation reporting;
- pipeline manifest validation/execution and generated T3 plugin scaffolding.

## Deliberate boundaries

- Graphical subtitle tracks are not imported directly; use OCR.
- VSF is a visual timing envelope, not speech recognition and not a semantic
  subtitle parser.
- `permissions` in plugin manifests are declarations for audit, not an OS
  security boundary.
- T2 C ABI plugin loading, plugin package signing/verification, and OS-level
  plugin sandboxing are not provided. `iris plugin new` creates a scaffold;
  it is not a package installer.
- Cloud model downloads and mirror management are configuration concerns, not
  implicit runtime behavior.
- Parallel pipeline execution is not assumed for inference operations.

## Compatibility risks

Runtime availability varies by build because CrispASR, CTranslate2, and ONNX
Runtime components are detected during configuration. A manifest can therefore
be structurally valid but unavailable on a particular machine.

Subtitle quality remains dependent on model output, source audio, language,
and timing density. `lint`, `quality`, visual review, and comparison against
reference tracks remain required for production delivery.

## Priorities

Near-term work should focus on:

1. improving hard-subtitle detection and OCR confidence filtering;
2. exposing more timing diagnostics in reports;
3. splitting oversized CLI/test translation units;
4. defining a secure plugin package, signature, and permission model before
   adding any privileged plugin loading.
