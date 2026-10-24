#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iris::catalog {

// ---------------------------------------------------------------------------
// 模型注册表：把「跑通各条链路需要的模型」固化成一张表，
// 让 `iris models install` 能一条命令装齐，`iris doctor` 能报缺哪个。
//
// 只收录**可下载**的条目；本地转换出来的模型（例如自己转的 OPUS-MT en→zh）
// 在 note 里说明来源与做法，不参与自动安装。
// ---------------------------------------------------------------------------

struct ModelFile {
    std::string url;      ///< huggingface.co 地址（可用 IRIS_HF_MIRROR 重写）
    std::string filename; ///< 落盘文件名（可带子目录，如 onnx/encoder_model.onnx）
};

struct ModelEntry {
    std::string key;      ///< 唯一标识，如 whisper-base
    std::string category; ///< asr / tts / translation / separation / diarize / punc / sat / ocr
    std::string name;     ///< 人类可读名称
    std::string subdir;   ///< 相对 models/ 的目录（如 ggml、qwen3-tts）
    std::vector<ModelFile> files;
    std::int64_t bytes = 0; ///< 大约总大小（仅展示）
    std::string engine;     ///< 对应的 --engine / 用途（可选）
    std::string note;       ///< 说明（授权、用途、替代品）
};

/// 全部条目（顺序即展示顺序）。
const std::vector<ModelEntry>& all();

/// 按 key 查找；找不到返回 nullptr。
const ModelEntry* find(const std::string& key);

/// 某个类别下的全部条目。
std::vector<const ModelEntry*> by_category(const std::string& category);

/// 已知类别（按注册表出现顺序）。
std::vector<std::string> categories();

/// 条目在 models_root 下的目录。
std::string target_dir(const ModelEntry& entry, const std::string& models_root);

/// 单个文件的落盘路径。
std::string target_path(const ModelEntry& entry, const ModelFile& file, const std::string& models_root);

/// 所有文件是否都已存在。
bool installed(const ModelEntry& entry, const std::string& models_root);

/// 把 huggingface.co 的地址重写到镜像（mirror 为空则原样返回）。
std::string rewrite_url(const std::string& url, const std::string& mirror);

} // namespace iris::catalog
