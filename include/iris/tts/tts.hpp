#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iris::tts {

// ---------------------------------------------------------------------------
// 语音合成（TTS）统一接口。
//
// 引擎分两类，均由 Iris 自写编排、运行时只引用打包好的推理 DLL：
//   qwen3-tts  CrispASR 进程内后端（Qwen3-TTS 12Hz GGUF + 12Hz tokenizer GGUF）
//   indextts   onnxruntime（IndexTTS 2.5 多 ONNX 图，编排仍在调试）
//
// 命令层（controller）只认这里的接口，不直接接触任何推理运行时。
// ---------------------------------------------------------------------------

/// TTS 引擎。
enum class TtsEngine {
    Qwen3Tts, ///< CrispASR `qwen3-tts` 后端；音色克隆走参考音频 + 逐字稿。
    IndexTts, ///< IndexTTS 2.5 ONNX（实验性）。
};

/// 解析 `--engine` 名称：`qwen3-tts`（别名 `qwen3tts` / `qwen3`）与
/// `indextts`（别名 `index-tts`）。成功返回 true 并写入 out。
bool parse_tts_engine(const std::string& name, TtsEngine& out);

/// 引擎规范名（写回 IR / 日志用），永不返回 nullptr。
const char* tts_engine_name(TtsEngine engine);

/// 引擎运行时是否随本次构建可用（对应 IRIS_HAS_CRISPASR / IRIS_HAS_ONNX）。
bool tts_engine_available(TtsEngine engine);

/// 一次合成请求（引擎无关；未使用的字段由各引擎忽略）。
struct TtsRequest {
    TtsEngine engine = TtsEngine::Qwen3Tts;
    std::string model;       ///< 主权重：qwen3-tts 为 talker GGUF，indextts 为模型目录
    std::string codec_model; ///< qwen3-tts 必需的 tokenizer/codec GGUF
    std::string text;        ///< 待合成文本
    std::string ref_wav;     ///< 参考音频（音色克隆；空 = 引擎默认音色）
    std::string ref_text;    ///< 参考音频逐字稿（qwen3-tts 用 WAV 克隆时必需）
    std::string language;    ///< 语言提示（如 zh / en；空 = 后端自动）
    int steps = 0;           ///< 采样步数（0 = 后端默认）
    float cfg_scale = 0.0f;  ///< CFG 强度（0 = 后端默认）
    int n_threads = 0;       ///< 推理线程数（0 = 硬件并发）
};

/// 合成结果：单声道 float32 PCM。
struct TtsResult {
    std::vector<float> pcm;
    int sample_rate = 0; ///< 引擎原生采样率（qwen3-tts 为 24000）

    /// 时长（秒）。
    [[nodiscard]] double duration_s() const {
        return sample_rate > 0 ? static_cast<double>(pcm.size()) / sample_rate : 0.0;
    }
};

/// 按请求合成。失败时 error 写入人类可读原因（已含引擎前缀）。
bool synthesize(const TtsRequest& req, TtsResult& out, std::string& error);

// ---- 引擎专用入口（由 synthesize 分发；一般不必直接调用）------------------
bool synth_qwen3_tts(const TtsRequest& req, TtsResult& out, std::string& error);
bool synth_indextts(const TtsRequest& req, TtsResult& out, std::string& error);

/// 推理线程数解析：requested <= 0 时取硬件并发数。
int resolve_tts_threads(int requested);

// ---- WAV I/O（16-bit PCM 单声道；tts / dub 共用）--------------------------
/// 写 16-bit PCM WAV（输入为 [-1,1] float32）。
bool write_wav(const std::string& path, const std::vector<float>& pcm, int sample_rate,
               std::string& error);

/// 读 16-bit PCM WAV（取首声道），保留文件原始采样率。
bool read_wav(const std::string& path, std::vector<float>& pcm, int& sample_rate,
              std::string& error);

/// 保证参考音频是 target_rate 的单声道 WAV：
/// 采样率已一致时直接把原路径写到 out_path，否则用外部 ffmpeg 转一份到临时目录。
/// 返回 false 时 error 说明原因（缺 ffmpeg / WAV 不可读）。
bool ensure_reference_rate(const std::string& ref_wav, int target_rate, std::string& out_path,
                           std::string& error);

// ---- 时间轴混音（dub 链路）-----------------------------------------------
/// 把 src 叠加到 dst 的 offset_samples 处；dst 不足时自动补零扩展。
/// 纯相加、不限幅——调用方用 peak_abs() 检查后统一归一，避免交叠处硬削波。
void mix_at(std::vector<float>& dst, const std::vector<float>& src, std::size_t offset_samples);

/// PCM 的绝对值峰值（用于配音音轨的整体归一）。空输入返回 0。
float peak_abs(const std::vector<float>& pcm);

// ---- 时长对齐（dub 链路）--------------------------------------------------
/// 裁掉首尾静音（内部停顿保留）：低于 threshold 的帧视为静音，
/// 两端各留 keep_ms 余量防止切掉字头字尾。原地修改 pcm。
void trim_silence(std::vector<float>& pcm, int sample_rate, float threshold = 0.003f,
                  int keep_ms = 20);

/// 变速不变调（ffmpeg atempo）。speed > 1 为加速（缩短时长），<= 1 原样返回。
/// 需要外部 ffmpeg；失败时 error 说明原因，out 不被修改。
bool time_compress(const std::vector<float>& in, int sample_rate, float speed,
                   std::vector<float>& out, std::string& error);

} // namespace iris::tts
