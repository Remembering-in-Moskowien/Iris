#pragma once

#include <string>
#include <vector>

#include "iris/asr/diarize.hpp"
#include "iris/asr/separate.hpp"
#include "iris/asr/align.hpp"
#include "iris/project/project.hpp"
#include "iris/quality/ass_lint.hpp"
#include "iris/view/ass_builder.hpp"
#include "iris/llm/scheduler.hpp"

namespace iris::controller {

/// 版本号（Model/IR generator.version 与 --version 共用）。
/// CI 通过 IRIS_BUILD_NUMBER 注入构建号（release tag = v{run_number}）；本地默认 0.1.0。
#ifndef IRIS_VERSION_STRING
#define IRIS_VERSION_STRING "0.1.0"
#endif
inline constexpr const char* kVersion = IRIS_VERSION_STRING;

// MVC Controller 层：每个子命令一个处理器，负责编排 Model 与 Service（ops），
// 并把结果交给 View（渲染/控制台）。返回进程退出码（0 成功，非 0 失败）。

/// asr：媒体 → IR（ffmpeg_convert → audio_preprocess → transcribe → sentence_split）。
/// diarize 非空时，在分句之后追加说话人分割阶段。
int run_asr(const std::string& input, const std::string& language, const std::string& engine,
            const std::string& model, const std::string& output,
            const asr::DiarizeOptions* diarize = nullptr, int n_threads = 0,
            const asr::SeparateOptions* separate = nullptr,
            const std::string& punc_model = {},
            const std::string& split_strategy = "rule", const std::string& split_model = {});

/// build：IR → ASS/SRT/TXT（View 层渲染）。template/bilingual/shift 只影响 ASS。
int run_build(const std::string& ir_path, const std::string& format, const std::string& output,
              const view::AssBuildOptions& options);

/// validate：校验 IR 文件。
int run_validate(const std::string& ir_path);
int run_compact(const std::string& ir_path, const std::string& out_path);

/// doctor：环境诊断（外部媒体工具 + 内嵌 ASR/TTS 后端可用性）。
int run_doctor();

/// models list：列出已注册提供者。
int run_models_list();

/// vsf：视频硬字幕帧定位（自实现，ROI 由用户传精准绝对坐标，无外部 VSF 依赖）。
int run_vsf(const std::string& input, const std::string& output,
             int roi_x, int roi_y, int roi_w, int roi_h, double sample_fps);

/// tts 命令参数（文本 → 语音；引擎无关）。
struct TtsOptions {
    std::string engine = "qwen3-tts";
    std::string model;  ///< qwen3-tts: talker GGUF；indextts: ONNX 模型目录
    std::string codec;  ///< qwen3-tts 必需的 tokenizer/codec GGUF
    std::string text;
    std::string ref_wav;
    std::string ref_text;
    std::string language;
    std::string out_wav = "tts.wav";
    int steps = 0;
    int n_threads = 0; ///< 0 = 自动（min(8, 核数)）
    float cfg_scale = 0.0f;
};

/// tts：文本 → 语音（qwen3-tts / indextts）。
int run_tts(const TtsOptions& opts);

/// tts --inspect：用自研 GGUF 解析器打印模型结构（元数据 + 张量分组），不合成。
int run_tts_inspect(const std::string& model_path);

/// dub 命令参数（媒体/IR → 转写/情感 → 逐句 TTS → 配音音轨）。
struct DubOptions {
    std::string input;
    std::string engine = "qwen3-tts";
    std::string model;
    std::string codec;
    std::string ref_wav;
    std::string ref_text;
    std::string language = "en";
    std::string asr_engine = "whisper";
    std::string asr_model;
    std::string emotion_model;
    std::string out_wav = "dub.wav";
    std::vector<std::string> voice_refs;  ///< SPEAKER=参考音频（可重复）
    std::vector<std::string> voice_texts; ///< SPEAKER=逐字稿（可重复）
    std::string voices_json;              ///< 音色配置文件
    std::string ir_out; ///< 非空则把本次 IR 另存一份
    int steps = 0;
    int n_threads = 0; ///< 0 = 自动（min(8, 核数)）
    float cfg_scale = 0.0f;
    bool fit_timing = true; ///< 按原句时隙对齐时长（--no-fit 关闭）
    int gap_ms = 200;       ///< 句间最小停顿
    float max_speed = 1.2f; ///< 时长压缩的变速上限
};

/// dub：配音线路串联（asr → emotion → tts）。
int run_dub(const DubOptions& opts);

/// translate 命令参数（IR/媒体 → 逐句翻译 → 新增 translation 轨道）。
struct TranslateCommandOptions {
    std::string input;                         ///< IR（*.json）或媒体文件
    std::string model;                         ///< 翻译模型 GGUF
    std::string engine = "auto";               ///< auto | m2m100 | opus-mt | wmt21 | madlad
    std::string from = "en";
    std::string to = "zh";
    std::string out_ir;                        ///< 非空则把结果 IR 存到该路径
    std::string asr_engine = "whisper";        ///< 媒体输入时使用
    std::string asr_model;
    int beam_size = 5;
    int max_tokens = 256;
    llm::Options llm;
};

/// translate：机器翻译（非 LLM：OPUS-MT/Marian、M2M100）。
int run_translate(const TranslateCommandOptions& opts);

/// OCR 命令选项。
struct OcrCommandOptions {
    std::string input;
    std::string out_ir;
    std::string language = "zh";
    std::string roi;                    ///< x,y,w,h（空 = 画面下方 40%）
    std::string model_dir = "models/ocr";
    double sample_fps = 1.0;
};

/// from-script：把纯文本剧本对齐到音视频，产出带时间码的 IR。
struct FromScriptOptions {
    std::string input;                    ///< 音视频（或已有 IR）
    std::string script;                   ///< 剧本文本（一行一条字幕）
    std::string out_ir;                   ///< 输出 IR（空 = <input>.script.iris.json）
    std::string language = "en";
    std::string asr_engine = "whisper";
    std::string asr_model = "models/ggml/ggml-base.bin";
    bool keep_speaker_labels_in_text = false; ///< true = 不剥离行首 "说话人:"
};

int run_from_script(const FromScriptOptions& options);

/// serve：启动 REST 服务（异步任务 API）。
int run_serve(const std::string& host, int port, const std::string& models_root,
              const std::string& work_root, bool json_mode = false);

/// models list：列出注册表里模型的本机安装状态。
int run_models_list(const std::string& models_root);

/// models install：按 key / 类别 / 全部下载模型（已存在则跳过）。
int run_models_install(const std::vector<std::string>& keys, const std::string& models_root,
                       const std::string& mirror, bool all, const std::string& category,
                       bool dry_run);

/// quality：统计 IR 的字幕质量指标并渲染 HTML 报告（有 error 时返回 2）。
int run_quality(const std::string& ir_path, const std::string& out_html);

/// lint：只读体检 ASS/SSA（样式、\K、语速、重叠、字体、歌词候选段），产出 HTML 报告。
/// 不改写任何字幕文件；有 error 时返回 2。
int run_lint(const std::vector<std::string>& inputs, const quality::LintOptions& options,
             const std::string& out_html);

/// style extract：从成品 ASS 反推样式模板 JSON（供 build --template 使用）。
int run_style_extract(const std::string& ass_path, const std::string& out_json,
                      const std::string& name);

/// karaoke：按源行 \K 节奏投影译文（或同码兄弟行），生成逐字 \K。
/// even_syllables=true 时兜底均分按音节加权；only_role 非空时只处理该角色（如 lyric）。
int run_karaoke(const std::string& ir_path, const std::string& out_path, bool fill_even,
                bool overwrite, int tolerance_ms, bool even_syllables = false,
                const std::string& only_role = {});

/// ocr：从视频硬字幕提取文本 → IR（vsf 找字幕段 + PP-OCR 识别）。
int run_ocr(const OcrCommandOptions& options);

/// convert：单个字幕文件（SRT/ASS/VTT）→ IR；也可以直接吃容器（MKV/MP4），
/// 用 --track 选一条内封字幕轨（索引 / lang=chi / title=SDH）先抽后导。
int run_convert(const std::vector<std::string>& inputs, const std::string& language,
                const std::string& out_path, const std::string& track_selector = {});

/// tracks：列出容器里的流（字幕轨的语言/标题/默认标志），只读。
int run_tracks(const std::string& media);

/// merge：两条字幕（IR 或字幕文件）→ 一条双语 IR（重分段）。
struct MergeOptions {
    std::string primary_language = "en";
    std::string secondary_language = "zh";
    std::string policy_sdh = "keep";          ///< keep | drop | glossary
    std::string policy_interjection = "keep";
    std::string policy_dialogue = "keep";
    std::string policy_lyric = "keep";
    std::string glossary_path;
    std::string report_path;                  ///< HTML 复核报告（可空）
    std::string split_strategy = "rule";     ///< rule | sat（对主副轨做可选语义分句）
    std::string split_model = "models/sat";  ///< SaT 模型目录
    bool allow_english_merge = true;
    int max_chars_en = 42;
    int max_lines_en = 2;
    double max_cps_en = 25.0;
    double max_cps_zh = 8.0;
};

/// merge：英文为主轨、中文为译文轨时的重分段（1:1 / 1:n / n:1 / 单语三档 / 注释）。
int run_merge(const std::string& primary_path, const std::string& secondary_path,
              const std::string& out_path, const MergeOptions& options);

/// combine：多个字幕文件 → IR（同一语言拼接去重；--bilingual 时第二份作为译文对齐）。
/// text_mode=true 时第二份按「纯文本，一行一条」处理，并按行序配对（不做时间重叠匹配）。
int run_combine(const std::vector<std::string>& inputs, const std::string& language,
                const std::string& out_path, bool bilingual, bool text_mode = false);

/// correct：IR 文本/时间码校正（空白、标点、空句、反序与重叠时间码、规则、术语）。
struct CorrectCommandOptions {
    std::string out_path;
    std::string rules_path;    ///< 规则文件（可空）
    std::string glossary_path; ///< 术语表（可空）
    bool dry_run = false;      ///< 只打印改动，不写文件
    int samples = 10;          ///< 打印的改动样例数
};
int run_correct(const std::string& ir_path, const CorrectCommandOptions& options);

/// diff：把 IR 与参考字幕按时间重叠对读，找漏句/多句/长度异常。
int run_diff(const std::string& ir_path, const std::string& reference_path,
             const std::string& out_html, double min_overlap, bool cjk_only = false);

/// align：强制对齐（Qwen3-ForcedAligner / CTC），把词级时间写进 IR。
int run_align(const std::string& media, const std::string& ir_path, const std::string& out_path,
              const asr::AlignOptions& options);

/// project scan：只读台账（事件 / 歌词覆盖 / 待补 / 错误）。
int run_project_scan(const std::vector<std::string>& inputs, const project::ScanOptions& options,
                     const std::string& out_html, const std::string& out_json);

/// project run：批量 convert→规则→correct→karaoke→build→lint（可增量）。
int run_project_run(const std::vector<std::string>& inputs, const project::RunOptions& options,
                    const std::string& manifest_path, const std::string& out_html);

/// run：按 pipeline.json 执行注册的静态操作符，产出 IR。
int run_pipeline(const std::string& pipeline_path, const std::string& input,
                 const std::string& language, const std::string& engine,
                 const std::string& model, const std::string& output,
                 const std::string& render_output);
int run_plugin_list();
int run_plugin_validate(const std::string& path);

/// plugin invoke：启动一个 T3 进程插件并调用 describe/invoke（见 docs/plugins.md）。
int run_plugin_invoke(const std::string& id, const std::string& method,
                      const std::string& kind, const std::string& input_path,
                      const std::string& params_json, const std::string& out_path,
                      int timeout_ms);

/// plugin check：对一份 JSON 输入运行所有启用的 T3 check 插件并汇总结果。
int run_plugin_check(const std::string& input_path, const std::string& params_json,
                     const std::string& only_id);

/// plugin new：生成 Python 进程插件骨架（plugin.json + main.py）。
int run_plugin_new(const std::string& id, const std::string& kind, const std::string& dir,
                   bool force);

/// emotion：语音情感分析（A/D/V 三维，Wav2Small ONNX）。
int run_emotion(const std::string& onnx_path, const std::string& wav_path);

} // namespace iris::controller
