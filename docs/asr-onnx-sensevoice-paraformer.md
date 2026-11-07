# SenseVoice and Paraformer ONNX backends

Iris includes two CTC-style ONNX backends implemented in-process:
`sensevoice-onnx` and `paraformer-onnx`. They use the same audio preparation
path as the other ASR engines but have model-specific feature and decoding
contracts.

## Model layout

```text
models/sensevoice/
  model.int8.onnx
  tokens.txt

models/paraformer/
  model.int8.onnx
  tokens.txt
```

The exact model graph is inspected at runtime. Do not hard-code input/output
names or vocabulary sizes in a deployment script.

```bash
iris asr input.wav --engine sensevoice-onnx \
  --language zh --model models/sensevoice -o input.iris.json
iris asr input.wav --engine paraformer-onnx \
  --language zh --model models/paraformer -o input.iris.json
```

Aliases `sensevoice` and `paraformer` are accepted when registered by the
current build.

## Processing

The backend performs:

1. mono audio conversion and resampling;
2. Kaldi-compatible fbank extraction;
3. LFR stacking and CMVN;
4. ONNX inference;
5. CTC token decoding and sentence construction.

The implementation copies strided subgraph data into compact buffers before
resizing. This avoids row-stride errors that produce blank or noisy CTC
outputs.

## Runtime and validation

ONNX Runtime is detected at configure time. `iris doctor` and
`iris plugin list` report whether the backend is available. `plugin validate`
and pipeline execution reject a missing runtime or malformed model rather than
silently switching engines.

The backend emits normal IR timing and provenance. Use `align` if finer
word-level timing is required, and inspect the generated text before rendering
production subtitles.
