# 迁移指南：Centurion (C#/.NET 10) → daisy (C++20)

本文档是从 Centurion 迁移到 daisy 的权威映射与进度记录。
原仓库 `Remembering-in-Moskowien/Centurion` 保持 private，作为参考实现。

## 1. 项目结构映射

| Centurion（C#） | daisy（C++） | 说明 |
|---|---|---|
| `src/Centurion.Models` | `include/daisy/model` + `src/model` | 数据模型、IR 契约、元数据 |
| `src/Centurion.Abstractions` | `include/daisy/core` | Operator / Provider / 管线抽象 |
| `src/Centurion.Core` | `include/daisy/core` + `src/core` + `include/daisy/ops` + `src/ops` | DAG 引擎、操作符、策略、提供者 |
| `src/Centurion.Cli` | `include/daisy/cli` + `src/cli` | CLI11 前端（替代 Spectre.Console） |
| `src/Centurion.Tests` | `tests/` | Catch2（替代 xUnit） |
| `*.centurion.json`（IR v1.0） | `*.daisy.json`（IR v1.0，字段契约一致） | 中间文件 |

## 2. 核心类型映射

| C# | C++ | 备注 |
|---|---|---|
| `CenturionDocument` | `daisy::model::IrDocument` | schemaVersion + generator + provenance + config + state |
| `GeneratorInfo` | `daisy::model::GeneratorInfo` | tool/version/command/generatedAt/inputFile/outputFile |
| `ProvenanceEntry` | `daisy::model::ProvenanceEntry` | operator/model/parametersHash/appliedAt |
| `WorkflowConfig` | `daisy::model::WorkflowConfig` | 常用字段已映射（inputFilePath、language、transcriberEngine 等） |
| `WorkflowState` | `daisy::model::WorkflowState` | 各阶段产物路径 + 轨道集合 |
| `Sentence` / `Word` | `daisy::model::Sentence` / `Word` | startMs/endMs/text/words |
| `IRTrack` | `daisy::model::Track` | type + language + sentences |
| `SubtitleTime` 时间码 | `daisy::model::TimeCode` | ASS（百分秒）/ SRT（毫秒）解析与格式化 |
| `Operator` | `daisy::core::Operator` | name() + dependencies() + execute(ctx) |
| `Pipeline`（DAG 执行器） | `daisy::core::Pipeline` | 注册 → 拓扑排序 → 执行 |
| `IAsrProvider` 等 | `daisy::core::IAsrProvider` | provider 抽象 |
| `AppConfig`（centurion.config.json） | `daisy::model::AppConfig`（daisy.config.json） | downloadProxy 等 |

## 3. 已迁移（当前提交）

- **IR 文档模型**：完整 JSON 序列化（字段名与 C# System.Text.Json 契约一致）、加载/保存/校验。
- **时间码**：ASS / SRT 解析与格式化，非法输入拒绝。
- **字幕渲染**：ASS v4.00+ 脚本（样式表 + 事件表）、SRT、TXT。
- **DAG 引擎**：Operator 注册、依赖声明、Kahn 拓扑排序、顺序执行、溯源记录。
- **主链路操作符**：`FfmpegConvert`（ffmpeg → 16kHz 单声道 WAV）、`AudioPreprocess`
  （speechnorm + highpass）、`Transcribe`（whisper.cpp `-oj` 输出解析）、`SentenceSplit`
  （rule 策略，标点边界 + 最大长度硬切，子句时间轴按比例分配）、`SubtitleBuild`。
- **提供者**：`WhisperCppAsr`（子进程调用 whisper-cli，解析 segments JSON）。
- **CLI**：`asr` / `build` / `validate` / `doctor` / `models list` / `init`(stub)。
- **测试**：时间码、ASS/SRT 渲染、IR 序列化往返与校验。
- **CI**：GitHub Actions 双平台（Ubuntu gcc / Windows MSVC）构建 + ctest。

## 4. 迁移路线图

按依赖顺序推进（优先级从高到低）：

1. **IR 完整字段**：补齐 WorkflowConfig / WorkflowState 其余字段（音频预处理开关、
   翻译/配音参数、质量报告参数），保证 `migrate` 命令可双向读取 C# 版 IR。
2. **OCR 链路**：VideoSubFinder 取帧 → RapidOCR/GLM-OCR（本地 ONNX / 云 API）→ 烧录字幕。
3. **翻译链路**：`Translate` 操作符 + LLM provider 抽象（11+ 提供者，glossary，目标文字对齐）。
4. **配音链路**：`Dub` 操作符 + TTS provider（Qwen3-TTS / QORA-TTS / IndexTTS），
   时间对齐与混音（rubberband / atempo）。
5. **说话人分离**：`Diarization` 操作符（foxnose / WeSpeaker 嵌入）。
6. **质量报告**：`QualityAssessor` / `QualityHtmlReport` → HTML 报告生成。
7. **REST API**：`serve` 命令（cpp-httplib 替代 ASP.NET Core in-process）。
8. **更新机制**：`update` 自更新（对应 Release 发布流）。

## 5. 工程约束

- C++20，头文件在 `include/daisy`，实现分散到 `src/`，命名空间 `daisy::`。
- 所有外部工具（ffmpeg、whisper-cli、demucs …）通过子进程调用，与 C# 版一致
  （生产级进程管理将逐步替换 `popen` 为真正的进程 API）。
- 模型与工具首次使用自动下载，镜像策略（hf-mirror / gh-proxy）保持与 C# 版一致。
- 依赖全部经 CMake FetchContent，锁版本（见 `cmake/dependencies.cmake`）。

## 6. 验证

- 单元测试：`ctest --test-dir build --output-on-failure`（时间码 / 渲染 / IR 往返）。
- CI：push/PR 触发双平台构建 + 测试。
- 端到端（本地）：`daisy asr <media> --model <ggml>.bin` → `daisy build out.daisy.json`。
