# 插件与内置扩展

## 设计草案与当前代码的已知偏差

随任务提供的 v1.0 文档是讨论稿，其“项目事实”和耦合点需以仓库代码为准：

| 草案描述 | 当前代码事实 |
|---|---|
| CMake 最低版本为 3.20 | `CMakeLists.txt` 要求 CMake 3.24。 |
| CLI 命令由约 1900 行 if 链路由 | 当前入口通过 CLI11 子命令声明和分发。 |
| ASR 共有五种引擎 | 当前 `AsrBackend` 有六种，另含 `whisper-onnx`；README 已补充其模型目录格式。 |
| `ProviderRegistry` 是运行时后端分发器 | 当前它只管理提供者元数据；ASR/TTS 的执行由下述类型化静态扩展注册表完成。 |

后续架构决策应先核对代码和现行用户文档，不应直接照搬这些过时描述。

## 当前能力

Iris 已有编译期静态扩展注册和执行分发。ASR/TTS 后端与字幕渲染器注册规范 ID、兼容别名、运行时可用性与类型安全的执行入口；TTS 注册还提供会话工厂。CLI/控制器既有入口通过相同注册项解析名称、检查运行时并分发执行。

声明式管线使用 `apiVersion: 1` 的 JSON manifest。当前运行器注册了 `ffmpeg_convert`、`audio_preprocess`、`transcribe`、`sentence_split`、`build` 和只读 `lint` 检查；每个操作固定声明输入/输出 facet。依赖必须组成 DAG，并且每个非源 facet 必须由依赖祖先提供。未知字段、未知操作、未提供的 facet、重复依赖、缺失依赖和环都会在执行前拒绝。节点参数按操作白名单校验。`iris plugin list` 按 operation/asr/tts/render 四组列出静态注册项及可用性（加 `--json` 输出结构化清单）；`iris plugin validate <pipeline.json>` 校验 manifest 并检查本构建的 ASR 运行时；`iris run <pipeline.json> <media>` 执行并生成 IR。原有 `asr` 等命令仍保持独立入口。

示例 [`pipeline-asr.json`](./pipeline-asr.json)：

```json
{
  "apiVersion": 1,
  "nodes": [
    { "id": "convert", "operation": "ffmpeg_convert" },
    { "id": "preprocess", "operation": "audio_preprocess", "dependsOn": ["convert"] },
    {
      "id": "transcribe",
      "operation": "transcribe",
      "dependsOn": ["preprocess"],
      "parameters": { "engine": "whisper", "model": "models/ggml/ggml-base.bin" }
    },
    { "id": "split", "operation": "sentence_split", "dependsOn": ["transcribe"] }
  ]
}
```

```powershell
iris plugin list
iris plugin validate pipeline.json
iris run pipeline.json media.mp4 --language en --output media.iris.json --render-output media.ass
```

`plugin validate` 会拒绝未知 ASR 引擎和本构建不可用的后端；`run` 还会逐个 `transcribe` 节点检查 engine/model，因此同一 manifest 里的不同节点可以使用不同引擎或模型。`plugin list` 的可用性来自构建时运行时探测，所以同一 manifest 在不同机器上的验证结果可能不同；这是“本机构建能否执行”的检查，不是 manifest 的跨机器合法性判断。

`build` writes its subtitle artifact in the run's temporary work directory; use
`--render-output <path>` to copy it to a persistent location. The `--output` option is for the IR
document. A build node accepts `format` (`ass`, `srt`, or `txt`), `template`, `bilingual`,
`shiftMs`, and `scale` parameters; the latter four currently affect ASS rendering only. `lint`
currently accepts ASS/SSA artifacts only and reports a clear error for other render formats.

现有 ASR ID：`whisper`、`qwen3-asr`、`whisper-ct2`、`whisper-onnx`、`sensevoice-onnx`、`paraformer-onnx`。兼容别名：`sensevoice`、`paraformer`。

现有 TTS ID：`qwen3-tts`、`indextts`。兼容别名：`qwen3tts`、`qwen3`、`index-tts`。

后端是否可用仍由构建时探测到的 CrispASR、CTranslate2、ONNX Runtime 决定。缺少所需运行时的后端会报告不可用，不会绕过检查或静默回退；`plugin validate` 与 `run` 都会在装配阶段拒绝不可用后端。

`core::ExtensionRegistry<T>` 提供通用的类型化注册、规范 ID/别名查找和重复项校验。ASR、TTS 与字幕渲染器都在各自实现中构建注册表，`iris plugin list` 的清单直接来自这些注册项，当前仅供随 Iris 一起编译的内置扩展使用。现有 `core::ProviderRegistry` 仍是提供者元数据目录，不承担执行分发。

## 信任边界与未实现能力

当前 pipeline 操作仍为随程序编译的内置代码。声明式 JSON 不是脚本沙箱，也不授权任意函数或宿主操作。用户脚本进程、签名包管理、权限执行、插件仓库和 T2 C ABI 动态库仍未实现；在这些能力完成前，不允许不可信代码加载。

## 增加内置后端

1. 在对应的后端枚举/请求类型中定义稳定的内部值。
2. 在 ASR 或 TTS 静态注册表中添加规范 ID、当前兼容别名、可用性标志和执行函数；TTS 还需提供会话工厂。
3. 保持运行时缺失时的不可用语义，并为规范 ID、别名、未知 ID、可用性及执行路径添加测试。
4. 更新本文和相关用户文档，明确新能力仍是内置静态扩展，而不是可安装的第三方插件。
