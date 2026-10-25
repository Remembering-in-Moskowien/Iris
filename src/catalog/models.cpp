#include "iris/catalog/models.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace iris::catalog {

namespace {

constexpr const char* kHf = "https://huggingface.co/";

constexpr std::int64_t mb(std::int64_t v) { return v * 1024 * 1024; }

std::vector<ModelEntry> build() {
    using F = ModelFile;
    std::vector<ModelEntry> v;

    // ---------------- ASR ----------------
    v.push_back({"whisper-tiny", "asr", "Whisper tiny（ggml）", "ggml",
                 {{std::string(kHf) + "ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin",
                   "ggml-tiny.bin"}},
                 mb(75), "whisper", "最快，适合验证链路"});
    v.push_back({"whisper-base", "asr", "Whisper base（ggml）", "ggml",
                 {{std::string(kHf) + "ggerganov/whisper.cpp/resolve/main/ggml-base.bin",
                   "ggml-base.bin"}},
                 mb(142), "whisper", "质量/速度平衡的默认选择"});
    v.push_back({"qwen3-asr-0.6b", "asr", "Qwen3-ASR 0.6B（ggml q4_k）", "qwen3",
                 {{std::string(kHf) + "cstr/qwen3-asr-0.6b-GGUF/resolve/main/qwen3-asr-0.6b-q4_k.gguf",
                   "qwen3-asr-0.6b-q4_k.gguf"}},
                 mb(602), "qwen3-asr", "多语种；CPU 上约 3.9 s / 11 s 音频"});
    v.push_back({"whisper-ct2-base", "asr", "Whisper base（CTranslate2）", "ct2/whisper-base",
                 {{std::string(kHf) + "Systran/faster-whisper-base/resolve/main/model.bin", "model.bin"},
                  {std::string(kHf) + "Systran/faster-whisper-base/resolve/main/config.json", "config.json"},
                  {std::string(kHf) + "Systran/faster-whisper-base/resolve/main/tokenizer.json",
                   "tokenizer.json"},
                  {std::string(kHf) + "Systran/faster-whisper-base/resolve/main/vocabulary.txt",
                   "vocabulary.txt"},
                  {std::string(kHf) + "Systran/faster-whisper-base/resolve/main/preprocessor_config.json",
                   "preprocessor_config.json"}},
                 mb(143), "whisper-ct2", "CTranslate2 目录式加载"});
    v.push_back({"whisper-onnx-tiny-en", "asr", "Whisper tiny.en（ONNX）", "onnx/whisper-tiny-en",
                 {{std::string(kHf) + "onnx-community/whisper-tiny.en/resolve/main/onnx/encoder_model.onnx",
                   "onnx/encoder_model.onnx"},
                  {std::string(kHf) +
                       "onnx-community/whisper-tiny.en/resolve/main/onnx/decoder_model_merged.onnx",
                   "onnx/decoder_model_merged.onnx"},
                  {std::string(kHf) + "onnx-community/whisper-tiny.en/resolve/main/tokenizer.json",
                   "tokenizer.json"},
                  {std::string(kHf) + "onnx-community/whisper-tiny.en/resolve/main/config.json",
                   "config.json"},
                  {std::string(kHf) +
                       "onnx-community/whisper-tiny.en/resolve/main/generation_config.json",
                   "generation_config.json"}},
                 mb(147), "whisper-onnx", "自写 ONNX 编排（encoder/decoder）"});
    v.push_back({"sensevoice", "asr", "SenseVoice Small（ONNX int8）", "sensevoice",
                 {{std::string(kHf) +
                       "csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/resolve/main/"
                       "model.int8.onnx",
                   "model.int8.onnx"},
                  {std::string(kHf) +
                       "csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/resolve/main/"
                       "tokens.txt",
                   "tokens.txt"}},
                 mb(229), "sensevoice-onnx", "中英日韩粤，CTC，无需分词器文件"});
    v.push_back({"paraformer-zh", "asr", "Paraformer 中文（ONNX int8）", "paraformer",
                 {{std::string(kHf) +
                       "csukuangfj/sherpa-onnx-paraformer-zh-2024-03-09/resolve/main/model.int8.onnx",
                   "model.int8.onnx"},
                  {std::string(kHf) +
                       "csukuangfj/sherpa-onnx-paraformer-zh-2024-03-09/resolve/main/tokens.txt",
                   "tokens.txt"}},
                 mb(217), "paraformer-onnx", "中文专用；输出无标点，建议配 --punc"});

    // ---------------- 强制对齐 ----------------
    v.push_back({"qwen3-forced-aligner", "align", "Qwen3-ForcedAligner 0.6B（GGUF q4_k）", "aligner",
                 {{std::string(kHf) +
                       "cstr/qwen3-forced-aligner-0.6b-GGUF/resolve/main/"
                       "qwen3-forced-aligner-0.6b-q4_k.gguf",
                   "qwen3-forced-aligner-0.6b-q4_k.gguf"}},
                 mb(500), "align",
                 "词级/字级时间戳（Qwen3-ASR 架构）；给 karaoke 提供 words"});

    // ---------------- TTS ----------------
    v.push_back({"qwen3-tts-0.6b", "tts", "Qwen3-TTS 0.6B（ggml q8_0）+ 12 Hz tokenizer", "qwen3-tts",
                 {{std::string(kHf) +
                       "cstr/qwen3-tts-0.6b-base-GGUF/resolve/main/qwen3-tts-12hz-0.6b-base-q8_0.gguf",
                   "qwen3-tts-12hz-0.6b-base-q8_0.gguf"},
                  {std::string(kHf) +
                       "cstr/qwen3-tts-tokenizer-12hz-GGUF/resolve/main/qwen3-tts-tokenizer-12hz.gguf",
                   "qwen3-tts-tokenizer-12hz.gguf"}},
                 mb(1282), "qwen3-tts", "音色克隆；需要 24 kHz 参考音频 + 逐字稿"});
    v.push_back({"qwen3-tts-voices", "tts", "Qwen3-TTS 预设音色（ggml）", "qwen3-tts",
                 {{std::string(kHf) +
                       "cstr/qwen3-tts-voices-GGUF/resolve/main/qwen3-tts-voice-default.gguf",
                   "qwen3-tts-voice-default.gguf"}},
                 0, "qwen3-tts", "可选的预设音色，不依赖参考音频"});

    // ---------------- 翻译 ----------------
    v.push_back({"m2m100-418m", "translation", "M2M100 418M（ggml q8_0）", "translation",
                 {{std::string(kHf) + "cstr/m2m100-418m-GGUF/resolve/main/m2m100-418m-q8_0.gguf",
                   "m2m100-418m-q8_0.gguf"}},
                 mb(502), "m2m100", "多语种互译（100 种）"});
    v.push_back({"opus-mt-de-en", "translation", "OPUS-MT de→en（ggml q8_0）", "translation",
                 {{std::string(kHf) + "cstr/opus-mt-de-en-GGUF/resolve/main/opus-mt-de-en-q8_0.gguf",
                   "opus-mt-de-en-q8_0.gguf"}},
                 mb(81), "opus-mt", "专用双语模型，质量优于多语种大模型"});
    // 没有可直接下载的 GGUF：只登记文件名用于状态检查，安装时提示需本地转换。
    v.push_back({"opus-mt-en-zh", "translation", "OPUS-MT en→zh（本地转换，不自动下载）", "translation",
                 {{"", "opus-mt-en-zh-f16.gguf"}},
                 mb(154), "opus-mt",
                 "cstr 未提供 en→zh 的 GGUF；用 tools/convert_marian 从 Helsinki-NLP/opus-mt-en-zh 转换"});

    // ---------------- 人声分离 ----------------
    v.push_back({"htdemucs", "separation", "HTDemucs（ggml q8_0，4 段）", "separation",
                 {{std::string(kHf) + "cstr/htdemucs-GGUF/resolve/main/htdemucs-q8_0.gguf",
                   "htdemucs-q8_0.gguf"}},
                 mb(53), "htdemucs", "默认分离模型；单线程，iris 用分块并行加速"});
    v.push_back({"mel-band-roformer-vocals", "separation", "Mel-Band RoFormer（vocals only, f16）",
                 "separation",
                 {{std::string(kHf) +
                       "cstr/mel-band-roformer-vocals-GGUF/resolve/main/mel-band-roformer-vocals-f16.gguf",
                   "mel-band-roformer-vocals-f16.gguf"}},
                 mb(436), "mel-band-roformer", "质量更好但更慢（11 s 音频约 283 s）"});

    // ---------------- 说话人分割 ----------------
    v.push_back({"wespeaker-resnet34-lm", "diarize", "WeSpeaker ResNet34-LM（ggml）", "",
                 {{std::string(kHf) +
                       "cstr/wespeaker-resnet34-lm-GGUF/resolve/main/wespeaker-resnet34-lm.gguf",
                   "wespeaker-resnet34-lm.gguf"}},
                 mb(23), "diarize", "放 models/ 根目录，或 ~/.cache/crispasr/"});

    // ---------------- 标点 / 分句 ----------------
    v.push_back({"fireredpunc-q4_k", "punc", "FireRedPunc（ggml q4_k）", "punc",
                 {{std::string(kHf) + "cstr/fireredpunc-GGUF/resolve/main/fireredpunc-q4_k.gguf",
                   "fireredpunc-q4_k.gguf"}},
                 mb(56), "punc", "中文 ASR 补标点；--punc 使用"});
    v.push_back({"sat-3l-sm", "sat", "SaT 3L-sm（ONNX）+ XLM-R 分词器", "sat",
                 {{std::string(kHf) +
                       "segment-any-text/sat-3l-sm/resolve/main/model_optimized.onnx",
                   "model_optimized.onnx"},
                  {std::string(kHf) + "FacebookAI/xlm-roberta-base/resolve/main/tokenizer.json",
                   "tokenizer.json"},
                  {std::string(kHf) +
                       "FacebookAI/xlm-roberta-base/resolve/main/sentencepiece.bpe.model",
                   "sentencepiece.bpe.model"}},
                 mb(422), "sat", "语义分句（--split sat）"});

    // ---------------- OCR ----------------
    v.push_back({"ppocrv5", "ocr", "PP-OCRv5 det + rec + 字典", "ocr",
                 {{std::string(kHf) + "bluecopa/rapidocr-onnx/resolve/main/PP-OCRv5_det.onnx",
                   "PP-OCRv5_det.onnx"},
                  {std::string(kHf) +
                       "bluecopa/rapidocr-onnx/resolve/main/ch_PP-OCRv5_rec_mobile_infer.onnx",
                   "ch_PP-OCRv5_rec_mobile_infer.onnx"},
                  {std::string(kHf) + "bluecopa/rapidocr-onnx/resolve/main/ppocrv5_dict.txt",
                   "ppocrv5_dict.txt"}},
                 mb(100), "ocr", "硬字幕提取（ocr 命令）"});

    return v;
}

} // namespace

