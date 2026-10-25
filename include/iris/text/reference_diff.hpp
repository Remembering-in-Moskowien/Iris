#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"
#include "iris/subtitle/parser.hpp"

namespace iris::text {

// ---------------------------------------------------------------------------
// 参考字幕对读：拿一份「权威版本」（字幕组成品、官方字幕）和当前 IR 对齐，
// 找漏句、多句、长度异常（疑似错译/漏译）的句子。
// ---------------------------------------------------------------------------

struct RefPair {
    int index = 0; ///< 句子序号（1 起）
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string ours;
    std::string reference;
    std::string kind; ///< no-ref（参考没覆盖）| uncovered（我们没覆盖参考）| length
};

struct RefDiffReport {
    int sentences = 0;
    int reference_cues = 0;
    int matched = 0;
    int missing_in_ours = 0;
    int missing_in_reference = 0;
    int length_mismatch = 0;
    std::vector<RefPair> issues;
};

RefDiffReport diff_reference(const model::IrDocument& doc,
                             const std::vector<subtitle::Cue>& reference,
                             double min_overlap = 0.3, int max_issues = 200,
                             bool cjk_only = false);

std::string ref_diff_html(const RefDiffReport& report, const std::string& title);

} // namespace iris::text
