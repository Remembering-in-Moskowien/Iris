# Iris architecture

## Overview

Iris is a C++20, local-first media pipeline. Commands convert media or
subtitle sources into a versioned `*.iris.json` document; later commands
consume that document without repeating expensive work.

```text
media / subtitles / script / hard subtitles
                  |
       convert -> transcribe -> split
                  |
              IR 1.0
                  |
 correct -> translate -> align -> karaoke -> build / quality / dub
```

The declarative pipeline runner uses the same operations as the CLI and
executes a validated DAG. The ordinary commands remain available for
interactive and incremental work.

## Layers

| Layer | Responsibility |
|---|---|
| `cli` | CLI11 command declarations and dispatch |
| `controller` | Command orchestration, validation, result output |
| `ops` | Pipeline operations such as conversion, transcription, OCR, and build |
| `core` | Operators, pipeline execution, providers, and extension registries |
| `model` | IR data model, JSON serialization, configuration |
| `view` | ASS/SRT/TXT rendering and style handling |
| `infra` | Processes, UTF-8 paths, logging, runtime detection |
| `ui` | Console presentation, agent mode, JSON mode, progress |

## Core abstractions

### Operators and pipelines

An operator has a stable name, declared dependencies, and an execution
function. The pipeline runner validates missing dependencies and cycles, then
performs a topological execution. Each node declares input/output facets so a
manifest cannot consume an artifact that no ancestor produces.

Parallel execution is not assumed: operations that use CPU inference or
external tools must preserve deterministic ordering and explicit resource
limits.

### Extensions and providers

ASR, TTS, and render implementations use typed static extension registries.
Each registration contains a canonical ID, aliases, runtime availability, and
the type-safe factory or execution entry point. `iris plugin list` reads these
same registrations, so discovery and execution cannot silently diverge.

`ProviderRegistry` remains a metadata catalog. It is not the runtime dispatch
mechanism. T3 process plugins are a separate boundary and communicate over
stdio JSON Lines. Discovery, validation, invocation, and scaffold generation
are implemented; manifests do not grant capabilities. T2 dynamic-library
plugin loading, package signing/verification, and OS-level sandboxing are not
provided.

## IR 1.0

The root document contains:

```json
{
  "schemaVersion": "1.0",
  "generator": {},
  "provenance": [],
  "config": {},
  "state": {
    "tracks": [
      {
        "type": "transcribe",
        "language": "en",
        "sentences": [
          { "startMs": 0, "endMs": 1200, "text": "Hello." }
        ]
      }
    ]
  }
}
```

Sentence timing is stored in milliseconds. Optional words use seconds for
compatibility with the ASR and forced-aligner APIs. Sentence fields include
`translatedText`, `speaker`, `style`, `raw`, `translatedRaw`, `role`, `layer`,
and `comment`. Workflow state also stores styles, script information, timeline
offset, and generated audio paths.

New fields are additive and old IR files remain readable when fields are
missing. `provenance` records the operation, model, parameters hash, and
timestamp needed to audit a generated artifact.

## Timing boundaries

ASR supplies the primary sentence timeline. Forced alignment may refine those
boundaries only when aligned word spans are valid, non-zero, and not
implausibly compressed. The VSF hard-subtitle detector is different: it
produces a visual timing envelope from sampled frames. VSF does not infer
speech or text and must not overwrite ASR sentence boundaries without a
separate reconciliation step.

## Platform decisions

- FFmpeg/FFprobe remain subprocess dependencies for media decoding and frame
  extraction.
- Windows filesystem and child-process paths use UTF-8 conversion helpers;
  direct construction of `std::filesystem::path` from UTF-8 `std::string` is
  unsafe on the supported Windows code page.
- Errors are surfaced through logs and command exit codes; missing runtime
  backends are not silently replaced.
- IR is the exchange contract so expensive stages can be resumed and
  rendered repeatedly.
