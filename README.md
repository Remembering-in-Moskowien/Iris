# iris

**Speech in, subtitles out.** iris 是 Centurion（.NET 10）的 **C++ 迁移项目**：
把音视频转成高质量 ASS 字幕，并逐步覆盖说话人分离、机器翻译、人声分离与机器配音
（配音链路已用 **qwen3-tts** 打通，IndexTTS 在编排中）。默认完全本地运行，GPU 感知。

> 原仓库 `Centurion` 已设为 private，保留为 C# 参考实现与历史基线；
> 新代码在 `iris`（本仓库）中以 C++ 继续演进。

![lang](https://img.shields.io/badge/language-C%2B%2B20-2ea44f)
![build](https://img.shields.io/badge/build-CMake-blue)
![status](https://img.shields.io/badge/status-migrating-yellow)

---

## 快速开始

```bash
# 1. 配置并构建（需要 CMake ≥ 3.20 + C++20 编译器）
cmake -S . -B build -DIRIS_BUILD_TESTS=ON
cmake --build build --config Release

# 2. 转写音视频到中间文件（IR；运行 asr 需要 ffmpeg，且需自备 ASR 模型）
build/bin/iris asr demo.mp4 --language en --model /path/to/ggml-base.bin

# 3. 渲染字幕
build/bin/iris build demo.mp4.iris.json --format ass

# 4. 环境诊断
build/bin/iris doctor
```

Windows 一键构建：`powershell -ExecutionPolicy Bypass -File scripts/bootstrap.ps1`。

### ASR 模型与引擎（模型名 → 内嵌推理项目）

ASR 推理全部在进程内完成，**不依赖外部 ASR 可执行文件**（不再调用 whisper-cli 等）：

| 模型名（`--engine`） | 模型文件 | 推理项目（CMake 内嵌） |
|---|---|---|
| `whisper`（默认） | ggml GGUF（如 ggml-base.bin） | [CrispStrobe/CrispASR](https://github.com/CrispStrobe/CrispASR)（ggml） |
| `qwen3-asr` | ggml GGUF（qwen3-asr 模型） | [CrispStrobe/CrispASR](https://github.com/CrispStrobe/CrispASR)（ggml） |
| `whisper-ct2` | CTranslate2 模型目录（`ct2-transformers-converter` 转换） | [OpenNMT/CTranslate2](https://github.com/OpenNMT/CTranslate2)（transformer） |
| `sensevoice-onnx` | 目录：`model.int8.onnx` + `tokens.txt` | **Iris 自写 ONNX 编排**（kaldi fbank + LFR + CMVN + CTC） |
| `paraformer-onnx` | 目录：`model.int8.onnx` + `tokens.txt` | 同上（中文，非自回归 CTC） |

```bash
build/bin/iris asr demo.mp4 --engine qwen3-asr --language zh --model /path/to/qwen3-asr.gguf
build/bin/iris asr demo.mp4 --engine whisper-ct2 --language en --model /path/to/whisper-ct2-model/
build/bin/iris asr demo.mp4 --engine sensevoice-onnx --language en --model models/sensevoice
build/bin/iris asr demo.wav --engine paraformer-onnx --language zh --model models/paraformer
```

- ggml 模型（whisper / qwen3-asr）需自行准备：whisper GGUF 可从 whisper.cpp / Hugging Face
  获取；qwen3-asr GGUF 可从 CrispASR / qwen3-asr.cpp 的发布页获取。
- whisper-ct2 需先用 `ct2-transformers-converter` 将 Hugging Face 的 whisper 模型
  转换为 CTranslate2 格式的目录。
- 自动下载与镜像加速（hf-mirror / gh-proxy.com 及 `downloadProxy`）尚未实现，
  规划中；模型与工具（ffmpeg 等）目前一律由用户自行准备。

### 构建选项

| CMake 选项 | 默认 | 说明 |
|---|---|---|
| `IRIS_BUILD_TESTS` | ON | 构建 Catch2 测试 |
| `IRIS_RUNTIMES_DIR` | `tools/runtimes` | 打包运行时 DLL 目录（crispasr / ct2 / onnx） |
| `IRIS_BUILD_NUMBER` | 空 | 构建号（CI 注入 GitHub run number，release tag = `v{run_number}`） |

推理后端不靠编译开关，而是按 `tools/runtimes` 下**是否放着打包好的 DLL** 自动启用：
缺 `crispasr/` 就没有 whisper / qwen3-asr / qwen3-tts，缺 `onnx/` 就没有 indextts 与
whisper-onnx，缺 `ct2/` 就没有 whisper-ct2。当前可用后端见 `iris doctor`。

轻量构建（只编译主链路骨架，不含任何推理运行时）：

```bash
cmake -S . -B build -DIRIS_RUNTIMES_DIR=/nonexistent
```

---

## 配音链路（dub）

一条命令完成 **转写 → 情感分析 → 逐句 TTS → 按时码铺轨**，输出单声道配音 WAV。
推理全部在进程内（CrispASR 的 `qwen3-tts` 后端），只把 ffmpeg 留给媒体转码。

需要的两个 GGUF（缺一不可，放 `models/qwen3-tts/` 即可，`/models/` 已 gitignore）：

| 文件 | 来源 | 大小 |
|---|---|---|
| `qwen3-tts-12hz-0.6b-base-q8_0.gguf` | `cstr/qwen3-tts-0.6b-base-GGUF` | 940 MB |
| `qwen3-tts-tokenizer-12hz.gguf` | `cstr/qwen3-tts-tokenizer-12hz-GGUF` | 342 MB |

```bash
# 1) 媒体 → 配音（asr → [emotion] → tts）
build/bin/iris dub demo.mp4 \
  --engine qwen3-tts \
  --model models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf \
  --codec models/qwen3-tts/qwen3-tts-tokenizer-12hz.gguf \
  --asr-engine qwen3-asr --asr-model models/qwen3/qwen3-asr-0.6b-q4_k.gguf \
  --language en \
  --ref ref.wav --ref-text "ref.wav 的逐字稿" \
  --emotion-model models/emotion/wav2vec2-emotion-fp32.onnx \
  --out dub.wav --ir-out dub.iris.json

# 2) 已有 IR → 只重新配音（跳过转写）
build/bin/iris dub dub.iris.json \
  --model models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf \
  --codec models/qwen3-tts/qwen3-tts-tokenizer-12hz.gguf \
  --out dub2.wav

# 3) 只合成一句话
build/bin/iris tts \
  --model models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf \
  --codec models/qwen3-tts/qwen3-tts-tokenizer-12hz.gguf \
  --text "Hello there" --out hello.wav
```

链路行为：

- **音色克隆**：`--ref` + `--ref-text`（参考音频逐字稿）缺一不可；参考音频不是 24 kHz 时，
  先用 ffmpeg 转一份 24 kHz 单声道副本再交给 speaker encoder。不给 `--ref` 用后端默认音色。
- **时长对齐**（`--no-fit` 可关）：合成结果先**裁掉首尾静音**，再压进原句时隙
  （**自研 WSOLA** 变速不变调，无 ffmpeg 子进程，最多 `--max-speed` 倍，默认 1.2）；
  压到上限仍放不下的那一点，让**下一句顺延**而不是截断语音。因此：
  - 每句都靠近原句时隙长度，不再有「被拉长」的感觉；
  - 任意两句之间至少留 `--gap-ms` 的停顿（默认 200 ms），不会首尾相接；
  - 顺延量会打在日志里（`顺延 1083ms（累计 1083ms）`），可据此判断是模型语速偏慢还是文本太长。
  - 变速上限是「自然度 ↔ 同步」的旋钮：默认 1.2 偏自然；要更贴时间轴就调高（1.4 已明显偏赶）。
- **混音**：句子交叠时相加，最后整体归一峰值到 -0.1 dBFS 以内（保住句间相对音量、不硬削波）；
  轨道里有 `translatedText` 时优先用译文配音（翻译链路接上后 dub 不用改）。
- **产物**：`--out` 的 WAV（采样率 = 引擎原生采样率，qwen3-tts 为 24 kHz），
  IR 的 `state.isDubbed` / `state.dubbedAudioPath` 记录本次结果。
- **性能**（CPU，0.6B q8_0）：RTF ≈ 2.5–3，即 1 分钟成片约需 3 分钟合成。

TTS 引擎：

| `--engine` | 权重 | 状态 |
|---|---|---|
| `qwen3-tts` | talker GGUF + 12Hz tokenizer GGUF | ✅ 端到端可用（CrispASR 进程内） |
| `indextts` | IndexTTS 2.5 ONNX 目录（9 个图） | 🚧 目录校验 + 9 图加载完成，编排未接通 |

> ⚠️ CrispASR 对合成音频嵌入水印，但 C ABI 不会自动插入朗读式「AI 生成」声明；
> 对外发布时需自行提供可见/可听的 AI 标识（规范文案见 `crispasr_session_disclaimer_text()`），
> 克隆他人音色还需取得说话人同意。
>
> ℹ️ `emotion` 请用 **fp32** 权重：`wav2vec2-emotion-int8.onnx` 含 `ConvInteger`，
> 随包的 onnxruntime CPU EP 不支持该算子。

### 自研 Qwen3-TTS 运行时（进行中）

目标：**自己掌握 Qwen3-TTS 的输入输出**（文本 → 码流 → 波形），不再受推理后端的
固定语速/固定接口限制，为「按目标时长生成」留出控制点。现状与已落地部分：

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1 | 自研 GGUF 解析（元数据 + 张量表） | ✅ `iris tts --inspect --model <gguf>` 可读真实权重 |
| P1b | 自研变速（WSOLA，替代 ffmpeg atempo） | ✅ dub 已使用 |
| P2 | Talker 前向（28 层 GQA + q/k-norm + mRoPE + KV cache） | ✅ 前向与增量解码一致（<1e-3） |
| P2.3 | 提示词布局 + 与参考实现的 logits parity + 采样循环 | 🚧 进行中 |
| P3 | Code predictor（5 层深度变换器 × 16 码本/帧） | 🚧 待实现 |
| P4 | 解码器（RVQ dequant + 8 层 + Snake + 转置卷积 → 24 kHz） | 🚧 待实现 |
| P5 | Speaker encoder（音色克隆）+ BPE 文本前端 | 🚧 待实现 |

> parity 基准已就绪：参考后端支持 `QWEN3_TTS_DUMP_DIR=<dir>` 导出
> `talker_prefill_logits.bin`(3072)、`icl_input.bin`(139×1024)、
> `generated_codes_s00.bin`(30 帧×16 码本)、`cp_f*/step*` 等中间张量，
> 可直接用来逐层对拍自研推理。

`iris tts --inspect` 读出的模型结构（可作为运行时实现的对照表）：

```
talker      28 层 · d_model 1024 · 16 heads / 8 kv · head_dim 128 · ff 3072
            codec vocab 3072 · text vocab 151936 → 2048 · 16 码本/帧
            max_pos 32768 · position_id_per_seconds 13 · rope_theta 1e6 · mRoPE
code_pred   5 层 · d_model 1024 · 16/8 heads · vocab 2048 · max_length 20
codec.dec   8 层 · d_model 512 · 16 heads · latent 1024 · decoder 1536
            16 量化器 × 2048（语义 1 × 4096） · 12.5 Hz → decode_upsample 1920 → 24 kHz
```

> 在 P2–P4 落地前，音频仍由 CrispASR 的 `qwen3-tts` 后端生成；WSOLA 与时长对齐已在
> 链路中生效，与后端解耦，换成自研推理后无需改动 dub。

一键复跑（需要上面的两个 GGUF 与 ffmpeg）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/verify-dub.ps1
# 输出：dub.wav 时长/采样率/音量校验 + whisper 回听文本 + [OK] dub 链路通畅
```

---

## 翻译链路（非 LLM 的 NMT）

`translate` 走 **MarianMT / OPUS-MT、M2M100、WMT21、MADLAD** 这类经典编码器-解码器
NMT 模型（不是 LLM），同样在进程内跑（CrispASR 的 `crispasr_session_translate_text`）。
产出 IR 的 `translation` 轨道（每句保留原文 `text`，译文写 `translatedText`），
`dub` 会自动优先用译文配音。

| 模型 | GGUF | 语言 | 大小 |
|---|---|---|---|
| M2M100-418M | `cstr/m2m100-418m-GGUF` | 100 语言任意方向（含 en↔zh） | 285 MB(q4_k) / 526 MB(q8_0) |
| OPUS-MT / Marian | `cstr/opus-mt-<src>-<tgt>-GGUF` | 单方向（官方托管 24 个方向）；en↔zh 需自行转换，见下 | 84–160 MB |

```bash
# IR → 译文 IR（en → zh）
build/bin/iris translate out.iris.json \
  -m models/translation/m2m100-418m-q8_0.gguf --from en --to zh \
  --out out.zh.iris.json

# 媒体一步到位（内部先跑 ASR）
build/bin/iris translate demo.mp4 -m models/translation/m2m100-418m-q8_0.gguf \
  --from en --to zh --asr-engine whisper --asr-model models/ggml/ggml-base.bin

# 接配音（dub 会用 translation 轨道的译文）
build/bin/iris dub out.zh.iris.json --model models/qwen3-tts/... --codec ... --out dub.zh.wav
```

要点：

- `--engine auto`（默认）按 GGUF 自动识别后端；想强制 OPUS-MT 就 `--engine opus-mt`（映射到 `marian`）。
- MT 模型默认 **beam=5**（checkpoint 的 `num_beams`），贪心会退化成复读；`--beam 1` 可关。
- 中文目标会自动做标点/空格归一（半角→全角、去掉汉字旁空格）。
- **OPUS-MT en↔zh 的转换**（一次性，需要 Python + torch）：

  ```bash
  # 1) 取 HF 原始权重（config.json / source.spm / target.spm / vocab.json / pytorch_model.bin）
  # 2) 用 CrispASR 的转换脚本
  python models/convert-marian-to-gguf.py \
      --input  <opus-mt-en-zh 目录> \
      --output models/translation/opus-mt-en-zh-f16.gguf
  # 3) iris translate ... --engine opus-mt -m models/translation/opus-mt-en-zh-f16.gguf
  ```

  本仓库已实测：`Helsinki-NLP/opus-mt-en-zh` → 160 MB F16 GGUF，
  两句 1.0 s/句（CPU），译文「所以，我的美国同胞们，不要问你们的国家能为你做什么，问什么」。

一键复跑：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/verify-translate.ps1 -Ir out.iris.json
# 输出：译文 + IR 校验 + 汉字数量 + [OK] 翻译链路通畅
```

---

## 命令

| 命令 | 用途 | 状态 |
|---|---|---|
| `asr` | 媒体 → IR（ffmpeg 转换 → 预处理 → 进程内 ASR 推理 → 分句；`--separate` 分人声、`--punc` 补标点、`--diarize` 分说话人、`--split sat` 语义分句） | ✅ 已迁移 |
| `build` | IR → ASS / SRT / TXT | ✅ 已迁移 |
| `validate` | IR 结构校验 | ✅ 已迁移 |
| `doctor` | 环境诊断（ffmpeg / ASR / TTS 后端 / 配置） | ✅ 已迁移 |
| `serve` | REST API：`POST /jobs` 提交子命令、`GET /jobs/{id}` 查状态与产物（异步子进程） | ✅ 已迁移 |
| `models` | 提供者注册表 | ✅ 已迁移 |
| `tts` | 文本 → 语音（qwen3-tts / indextts，音色克隆） | ✅ 已迁移 |
| `emotion` | 语音情感分析（6 类 wav2vec2 ONNX） | ✅ 已迁移 |
| `dub` | 配音链路：媒体/IR → (asr) → (emotion) → 逐句 TTS → 配音音轨 | ✅ 已迁移 |
| `vsf` | 视频硬字幕帧定位（自实现，无 OpenCV） | ✅ 已迁移 |
| `init` | 交互式设置 | 🚧 待迁移 |
| `serve` | REST API：`POST /jobs` 提交子命令、`GET /jobs/{id}` 查状态与产物（异步子进程） | ✅ 已迁移 |
| `models` | 模型注册表：`models list` 看安装状态，`models install [keys] [--all] [--category x]` 一键下载（支持 `--mirror`/`IRIS_HF_MIRROR`） | ✅ 已迁移 |
| `quality` | 字幕质量报告（CPS/重叠/空白/重复/说话人，输出自包含 HTML；有 error 时 exit 2） | ✅ 已迁移 |
| `ocr` | 视频硬字幕 → IR（vsf 找字幕段 + PP-OCRv5 检测/识别） | ✅ 已迁移 |
| `convert` / `combine` | 字幕文件（SRT/ASS/VTT）→ IR；`combine --bilingual` 做双语对齐 | ✅ 已迁移 |
| `from-script` | 纯文本剧本 → 带时间码的 IR（转写做锚点 + NW 全局对齐 + 未匹配行按字符数插值；支持行首说话人标签） | ✅ 已迁移 |
| `correct` | IR 文本/时间码校正（空白、重复标点、空句、反序与重叠） | ✅ 已迁移 |
| `translate` | 机器翻译（dub 现已能用译文轨道配音） | 🚧 待迁移 |
| `quality` / `pipeline-graph` | QA 与 IR 工具 | 🚧 待迁移 |
| `serve` | REST API | 🚧 待迁移 |

---

## 架构

与 Centurion 相同的 operator-pipeline（DAG）架构：每个阶段是独立、可替换的模块，
按依赖图执行。当前 C++ 版实现了主链路的骨架：

```
input ─► FfmpegConvert ─► AudioPreprocess ─► Transcribe(crispasr) ─► SentenceSplit
   ─► IR (*.iris.json) ─► build ─► ASS / SRT / TXT
                     └─► AnalyzeEmotion(可选) ─► DubTts ─► dub.wav
```

- `FfmpegConvert` / `AudioPreprocess`：仍以子进程调用 **ffmpeg** 完成媒体解码与音频归一化
  （保留的外部工具，与 VSF 等媒体工具同属此列）。
- `Transcribe`：**进程内推理**。whisper(ggml) / qwen3-asr(ggml) 走 CrispASR
  （`crispasr_session_*` C API，模型后端自动识别）；whisper(transformer) 走 CTranslate2
  （`ctranslate2::models::Whisper`，30 秒窗口 + log-mel 特征 + 时间戳 token 解析）。
- `AnalyzeEmotion` / `DubTts`：dub 链路的两段——先做 6 类情感分析留痕，再把每个句子
  送进 TTS 引擎并按 `start_ms` 铺到同一条时间轴上。

| 目录 | 角色（对应 C# 项目） |
|---|---|
| `include/iris/model` | 数据模型（IR 文档、时间码、字幕、配置）—— 对应 `Centurion.Models` |
| `include/iris/core` | DAG 执行器、Operator 抽象、Provider 抽象、Result —— 对应 `Centurion.Core/Abstractions` |
| `include/iris/ops` | 具体操作符（ffmpeg_convert / transcribe / asr_pipeline / analyze_emotion / dub_tts …） |
| `include/iris/asr` | ASR 后端（CrispASR / CTranslate2 进程内推理；ct2_mel 特征提取） |
| `include/iris/tts` | TTS 引擎（qwen3-tts → CrispASR；indextts → onnxruntime）+ WAV I/O + 时间轴混音 |
| `include/iris/emotion` | 语音情感分类（wav2vec2 ONNX） |
| `include/iris/infra` | 基础设施（日志、子进程封装、i18n） |
| `include/iris/cli` | CLI11 前端 —— 对应 `Centurion.Cli` |
| `tests/` | Catch2 单元测试 —— 对应 `Centurion.Tests` |

推理运行时（crispasr / ctranslate2 / onnxruntime）以 `tools/runtimes` 下的**打包 DLL** 引用，
不内嵌其源码；构建期依赖通过 CMake 引入：nlohmann/json、spdlog、CLI11、Catch2（测试）。

详细的 C# → C++ 映射与迁移路线见 [docs/migration.md](docs/migration.md)。

---

## License

MIT — 见 [LICENSE](LICENSE)。

## 终端体验

自研终端呈现层（`src/ui/console.cpp`，参考 Centurion 的 Spectre.Console 语义色 + Ollama 的进度展示）：

- 语义色：错误红粗、警告金、成功绿、强调青、次要灰；`NO_COLOR` / `IRIS_PLAIN=1` 自动退化为纯文本；
- 符号表：`✓ ✗ → ▶ ● ○`，`IRIS_ASCII=1` 退化为 `OK / ERR / -> / >`；
- 结构元素：banner、小节标题、按键值对齐的表格（按显示宽度对齐，中文占 2 列）；
- 进度：下载时显示字节/速度/ETA 进度条；流水线逐阶段打印 `▶ 阶段` 与 `✓ 阶段 1.2s`；
- 编码：Windows 控制台一律经 `WriteConsoleW` 输出（UTF-16），**不依赖也不修改控制台代码页**，因此在 GBK（936）终端里也不会乱码；重定向/管道时输出 UTF-8 字节，可被脚本按 UTF-8 解析；`IRIS_PLAIN_OUTPUT=1` 可退回原始字节输出；
- 第三方推理库写到 stderr 的模型加载日志默认静音，`IRIS_VERBOSE=1` 可恢复。