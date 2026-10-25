# iris 交接文档（Handoff）

> 给下一个会话/接手者：这份文档记录当前状态、关键决策、坑位与建议的下一步。
> 最后更新：build 0.1.42（多平台自动发布 + `--json` 接入 serve 之后）。

## 1. 项目是什么

iris 是 **Centurion（C#/.NET 10）的 C++20 迁移**：媒体 → 字幕/配音的全链路工具。

```
【媒体】媒体 → [人声分离] → 预处理 → 转写(6 引擎) → [标点] → 分句(rule|sat) → [说话人分割] → IR
【字幕】SRT/ASS/VTT → convert / combine(--bilingual) → IR
【剧本】纯文本剧本 → from-script（转写做锚点 + NW 对齐）→ IR
【硬字幕】视频 → ocr（vsf 自动 ROI + PP-OCRv5）→ IR
                        ↓
        IR → correct → build(ASS/SRT/TXT) → translate → dub(单/多音色)
                        ↓
                  quality（HTML 报告）
```

## 2. 目录速查

| 路径 | 内容 |
|---|---|
| `include/iris/`, `src/` | 核心库（MVC：model/view/controller/ops/asr/tts/mt/text/ocr/sat/script/subtitle/server/ui） |
| `src/ops/*.cpp` | 管线算子（ffmpeg_convert, audio_preprocess, transcribe, sentence_split, diarize, vocal_separate, dub_tts, translate, correct…） |
| `src/cli/cli.cpp` | CLI 全部子命令（单文件，1800+ 行，后续可拆分） |
| `src/controller/*.cpp` | 每个命令的实现 + JSON 结果输出 |
| `src/ui/console.cpp` | 终端呈现层（颜色/表格/进度/agent/json 模式） |
| `src/infra/` | 进程、管道、线程、i18n、日志、静音、路径 |
| `tests/test_tts.cpp` | **全部 44 个用例都在这一个文件里**（含 tts/asr/ocr/sat/script/catalog/serve/ui/config），后续建议拆分 |
| `tools/runtimes/` | 预编译推理运行时（crispasr/ct2/onnx），gitignore，CMake 自动探测 |
| `docs/gap-analysis.md` | 与原 Centurion 的功能对照矩阵 |
| `docs/asr-onnx-sensevoice-paraformer.md` | FunASR ONNX 后端规格 |
| `models/` | 本地模型（gitignore） |

## 3. 当前完成度

- 顶层命令：**15/16**（只缺 `update`；`migrate` 无需求）
- 链路：媒体/字幕/剧本/硬字幕四类入口全部打通
- 测试：**90/90 通过**（`ctest --test-dir build -C Release`；本次新增 extension/manifest 3 个用例）
- 插件：P0/P1 已落地——ASR/TTS/render 静态注册表（`ExtensionRegistry`）、`plugin list`（含 `--json`）、`plugin validate`（结构 + 本机运行时）、`run <pipeline.json>`（facet DAG 校验 + 执行）；T2 DLL / T3 进程插件仍未实现（见 `docs/plugins.md`）
- 发布：多平台 GitHub Actions（Windows/Linux/macOS），按 `run_number` 出 `v{n}`

## 4. 关键决策（改动前先读）

1. **线程上限 8**：混合架构 CPU 上 `hardware_concurrency`（14）会让 ggml 慢 70–100 倍。
   所有推理路径统一走 `infra::resolve_threads()`；`IRIS_THREADS` 可覆盖。
2. **分离是单线程库**：CrispASR 的 htdemucs `n_threads=0`（死字段）。iris 用「16 s 分块 + 每块独立会话 + 交叉淡入」拿到 ~8× 加速。
3. **TTS 会话复用**：qwen3 后端拆成 open/synthesize，dub 每音色一条会话（否则每句重载模型 + 重编码参考音频）。
4. **Windows 输出**：控制台走 `WriteConsoleW`（UTF-16，`src/ui/console.cpp`），**不改代码页**；ANSI SGR 被翻译成 `SetConsoleTextAttribute`。管道/重定向仍写 UTF-8 字节。
5. **CLI 参数**：`main.cpp` 用 `GetCommandLineW` 重建 UTF-8 argv（否则中文参数变 GBK）。
6. **数据目录**：`<安装盘>:/iris`（`IRIS_HOME` 可覆盖），配置 `config.json`，模型相对 `modelsDir`；旧 `%APPDATA%` 配置会自动迁移。
7. **`--agent` / `--json`**：前者给带前缀的人类行，后者 stdout 只放一个 JSON 文档（日志转 stderr，`std::cout` 被换成空缓冲）。
8. **IR 1.0 加法字段**：`state.styles` / `scriptInfo` / `timelineOffsetMs`、`sentence.style` / `raw` /
   `layer` / `comment`、`track.kind`；旧 IR 缺省即兼容，schemaVersion 仍是 "1.0"。
9. **成品保真**：`convert` 走 `subtitle::AssScript`（全字段样式 + 原始标签）；`build` 无模板时
   用 IR 样式原样回写。ASS→IR→ASS 在 255 部成品上语义比对 0 差异（未定义样式按文本语言回退）。
10. **路径编码**：Windows 上一律 `infra::utf8_path()` 开文件；不要用 `fs::path(std::string)`
    （按本地代码页解释，中文/全角冒号路径会抛异常）。

## 5. 已踩过的坑（别重犯）

