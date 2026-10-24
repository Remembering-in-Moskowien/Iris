#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/subtitle/parser.hpp"

namespace iris::subtitle {

// ---------------------------------------------------------------------------
// 合并/对齐（对应 C# CombineMergeOperator / CombineDedupeOperator）：
//   - 同语言多文件：按时间排序后拼接，去掉时间与文本都重复的 cue；
//   - 双语（--bilingual）：以第一个文件为主轨，第二个文件的文本按「时间重叠最多」
//     贴到主轨 cue 的 translatedText 上。
// ---------------------------------------------------------------------------

struct CombineStats {
    int cues_in = 0;
    int cues_out = 0;
    int duplicates = 0;
    int paired = 0;    ///< 双语模式下成功配上译文的条数
    int unpaired = 0;  ///< 主轨里没找到译文对应物的条数
};

/// 合并同语言 cue：按 start 排序，去掉与上一条「时间相近且文本相同」的重复项。
bool merge_cues(std::vector<Cue>& cues, std::int64_t tolerance_ms, CombineStats& stats,
                std::string& error);

/// 把中文/英文译文 cue 对齐到主轨（按时间重叠最多且 >= 重叠阈值）。
/// overlap_ratio 为「重叠时长 / 主轨 cue 时长」的下限（0.1 = 10%）。
void align_translations(std::vector<Cue>& primary, const std::vector<Cue>& second,
                        double overlap_ratio, CombineStats& stats);

} // namespace iris::subtitle
