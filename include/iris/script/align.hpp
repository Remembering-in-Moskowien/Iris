#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::script {

// ---------------------------------------------------------------------------
// 剧本对齐（对应 C# 的 ScriptLoaderOperator + ScriptTimelineMapperOperator）。
//
// 剧本格式：**一行台词 = 一条字幕**（空行忽略）。可选的行首说话人标签：
//     SPEAKER_1: 你好
//     张三：你好
// 会被拆成 speaker + text（配合 dub 多音色）。
//
// 对齐：把 ASR 的句子序列与剧本行序列做 Needleman-Wunsch 全局对齐
// （相似度 = 归一化词元的重合度），匹配上的行直接用该 ASR 句的时间码；
// 未匹配的行在相邻锚点之间按字符数比例插值；一个锚点都没有时，
// 按全片时长与字符数比例铺开（仍然能产出可用的时间轴）。
// ---------------------------------------------------------------------------

struct ScriptLine {
    std::string text;
    std::string speaker; ///< 行首标签（可为空）
};

/// 一行对齐结果。
struct AlignedLine {
    ScriptLine line;
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    int matched_index = -1; ///< 命中的 ASR 句下标；-1 表示插值
};

/// 读取剧本：按行切分、去空行、可选剥离行首说话人标签。
std::vector<ScriptLine> load_script(const std::string& path, bool strip_speaker,
                                    std::string& error);

/// 归一化词元：小写、去标点、中文按字切分（便于中英混排对齐）。
std::vector<std::string> tokenize(const std::string& text);

/// 两段文本的相似度（0..1）：词元重合度（Dice 系数）。
double similarity(const std::string& a, const std::string& b);

/// 全局对齐：返回与 lines 等长的结果；total_ms 用于无锚点时的兜底铺开。
std::vector<AlignedLine> align(const std::vector<ScriptLine>& lines,
                               const std::vector<model::Sentence>& asr_sentences,
                               std::int64_t total_ms);

/// 把对齐结果写成 IR 轨道（type = transcribe）。
model::Track to_track(const std::vector<AlignedLine>& aligned, const std::string& language);

} // namespace iris::script