const std::vector<ModelEntry>& all() {
    static const std::vector<ModelEntry> kEntries = build();
    return kEntries;
}

const ModelEntry* find(const std::string& key) {
    for (const auto& entry : all()) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

std::vector<const ModelEntry*> by_category(const std::string& category) {
    std::vector<const ModelEntry*> out;
    for (const auto& entry : all()) {
        if (entry.category == category) out.push_back(&entry);
    }
    return out;
}

std::vector<std::string> categories() {
    std::vector<std::string> out;
    for (const auto& entry : all()) {
        if (std::find(out.begin(), out.end(), entry.category) == out.end()) {
            out.push_back(entry.category);
        }
    }
    return out;
}

std::string target_dir(const ModelEntry& entry, const std::string& models_root) {
    namespace fs = std::filesystem;
    if (entry.subdir.empty()) return models_root;
    return (fs::path(models_root) / entry.subdir).string();
}

std::string target_path(const ModelEntry& entry, const ModelFile& file,
                        const std::string& models_root) {
    namespace fs = std::filesystem;
    return (fs::path(target_dir(entry, models_root)) / file.filename).string();
}

namespace {

/// 运行时还会去这些目录按文件名找模型（CrispASR 的缓存约定）。
std::vector<std::string> cache_dirs() {
    namespace fs = std::filesystem;
    std::vector<std::string> dirs;
    if (const char* home = std::getenv("USERPROFILE"); home != nullptr && *home != '\0') {
        dirs.push_back((fs::path(home) / ".cache" / "crispasr").string());
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        dirs.push_back((fs::path(home) / ".cache" / "crispasr").string());
    }
    return dirs;
}

/// 单文件条目允许放在缓存目录里（例如 wespeaker、htdemucs、punc）。
bool present_anywhere(const ModelEntry& entry, const ModelFile& file, const std::string& models_root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto path = target_path(entry, file, models_root);
    if (fs::exists(path, ec) && fs::file_size(path, ec) > 0) return true;
    if (entry.files.size() == 1) {
        for (const auto& dir : cache_dirs()) {
            const auto alt = (fs::path(dir) / file.filename).string();
            if (fs::exists(alt, ec) && fs::file_size(alt, ec) > 0) return true;
        }
    }
    return false;
}

} // namespace

bool installed(const ModelEntry& entry, const std::string& models_root) {
    if (entry.files.empty()) return false;
    for (const auto& file : entry.files) {
        if (!present_anywhere(entry, file, models_root)) return false;
    }
    return true;
}

std::string rewrite_url(const std::string& url, const std::string& mirror) {
    if (mirror.empty()) return url;
    const std::string host = "https://huggingface.co/";
    if (url.rfind(host, 0) != 0) return url;
    std::string base = mirror;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/" + url.substr(host.size());
}

} // namespace iris::catalog
