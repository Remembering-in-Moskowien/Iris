# Plugins and built-in extensions

Iris has two extension mechanisms:

1. typed, statically compiled registries for built-in ASR, TTS, render, and
   pipeline operations;
2. T3 process plugins that run as separate programs and speak UTF-8 JSON Lines.

## Built-in registries

```bash
iris plugin list
iris plugin list --json
```

The list is generated from the same registrations used for name resolution and
execution. Availability reflects the current build's runtime DLLs and model
support. Current ASR IDs are `whisper`, `qwen3-asr`, `whisper-ct2`,
`whisper-onnx`, `sensevoice-onnx`, and `paraformer-onnx`; current TTS IDs are
`qwen3-tts` and `indextts`, subject to availability.

## Pipeline manifests

Manifests use `apiVersion: 1`:

```json
{
  "apiVersion": 1,
  "nodes": [
    { "id": "convert", "operation": "ffmpeg_convert" },
    { "id": "preprocess", "operation": "audio_preprocess",
      "dependsOn": ["convert"] },
    { "id": "transcribe", "operation": "transcribe",
      "dependsOn": ["preprocess"],
      "parameters": { "engine": "whisper", "model": "models/ggml/base.gguf" } }
  ]
}
```

```bash
iris plugin validate docs/pipeline-asr.json
iris run docs/pipeline-asr.json input.wav -l en \
  -o output.iris.json --render-output output.ass
```

Validation rejects unknown fields/operations, duplicate or missing
dependencies, cycles, unavailable backends, and facet mismatches. The
`--output` option names the IR; `--render-output` copies a build artifact out
of the temporary run directory.

## T3 process plugins

A plugin directory contains `plugin.json` and the executable/script it names:

```json
{
  "id": "acme.check.cps",
  "kind": "check",
  "version": "0.1.0",
  "apiVersion": 1,
  "protocol": "stdio-jsonl",
  "command": "python",
  "args": ["check.py"],
  "timeoutMs": 5000,
  "permissions": ["fs:read"],
  "enabled": true
}
```

The host searches project-local `.iris/plugins` and `plugins`, then the user
data plugin directory. `IRIS_PLUGIN_DIRS` adds explicit search paths. Duplicate
IDs resolve to the first discovered plugin.

The protocol uses one UTF-8 JSON object per line. stdout is reserved for
protocol messages; logs belong on stderr or notifications:

```text
-> {"id":1,"method":"describe","params":{}}
<- {"id":1,"result":{"id":"acme.check.cps","kind":"check","apiVersion":1}}
-> {"id":2,"method":"invoke","params":{"kind":"check","input":{},"params":{}}}
<- {"id":2,"result":{}}
```

`describe` must agree with the manifest. Timeouts and process exits mark a
plugin degraded; the host starts a fresh process for the next invocation.
`plugin new` only generates a local scaffold and does not install, sign, or
approve a plugin.

## CLI and SDK

```bash
iris plugin validate plugins/example-check
iris plugin invoke example.check.cps --method describe
iris plugin invoke example.check.cps --method invoke \
  --input input.json --params '{"maxCps":5}'
iris plugin check input.json --params '{"maxCps":5}'
iris plugin new my.check.rules --kind check --dir plugins
```

`plugins/iris_plugin.py` provides a standard-library-only `Plugin`,
`@plugin`, and `serve()` SDK. The generated example under
`plugins/example-check` is the smallest reference implementation.

## Trust boundary

T3 plugins run with the current user's permissions. Manifest `permissions` are
for discovery and auditing only; they do not enforce filesystem, network, or
process restrictions. Do not load untrusted plugins. Iris currently does not
provide package signing/verification, package installation, OS sandboxing, or
T2 C ABI plugin loading.