| 坑 | 症状 | 正确做法 |
|---|---|---|
| `std::string(n, '─')` | 边框全 `�` | 按 UTF-8 字符串循环拼接 |
| `find_first_of(":：")` | 中文标签被截断 | 整串 `find`，全角冒号跳 3 字节 |
| `Ort::AllocatedStringPtr(...).get()` | 崩溃/垃圾输出名 | 把 AllocatedStringPtr 存进局部变量 |
| `resize` 子图按子图宽算行距 | 条纹噪声、CTC 全 blank | 先拷成紧凑缓冲再缩放 |
| `fs::path("D:") / "x"` | 得到 `D:x`（驱动器相对） | 显式拼分隔符 |
| PowerShell `here-string` 写中文源码 | 文件变 GBK 乱码/语法错 | 用 `write`/`edit` 工具改文件 |
| `run_process` 先等进程再读管道 | 输出 >64 KB 死锁 | 用线程并发排空（`run_process_split`） |
| `sentence_split` 按字节切 | 汉字被劈开 → IR 非法 UTF-8 | 按 UTF-8 字符推进 |
| 子进程命令行按字节转 wchar | 中文/全角路径传给 ffmpeg 变 `æ å­å¹`，报“文件不存在” | 参数整串 `MultiByteToWideChar(CP_UTF8)` 再拼命令行 |
| `correct` 消重叠删行 | 注释/歌词/未知样式被前一句覆盖时整行丢弃 | 按**样式名**分桶 + 歌词行豁免 + 完全覆盖时保留重叠 |

## 6. 环境与命令

```bash
# 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release          # 模型相关用例需要 cwd = 仓库根

# 常用
iris doctor
iris models list | iris models install --all
iris asr in.mp4 -l zh --punc --diarize --split sat
iris translate in.iris.json --from zh --to en -m models/translation/m2m100-418m-q8_0.gguf
iris dub in.iris.json --ref voice.wav --ref-text "..." --voice SPEAKER_2=other.wav
iris quality in.iris.json          # HTML 报告；有 error 时 exit 2
iris lint 作品目录 --fonts 字体目录 -o lint.html   # 成品只读体检 + 歌词待补清单
iris style extract 成品.ass -o theme.json         # 从成品反推样式模板
iris build in.iris.json --bilingual --template theme.json --shift +1:10:27.30
iris karaoke out.iris.json -o out.k.iris.json    # 打 \K（投影 / 词级 / 均分）
iris correct out.iris.json --rules docs/rules/mlp.rules --glossary docs/rules/glossary.tsv
iris diff out.iris.json --reference 参考.ass --cjk-only
iris align 视频.mkv --ir out.iris.json --model models/aligner/qwen3-forced-aligner-0.6b-q4_k.gguf
iris project scan 作品目录 --html ledger.html      # 只读台账
iris project run 作品目录 --out-dir iris-out       # 批量流水线（可增量）
iris plugin list                                   # operation/asr/tts/render 注册表
iris plugin validate docs/pipeline-asr.json        # 结构 + 本机 ASR 运行时校验
iris run docs/pipeline-asr.json models/jfk.wav -l en -o jfk.iris.json --render-output jfk.ass
iris --json plugin list                            # 结构化扩展清单
iris --json asr in.mp4 ...        # 单 JSON 文档
iris serve --port 8080            # REST（POST /jobs, GET /jobs/{id}）
```

## 7. 建议的下一步（按性价比排序）

1. **拆分 `cli.cpp` / `test_tts.cpp`**（纯重构，风险低，收益长期）
   - `cli.cpp` 1800+ 行，建议按命令拆成 `cli/asr.cpp` 等；
   - 测试单文件 44 用例，建议按模块拆。
2. **`serve` 增补**：`DELETE /jobs/{id}`（取消）、`GET /jobs/{id}/files/{name}`（下载产物）、并发 worker（现在串行）。
3. **`update`**：`iris update [--check]`，对比 GitHub Releases API 的 `v{run_number}`。
4. **模型下载体验**：`models install` 现在用外部 `curl`；可内置 HTTP（WinHTTP/libcurl）并加断点续传。
5. **dub 质量**：per-speaker 语速/音量归一、句间停顿自适应（现在固定 200 ms）、与视频 mux（ffmpeg 封装 mp4）。
6. **OCR 增强**：竖排/多行合并、`--roi` 自动多带（现在只取最密集一带）。
7. **LLM 能力**（P2 剩余）：`--split llm`、LLM 翻译，需要 provider 抽象 + 密钥配置（`config.json` 加 `llm` 段）。
8. **性能**：TTS codec 解码占 43%，可考虑缓存/量化；asr 的 Ct2 路径可尝试 int8。
9. **`align`（音频强制对齐）**：已实现（`align` 命令 + `qwen3-forced-aligner` 模型条目），
   逐句调用 `crispasr_align_words_abi` 写 `Sentence.words`，`karaoke` 自动吃；
   ⚠️ 本机网络不通、模型未下载，**运行时路径尚未端到端验证**（`--dry-run` 已验证解码与句子统计）。

## 8. 已知限制

- Linux/macOS 发布包**不含推理运行时**（运行时是 Windows DLL）；这些平台只能用核心功能（转换/字幕/质量/翻译需自备运行时）。
- OCR/分离/SaT 模型较大（~100 MB ~ 1.3 GB），不随包发布。
- 二进制未签名；macOS 需 `xattr -dr com.apple.quarantine`。
- Windows 发布包若拉不到 `runtime-bootstrap`，会打包「仅核心」版本（release notes 会标注）。

## 9. 发布流程（自动化）

- 触发：push `master` 或手动 `workflow_dispatch`
- 构建号：`github.run_number` → `IRIS_BUILD_NUMBER` → `0.1.{n}`
- 三平台矩阵构建 + `ctest` 通过后打包，最后统一 `gh release create v{n}`
- 资产：`iris-{n}-win-x64.zip` / `iris-{n}-linux-x64.tar.gz` / `iris-{n}-macos-arm64.tar.gz`
- Windows 的运行时 DLL 来自预发布 `runtime-bootstrap`（缺失则降级为仅核心并告警）
