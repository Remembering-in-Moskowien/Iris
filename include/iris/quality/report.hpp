#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::quality {

// ---------------------------------------------------------------------------
// 字幕质量报告（对应 C# 的 Quality* 系列算子 + HTML 报告）。
//
// 全部基于 IR 的纯文本/时间码统计，不依赖模型：
//   - 条目数、覆盖时长、每条字符数与「每秒字符数」（CPS，读起来太快会点名）
//   - 句间空隙：重叠（负间隔）、过长空白（> 2 s）、最短/平均/最长间隔
//   - 重复文本、空行
//   - 说话人分布（做过说话人分割时）
// ---------------------------------------------------------------------------

struct CueIssue {
    int cue_index = 0;       ///< 轨道内序号（1 起）
    std::string kind;        ///< overlap | long-gap | fast | duplicate | empty
    std::string severity;    ///< error | warning | info
    std::string detail;      ///< 人类可读说明
};

struct TrackMetrics {
    std::string type;
    std::string language;
    int cues = 0;
    std::int64_t span_ms = 0;   ///< 首条起点 → 末条终点
    double coverage = 0.0;      ///< 字幕覆盖时长 / 总时长
    int min_chars = 0;
    int max_chars = 0;
    double avg_chars = 0.0;
    double avg_cps = 0.0;       ///< 平均每秒字符数
    double max_cps = 0.0;
    int fast_cues = 0;          ///< 超过 CPS 上限的条数
    int overlaps = 0;
    int long_gaps = 0;
    std::int64_t min_gap_ms = 0;
    std::int64_t max_gap_ms = 0;
    double avg_gap_ms = 0.0;
    int duplicates = 0;
    int empty_cues = 0;
    std::map<std::string, int> speakers;
    std::vector<CueIssue> issues;
};

struct Report {
    std::vector<TrackMetrics> tracks;
    int errors = 0;
    int warnings = 0;
};

/// CPS 上限：中文 12 字/秒、拉丁 20 字符/秒（经验值，超过就算「太快」）。
double max_cps_for(const std::string& language);

/// 统计一条轨道的指标与问题。
TrackMetrics analyze_track(const model::Track& track, double max_cps);

/// 统计整个文档。
Report analyze(const model::IrDocument& doc);

/// 渲染自包含 HTML（内联样式，不依赖外部资源）。
std::string to_html(const model::IrDocument& doc, const Report& report);

} // namespace iris::quality
