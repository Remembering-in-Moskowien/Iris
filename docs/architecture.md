# daisy 架构

## 总览

daisy 是 Centurion（C#）的 C++ 迁移，保留 operator-pipeline（DAG）架构：
管线由独立、可替换的操作符组成，操作符声明依赖边，执行器拓扑排序后按序运行。
每个阶段的结果沉淀到**版本化中间文件**（`*.daisy.json`，即 IR），
后续命令只消费 IR，形成可断点恢复的命令链。

```
媒体输入
   │
   ▼
┌─────────────────────────────────────────────────────────┐
│ Pipeline (DAG 执行器)                                    │
│  FfmpegConvert → AudioPreprocess → Transcribe →          │
│  SentenceSplit → (…)                                    │
└─────────────────────────────────────────────────────────┘
   │
   ▼
IR (*.daisy.json) ──► build ──► ASS / SRT / TXT
```

## 分层

```
┌────────────────────────────────────────────────┐
│ cli    命令分发（CLI11）                        │
├────────────────────────────────────────────────┤
│ ops    具体操作符（业务阶段）                   │
├────────────────────────────────────────────────┤
│ core   引擎：Pipeline / Operator / Provider    │
├────────────────────────────────────────────────┤
│ model  数据模型 + IR 契约（nlohmann::json）     │
├────────────────────────────────────────────────┤
│ infra  子进程、日志、下载等基础设施             │
└────────────────────────────────────────────────┘
```

## 核心抽象

### Operator（操作符）

```cpp
class Operator {
    virtual std::string name() const = 0;                 // 唯一标识，写入 provenance
    virtual std::vector<std::string> dependencies() const; // DAG 边
    virtual bool execute(PipelineContext& ctx) = 0;        // 阶段逻辑
};
```

### Pipeline（DAG 执行器）

- `add(op)` 注册操作符（允许重复名称？——不，名称唯一，重复注册视为同名合并）。
- `run(ctx)`：Kahn 拓扑排序 → 按序执行；依赖缺失或存在环则失败；任一操作符失败即中止。
- 并行分支（C# 版支持）在后续版本引入（`std::execution` / 线程池）。

### Provider（提供者）

本地优先、云兜底的推理抽象：ASR / OCR / LLM / TTS / Diarization / VocalSeparation
各定义接口；`ProviderRegistry` 管理注册项，`models` 命令展示。

### IR 契约（*.daisy.json v1.0）

```json
{
  "schemaVersion": "1.0",
  "generator": { "tool": "daisy", "version": "...", "command": "asr", "generatedAt": "..." },
  "provenance": [ { "operator": "transcribe", "model": "whisper.cpp", "parametersHash": "..." } ],
  "config":   { "inputFilePath": "...", "language": "en", "transcriberEngine": "whisper", "..." },
  "state":    { "convertedAudioPath": "...", "tracks": [ { "type": "transcribe", "sentences": [...] } ] }
}
```

字段名与 C# 版 `*.centurion.json` 保持一致，保证未来可互相 `migrate`。

## 关键设计决策

| 决策 | 理由 |
|---|---|
| 子进程调用 ffmpeg / whisper-cli | 与 C# 版一致，工具链成熟，避免绑定原生库的构建复杂度 |
| 依赖用 CMake FetchContent 锁版本 | 可复现构建；CI 与本地一致 |
| IR 作为唯一交换契约 | 命令链可断点恢复；provenance 可审计 |
| Result<T> 替代异常传播 | 命令式错误处理，便于 CLI 层统一报错与退出码 |
| 时间码统一毫秒存储 | ASS（百分秒）与 SRT（毫秒）双向无损转换 |
