#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::text {

// ---------------------------------------------------------------------------
// 文本清理 / 校正（对应 C# 的 CorrectCommand + SubtitleTextCorrectorOperator +
// OverlapResolutionOperator + SubtitleTrackCheckerOperator 的核心规则）。
//
// 全部是纯字符串/时间码逻辑，不依赖模型，可单独单测。
// ---------------------------------------------------------------------------

/// 一次校正的统计（打印给用户）。
struct CleanupReport {
    int sentences_in = 0;
    int dropped = 0;          ///< 清空后为空的句子
    int whitespace_fixed = 0; ///< 首尾空白/多余空白/汉字间空格
    int punctuation_fixed = 0;///< 重复标点、半角标点
    int timing_fixed = 0;     ///< 反序/零长度时间码
    int overlap_fixed = 0;    ///< 与上一句重叠（裁掉重叠部分）
};

/// 清理单句文本：
/// - 去掉首尾空白、把连续空白压成一个空格；
/// - 去掉汉字/中文标点两侧的 ASCII 空格；
/// - 合并重复标点（"，。" → "。"，"。。" → "。"，"，，" → "，"）；
/// - 半角 , ; : ! ? 转全角（中文语境）。
/// 返回清理后的文本；report 可为 nullptr。
std::string clean_text(const std::string& text, CleanupReport* report = nullptr);

/// 就地校正一组句子（按时间顺序）：
/// - 文本清理（clean_text）；
/// - 清空后为空的句子直接丢弃；
/// - end <= start 的时间码修正为 start + 1ms；
/// - 与上一句重叠时将本句起点推到上一句结束（若因此变为零长度则丢弃）。
void clean_sentences(std::vector<model::Sentence>& sentences, CleanupReport& report);

/// 对整个 IR 文档的所有轨道执行校正。
CleanupReport correct_document(model::IrDocument& doc);

} // namespace iris::text
