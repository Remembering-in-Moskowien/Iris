# SenseVoice / Paraformer ONNX 转录后端（已实现）

两个模型都是 **非自回归 CTC**，共用同一套前处理，非常适合做 iris 的第二个「自写 ONNX ASR」
（现有 `whisper-onnx` 走 encoder/decoder，这两个只要一次前向 + 贪心解码）。

模型已下载到 `models/sensevoice/`、`models/paraformer/`（均在 gitignore 内）：

| 目录 | 文件 | 大小 |
|---|---|---|
| `models/sensevoice` | `model.int8.onnx` + `tokens.txt` | 239 MB / 316 KB |
| `models/paraformer` | `model.int8.onnx` + `tokens.txt` | 227 MB / 75 KB |

来源：`csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17`、
`csukuangfj/sherpa-onnx-paraformer-zh-2024-03-09`（int8 量化版）。

## ONNX I/O（读自模型图）

**SenseVoice**

```
in  x          [N, T, 560]  float32   ← 80 mel × LFR 7
in  x_length   [N]          int32
in  language   [N]          int32     0=auto 3=zh 4=en 7=yue 11=ja 12=ko 13=nospeech
in  text_norm  [N]          int32     14=with_itn 15=without_itn
out logits     [N, T, 25055] float32  ← CTC，每帧一个 token id
```

**Paraformer**

```
in  speech         [B, T, 560] float32
in  speech_lengths [B]         int32
out logits         [B, T', 8359] float32  ← CTC（T' 可能不等于 T，取 token_num 截断）
out token_num      [B]           int32
```

两者 `metadata_props` 都给出：`lfr_window_size=7`、`lfr_window_shift=6`、
`neg_mean`（560 个）、`inv_stddev`（560 个）、`vocab_size`。

## 前处理（kaldi 兼容 fbank + LFR + CMVN）

1. **PCM**：16 kHz 单声道（复用 `read_wav_16k_mono`）。
2. **fbank 80 维**：25 ms 窗 / 10 ms 移，`povey` 窗（`(0.5 - 0.5cos(2πn/(N-1)))^0.85`），
   预加重 0.97，`remove_dc_offset`，`snip_edges=false`（两端补边），
   功率谱 → mel 滤波器组（kaldi 的 mel 公式 `1127 ln(1+f/700)`，0–8000 Hz 均分 80 带）→ 自然对数。
3. **LFR**：每 7 帧拼成 560 维，移 6 帧；不足 7 帧时用最后一帧补齐。
4. **CMVN**：`(x + neg_mean) * inv_stddev`（逐维，560 维）。

## 解码

- **CTC 贪心**：对每帧取 argmax，去掉 blank（id 0）与连续重复。
- **SenseVoice**：输出前缀含 `<|zh|><|NEUTRAL|><|Speech|><|withitn|>` 这类标签 token，
  需要按 `<|...|>` 过滤掉；`tokens.txt` 是「token id → 文本」两列。
- **Paraformer**：`@@` 结尾的子词要与后续拼接；遇到 `</s>`/`<blank>` 跳过。

## 集成方式（与 whisper-onnx 对齐）

- `include/iris/asr/onnx_funasr.hpp` + `src/asr/onnx_funasr.cpp`：
  `class FunasrOnnxEngine`，负责模型加载、fbank/LFR/CMVN、一次前向、CTC 解码。
- `asr::AsrBackend` 增加 `SenseVoiceOnnx`、`ParaformerOnnx`；`parse_asr_backend` 接受
  `sensevoice-onnx`、`paraformer-onnx`；`asr_backend_available` 跟随 `IRIS_HAS_ONNX`。
- `--model` 传模型**目录**（含 `model.int8.onnx` 或 `model.onnx` + `tokens.txt`）。
- 输出 `model::Sentence`：整段一个句子（首尾时间码由帧数与 10 ms 帧移推算）；
  如需分句，交给现有 `SentenceSplit`。
- 复用 `infra::resolve_threads()` 设 `SetIntraOpNumThreads`（ONNX 默认会吃满所有核）。

## 实测（已验收）

- SenseVoice / jfk.wav（英文）：`And so my fellow Americans ask not what your country can do for you, ask what you can do for your country.` —— 全对，**2.5 s**（含模型加载）。
- Paraformer / 中文配音片段（10.2 s）：`所以我的美国同胞们不要问你们的国家能为你做什么问什么您可以为您的国家做点什么` —— **8.4 s**。
- `ctest`：`funasr: fbank / LFR / CTC helpers` 用例锁住帧数公式
  （1 s → 100 帧 × 80 维 → LFR 17 帧 × 560 维）与 CTC 拼接规则。
- 单测：fbank 帧数（`T = 1 + samples/160`）、LFR 帧数、CMVN 零均值近似、
  CTC 去重/去 blank、`<|...|>` 过滤。
