# daisy

**Speech in, subtitles out.** daisy 是 Centurion（.NET 10）的 **C++ 迁移项目**：
把音视频转成高质量 ASS 字幕，并逐步覆盖说话人分离、机器翻译、人声分离与机器配音
（Qwen3-TTS / QORA-TTS / IndexTTS 等）。默认完全本地运行，GPU 感知。

> 原仓库 `Centurion` 已设为 private，保留为 C# 参考实现与历史基线；
> 新代码在 `daisy`（本仓库）中以 C++ 继续演进。

![lang](https://img.shields.io/badge/language-C%2B%2B20-2ea44f)
![build](https://img.shields.io/badge/build-CMake-blue)
![status](https://img.shields.io/badge/status-migrating-yellow)

---

## 快速开始

```bash
# 1. 配置并构建（需要 CMake ≥ 3.20 + C++20 编译器 + ffmpeg）
cmake -S . -B build -DDAISY_BUILD_TESTS=ON
cmake --build build --config Release

# 2. 转写音视频到中间文件（IR）
build/bin/daisy asr demo.mp4 --language en --model /path/to/ggml-base.bin

# 3. 渲染字幕
build/bin/daisy build demo.mp4.daisy.json --format ass

# 4. 环境诊断
build/bin/daisy doctor
```

Windows 一键构建：`powershell -File scripts/bootstrap.ps1`。

第三方工具与模型（whisper.cpp、CrispASR、Demucs、QORA-TTS 等）沿用
Centurion 的策略：首次使用时自动下载，走国内可达的镜像（hf-mirror /
gh-proxy.com，可用 `daisy.config.json` 的 `downloadProxy` 覆盖）。

---

## 命令

| 命令 | 用途 | 状态 |
|---|---|---|
| `asr` | 媒体 → IR（ffmpeg 转换 → 预处理 → whisper.cpp 转写 → 分句） | ✅ 已迁移 |
| `build` | IR → ASS / SRT / TXT | ✅ 已迁移 |
| `validate` | IR 结构校验 | ✅ 已迁移 |
| `doctor` | 环境诊断（ffmpeg / whisper-cli / 配置） | ✅ 已迁移 |
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
input ─► FfmpegConvert ─► AudioPreprocess ─► Transcribe(whisper.cpp)
   ─► SentenceSplit ─► IR (*.daisy.json) ─► build ─► ASS / SRT / TXT
```

| 目录 | 角色（对应 C# 项目） |
|---|---|
| `include/daisy/model` | 数据模型（IR 文档、时间码、字幕、配置）—— 对应 `Centurion.Models` |
| `include/daisy/core` | DAG 执行器、Operator 抽象、Provider 抽象、Result —— 对应 `Centurion.Core/Abstractions` |
| `include/daisy/ops` | 具体操作符（ffmpeg_convert / transcribe / ass_build …） |
| `include/daisy/providers` | 提供者（whisper.cpp 等） |
| `include/daisy/cli` | CLI11 前端 —— 对应 `Centurion.Cli` |
| `tests/` | Catch2 单元测试 —— 对应 `Centurion.Tests` |

依赖通过 CMake FetchContent 获取：nlohmann/json、spdlog、CLI11、Catch2（测试）。

详细的 C# → C++ 映射与迁移路线见 [docs/migration.md](docs/migration.md)。

---

## License

MIT — 见 [LICENSE](LICENSE)。
