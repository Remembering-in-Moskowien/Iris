#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::view {

// ---------------------------------------------------------------------------
// IR → ASS 成品渲染（M1：模板 / 双语 / 整片偏移 / 4K 缩放）。
//
// 样式来源优先级：
//   1. --template（.json 模板，或直接用 .ass 成品反推）
//   2. IR 里的 styles（convert 过的成品能原样往返）
//   3. 内置 1080p 模板（MLP 项目的 chi / eng / 角色样式）
// ---------------------------------------------------------------------------

struct AssBuildOptions {
    std::string template_path;        ///< .json 或 .ass；空 = IR 样式 / 内置模板
    bool bilingual = false;           ///< 每个句子输出「译文 + 原文」两条同码事件
    std::int64_t shift_ms = 0;        ///< 额外整片偏移（叠加在 IR timelineOffsetMs 上）
    bool use_ir_offset = true;        ///< 是否叠加 IR 里的 timelineOffsetMs
    double scale = 1.0;               ///< 1.0 = 模板原尺寸；2.0 = 4K
    bool crlf = true;                 ///< Aegisub 习惯 CRLF
    bool bom = false;
    std::string title;                ///< 非空则覆盖 Script Info 的 Title
    bool keep_comments = true;
};

struct AssBuildStats {
    int lines = 0;
    int translated = 0;
    int comments = 0;
    int skipped_empty = 0;
};

struct AssBuildResult {
    std::string text;
    AssBuildStats stats;
    std::vector<std::string> warnings;
};

/// 渲染 IR → ASS。失败时返回 false 并写 error。
bool build_ass(const model::IrDocument& doc, const AssBuildOptions& options, AssBuildResult& out,
               std::string& error);

/// 读取模板：.ass/.ssa 直接解析；.json 读 {"scriptInfo": [[k,v]], "styles": [...]}。
bool load_ass_template(const std::string& path, subtitle::AssScript& out, std::string& error);

/// 内置 1080p 模板（MLP：中文底部 28 / 英文对白 100 / 英文歌词 880）。
subtitle::AssScript default_ass_template();

} // namespace iris::view
