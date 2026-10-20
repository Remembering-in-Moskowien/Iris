# iris

**Speech in, subtitles out.** iris 是 Centurion（.NET 10）的 **C++ 迁移项目**：
把音视频转成高质量 ASS 字幕，并逐步覆盖说话人分离、机器翻译、人声分离与机器配音
（Qwen3-TTS / QORA-TTS / IndexTTS 等）。默认完全本地运行，GPU 感知。

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

```bash
build/bin/iris asr demo.mp4 --engine qwen3-asr --language zh --model /path/to/qwen3-asr.gguf
build/bin/iris asr demo.mp4 --engine whisper-ct2 --language en --model /path/to/whisper-ct2-model/
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
| `IRIS_ASR_CRISPASR` | ON | 内嵌 CrispASR（ggml）：whisper / qwen3-asr 推理 |
| `IRIS_ASR_CT2` | ON | 内嵌 CTranslate2（transformer）：whisper-ct2 推理 |
| `IRIS_BUILD_TESTS` | ON | 构建 Catch2 测试 |

轻量构建（仅编译、不构建 ASR 推理库）：

```bash
cmake -S . -B build -DIRIS_ASR_CRISPASR=OFF -DIRIS_ASR_CT2=OFF
```

---

## 命令

| 命令 | 用途 | 状态 |
|---|---|---|
| `asr` | 媒体 → IR（ffmpeg 转换 → 预处理 → 进程内 ASR 推理 → 分句） | ✅ 已迁移 |
| `build` | IR → ASS / SRT / TXT | ✅ 已迁移 |
| `validate` | IR 结构校验 | ✅ 已迁移 |
| `doctor` | 环境诊断（ffmpeg / ASR 后端 / 配置） | ✅ 已迁移 |
| `models` | 提供者注册表 | ✅ 已迁移 |
| `init` | 交互式设置 | 🚧 待迁移 |
| `convert` / `combine` | 字幕文件 → IR | 🚧 待迁移 |
| `ocr` / `from-script` | OCR / 剧本导入 | 🚧 待迁移 |
| `correct` | 时间轴与文本校准 | 🚧 待迁移 |
| `translate` / `dub` | 翻译与配音 | 🚧 待迁移 |
| `quality` / `pipeline-graph` | QA 与 IR 工具 | 🚧 待迁移 |
| `serve` | REST API | 🚧 待迁移 |

---

## 架构

与 Centurion 相同的 operator-pipeline（DAG）架构：每个阶段是独立、可替换的模块，
按依赖图执行。当前 C++ 版实现了主链路的骨架：

```
input ─► FfmpegConvert ─► AudioPreprocess ─► Transcribe(crispasr)
   ─► SentenceSplit ─► IR (*.iris.json) ─► build ─► ASS / SRT / TXT
```

- `FfmpegConvert` / `AudioPreprocess`：仍以子进程调用 **ffmpeg** 完成媒体解码与音频归一化
  （保留的外部工具，与 VSF 等媒体工具同属此列）。
- `Transcribe`：**进程内推理**。whisper(ggml) / qwen3-asr(ggml) 走 CrispASR
  （`crispasr_session_*` C API，模型后端自动识别，支持 WAV/MP3/FLAC/OGG/Opus 等
  进程内解码）；whisper(transformer) 走 CTranslate2（`ctranslate2::models::Whisper`，
  30 秒窗口 + log-mel 特征 + 时间戳 token 解析，移植自 whisper.cpp / faster-whisper）。

| 目录 | 角色（对应 C# 项目） |
|---|---|
| `include/iris/model` | 数据模型（IR 文档、时间码、字幕、配置）—— 对应 `Centurion.Models` |
| `include/iris/core` | DAG 执行器、Operator 抽象、Provider 抽象、Result —— 对应 `Centurion.Core/Abstractions` |
| `include/iris/ops` | 具体操作符（ffmpeg_convert / transcribe / ass_build …） |
| `include/iris/providers` | 提供者（crispasr：CrispASR / CTranslate2 内嵌推理；ct2_mel 特征提取） |
| `include/iris/infra` | 基础设施（日志、子进程封装） |
| `include/iris/cli` | CLI11 前端 —— 对应 `Centurion.Cli` |
| `tests/` | Catch2 单元测试 —— 对应 `Centurion.Tests` |

依赖通过 CMake FetchContent 获取：nlohmann/json、spdlog、CLI11、CrispASR（含 ggml 子模块）、
CTranslate2、Catch2（测试）。

详细的 C# → C++ 映射与迁移路线见 [docs/migration.md](docs/migration.md)。

---

## License

MIT — 见 [LICENSE](LICENSE)。
