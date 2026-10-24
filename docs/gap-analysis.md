# iris ↔ Centurion 功能差距分析

对照对象：`Remembering-in-Moskowien/Centurion`（C#/.NET 10，本地克隆于 `.cache/stage/centurion`，
commit `7a3acd1`，2026-10-07）。方法：枚举对方的 CLI 命令注册、`Workflow/Pipeline/Operators`、
`Workflow/Strategy`、`Providers`、`Models`，与 iris 当前 `src/` 实现逐项比对。

## 1. 一句话结论

C# 侧有 **16 条顶层命令 + 27 个管线算子**；iris 目前 **11 条命令 + 7 个算子**。
主链（转码 → 预处理 → 转写 → 分句 → 渲染）已对等，翻译（非 LLM）与配音在 iris 侧
已能端到端跑通；**说话人分割、人声分离、校正、OCR、字幕导入、质量报告、模型管理、REST 服务**均已补齐，仅剩 `update` / `migrate` / LLM 类能力
这些整条链路仍然缺失，其中**说话人分割（Diarization）确实不在 iris 链路上**——
但运行时与模型都已就绪，可以立刻接。

| 统计项 | Centurion (C#) | iris (C++) |
|---|---|---|
| 顶层命令 | 16（init/asr/ocr/from-script/correct/translate/dub/convert/combine/serve/build/quality/pipeline-graph/validate/doctor/migrate）+ `models`/`providers` 两个分支 | 11（asr/build/validate/doctor/models/tts/dub/translate/emotion/vsf/init） |
| 管线算子 | 27 | 7 |
| 策略族 | 分句 7 / 转写 4 / 翻译 5 / 分离 3 / 对齐 1 | 分句 1 / 转写 3 / 翻译 1 / 分离 0 / 对齐 0 |
| 提供者 | ASR（本地+云）/ OCR / LLM / TTS / Diarization / VocalSeparation | 仅注册表展示，无实际 provider 链 |

## 2. 差距矩阵

图例：✅ 对等 ｜ 🟡 部分/降级 ｜ ❌ 缺失

| 能力域 | Centurion 实现（证据） | iris 现状 | 判定 |
|---|---|---|---|
| 媒体转码 | `FFmpegConvertOperator`、`FFmpegSplitter` | `ops/ffmpeg_convert.cpp` | ✅ |
| 音频预处理 | `AudioPreprocessOperator` | `ops/audio_preprocess.cpp` | ✅ |
| 转写（本地） | `CrispAsrWhisperStrategy` / `CrispAsrQwenStrategy` / `WhisperCppStrategy` | `asr/asr.cpp`（crispasr whisper/qwen3 + CT2） | ✅ |
| 转写（云） | `Asr/CloudAsrProvider.cs`、`ProviderChain` | 无 | ❌ |
| 分句 | `PassiveRuleSplit` / `AggressiveRuleSplit` / `LLMSplit` / `SaTSplit` + `XlmRBpeTokenizer` | ✅ rule + **sat**（`asr --split sat`：XLM-R SentencePiece 分词 + SaT ONNX，512/256 滑窗，失败退回 rule）；LLM 策略未做 | 🟡 |
| **说话人分割** | `DiarizationOperator`、`SpeakerProfilingOperator`、`PolyVoiceDiarizationStrategy`、`WeSpeakerDiarizationStrategy`、`DiarizationAudioPreprocessor` | ✅ `ops/diarize.cpp`（`asr --diarize`）：细网格 + 静音过滤 + 库推导 turns，写 `Sentence/Word.speaker` | ✅（P0 已接） |
| **人声分离** | `VocalSeparationOperator`、`HtDemucsOnnxEngine`、`DemucsVocalSeparationProvider` | ✅ `ops/vocal_separate.cpp`（`asr --separate`，HTDemucs，写 `state.vocalsPath`，预处理自动优先吃人声） | ✅（P0 已接，性能见下） |
| 翻译（非 LLM） | `OpusMtTranslationStrategy` + 自写 `OpusMtEngine`/`OpusMtTokenizer` | `mt/translator.cpp`（CrispASR marian/m2m100） | ✅ |
| 翻译（LLM） | `LLMTranslationStrategy`、`Llm/OpenAiCompatibleLlmProvider` | 无 | ❌ |
| 配音合成 | `TtsSynthesisOperator` + IndexTTS/Qora/LlamaTTS provider | `ops/dub_tts.cpp` + qwen3-tts | ✅ |
| 时间对齐 | `AlignmentOperator`、`TimeAlignmentOperator`、`AlignmentTempo`、`CrispAsrAlignmentStrategy` | WSOLA 压时隙 + 顺延（近似） | 🟡 |
| 混音/封装 | `AudioMixOperator`（dub → `*.dub.wav`，不封装回视频） | 内联混音 → WAV | ✅ |
| 校正 | `SpellCheckOperator`、`SubtitleTextCorrectorOperator`、`OverlapResolutionOperator`、`SubtitleTrackCheckerOperator`、`CorrectionReportOperator`、`TextPreprocessingOperator` | ✅ `correct` 命令 + [text/cleanup.cpp](src/text/cleanup.cpp)：空白/重复标点/空句/反序时间码/重叠消解（缺拼写检查与 LLM 校正） | 🟡 |
| OCR | `OcrExtractOperator`、`Ocr/OcrClientProvider` | ✅ `ocr` 命令：vsf 自动 ROI + 段内采样 + PP-OCRv5（DBNet 检测 + CRNN-CTC 识别，自写编排） | ✅ |
| 剧本导入 | `ScriptLoaderOperator`、`ScriptTimelineMapperOperator`、`BilingualSubtitleParserOperator` | 无 | ❌ |
| 字幕导入 | `ConvertParseOperator`、`CombineParse/Merge/Dedupe` | ✅ `convert` / `combine`（SRT/ASS/SSA/VTT 解析、去重、双语按时间重叠对齐） | ✅ |
| 质量报告 | `QualityReportOperator` + 整套 `Quality*` 模型 | ✅ `quality` 命令：CPS（按语言阈值）、覆盖率、重叠、长空白、重复、空行、说话人分布 + 自包含 HTML（`Quality*` 里的模型类指标未做，纯统计） | ✅ |
| 管线图 | `PipelineGraphCommand` / `PipelineGraphRenderer` | 无 | ❌ |
| REST 服务 | `ServeCommand`（ASP.NET Core in-process） | ✅ `serve`：自写 HTTP/1.1 + 异步任务队列（每任务一个 `iris` 子进程），端点 `/health` `/commands` `/models` `/jobs` | ✅ |
| 模型管理 | `models install/verify/remove`、`ModelRegistry` | 仅 `models list` | ❌ |
| 提供者诊断 | `providers list/test`、`ApiKeyStore`、`ProviderPolicies` | 仅 `models list` 静态表 | ❌ |
| 自更新 | `UpdateCommand`（存在但未在 CLI 注册） | 无 | ❌ |
| IR 迁移 | `MigrateCommand`（读 `*.centurion.json`） | 无 | ❌ |

## 3. 说话人分割专项（用户点名）

**C# 侧的做法**（`Operators/DiarizationOperator.cs`）：

- 输入音频优先级：`State.VocalsPath ?? State.PreprocessedAudioPath ?? State.ConvertedAudioPath`；
- 由 `DiarizationStrategyFactory` 按配置创建策略（PolyVoice / WeSpeaker）；
- 结果**按每个 Word 的时间中点**贴 `Speaker`，即词级说话人标注；
- 由 `PipelineOperatorFactory.CreateDiarizationOperator` 决定「**启用才装配该阶段**」，
  未启用时管线里根本没有这个算子（所以 C# 也并非默认开启）。

**iris 现状（已接入）**：`asr --diarize` 在分句之后追加 `Diarize` 算子
（`ops/diarize.cpp` ↔ `asr/diarize.cpp`）：

- 网格：把音频切成 1 s 小片（纯静音片不送聚类），交给 `crispasr_diarize_segments_turns_abi`；
- 取库推导的 **turns**（method 4 = FoxNose：WeSpeaker 嵌入 + 谱聚类），
  过滤「有声率 < 40%」的片段（交界静音否则会被聚成"另一个人"），
  再归并 < 500 ms 的碎片、按首次出现重编号 `SPEAKER_0..N`；
- 每句按**与 turns 的最大重叠**贴 `Sentence.speaker`，词级按词中点贴 `Word.speaker`；
- 其它轨道（translation）按时间中点共享同一套标签；
- 模型默认从 `~/.cache/crispasr/wespeaker*.gguf` 找，也可 `--speaker-model` 指定；
  `--speakers N` 可固定人数，`--diarize-method 5` 可换 Sortformer。

实测：单人 11 s 素材 → 1 人（两句都是 `SPEAKER_0`）；
双音色拼接素材（A 两句 + 0.8 s 静音 + B 两句）→ 2 人，标签严格按说话人分段。

## 4. IR 契约差距

C# `Centurion.Models` 里 iris 目前缺的字段：

- `Word.Speaker` / `SpeakerId`（iris 有 `speaker`，无 `speakerId`）；
- `AlignmentTempo`（对齐后每句的变速系数，iris 只在日志里输出）；
- 整个 `Quality*` 模型：`Meta` / `Counts` / `Coverage` / `Alignment` / `Timing` /
  `Confidence` / `Translation` / `Tts` / `Issues`（质量报告链路的载体）。

## 5. 建议的补齐顺序

| 优先级 | 内容 | 为什么现在做 | 状态 |
|---|---|---|---|
| **P0** | **说话人分割** | 运行时 API + 模型 + IR 字段全就绪；直接决定多音色配音与字幕标注 | ✅ 已接（`asr --diarize`） |
| **P0** | **人声分离**（HTDemucs） | `State.vocals_path` 字段已存在；配音/转写质量的上游开关 | ✅ 已接（`asr --separate`），但 CPU 上 ~19× 实时，慎用 |
| P0 | 推理线程数修复 | 默认 `hardware_concurrency` 在混合架构 CPU 上比 8 线程慢 70–100 倍 | ✅ 已修（whisper-tiny 82.7 s → 1.08 s） |
| P1 | 校正链（拼写、重叠消解、轨道自检） | 长视频字幕质量的刚需；纯文本逻辑，无模型依赖 | 🟡 文本/时间码校正已做，拼写检查待补 |
| P1 | 字幕导入（convert/combine）+ 剧本导入（from-script） | 「已有字幕→IR」入口 | 🟡 convert/combine 已做，from-script 待做 |
| P1 | OCR（VideoSubFinder 取帧 + OCR provider） | 硬字幕视频的入口 | ✅ `ocr` 命令（PP-OCRv5 ONNX） |
| P1 | 标点恢复 | 中文 ASR（Paraformer/SenseVoice）默认不带标点 | ✅ `asr --punc`（FireRedPunc） |
| P1 | dub 接入说话人（多音色） | 分割标签已有，差「按 speaker 选音色」 | ✅ `dub --voice SPEAKER=ref.wav` / `--voices voices.json` |
| P2 | 质量报告（Quality* + HTML） | 依赖校正/对齐数据先齐 | ✅ `quality` 命令 |
| P2 | LLM 翻译/分句、云 provider、`migrate`、`update` | 依赖模型与密钥体系 / 发布渠道 | ⏳ 待做（REST 已完成） |

### 人声分离的实测代价与并行化

CrispASR 的分离后端本身是**单线程**的（`htdemucs.cpp` 里 `p.n_threads = 0`，注释也写明
`n_threads` 是死字段），所以后台只看到一个核在跑（≈7% CPU / 14 核）。

| 方案 | 64 s 音频耗时 | 说明 |
|---|---|---|
| HTDemucs 单线程（原始） | ≈ 20 min（按 19× 实时估算） | 不可用于生产 |
| **HTDemucs + 16 s 分块 / 多会话并行** | **141 s（2.2× 实时）** | 现在的实现 |
| HTDemucs + 8 s 分块 | 199 s | 块太碎：模型加载次数与边界开销上升 |
| HTDemucs + 32 s 分块 | > 20 min（未跑完） | 并行度不足（3 块 / 3 线程） |
| mel-band-roformer（vocals-only, f16） | 283 s（11 s 音频） | 比 htdemucs 慢，且模型 457 MB |

结论：

- **默认仍用 HTDemucs q8_0**（54 MB、4 段、最快）；
- 自己实现「分块 + 每块独立会话 + 交叉淡入拼接」拿到 ~8× 加速，
  块长默认 **16 s / 重叠 1 s**，可用 `--separation-chunk` 调；
- `--separate` 仍建议只在需要时开（2.2× 实时，长片仍要等）。

## 6. 证据附录

- 命令注册：`.cache/stage/centurion/src/Centurion.Cli/Commands/CommandRegistrationExtensions.cs`
- 算子清单：`.cache/stage/centurion/src/Centurion.Core/Workflow/Pipeline/Operators/`（27 个文件）
- 策略族：`.cache/stage/centurion/src/Centurion.Core/Workflow/Strategy/`
- 分割实现：`.../Operators/DiarizationOperator.cs`、`.../Strategy/Diarization/`
- 装配逻辑：`.../Workflow/Factories/PipelineOperatorFactory.cs`（未启用则不装配）
- iris 侧：`src/ops/`（7 个算子）、`src/asr/asr.cpp`、`src/mt/translator.cpp`、`include/iris/model/ir_document.hpp`
