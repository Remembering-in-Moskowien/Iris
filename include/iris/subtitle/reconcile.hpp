#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "iris/model/ir_document.hpp"

namespace iris::subtitle {

// ---------------------------------------------------------------------------
// 双轨重分段（bilingual reconciliation）。
//
// 内封中英双轨的素材通常是「两次独立制作」：主轨（英文 SDH）有音效/歌词/说话人
// 标签，副轨（中文）只译对白，两边断句也不同（约 1/5 的条数不对齐）。直接把两条
// 时间轴按「最大重叠」配对会同时出现三种错误：同一条译文被贴到多行、短句被误配、
// 译文行被丢弃。
//
// 这里的做法：
//   1. 主轨当事件骨架（用户可选英文或中文为主）；
//   2. 每条副轨只归属一次（按最大重叠 + 阈值），从结构上杜绝重复；
//   3. 一条副轨横跨两条主轨时，先在空格/标点处按时长比切开；
//      切不动就在允许时把两条主轨并成一条；再不行就留主轨单语；
//   4. 主轨单语行按类别（音效/短句/对白/歌词）走 keep / drop / glossary；
//   5. 副轨独有的注释（（出口）之类）保留为单语事件。
// ---------------------------------------------------------------------------

/// 一条 cue 的角色。
enum class CueRole { dialogue, sdh, lyric, interjection, annotation };

const char* role_name(CueRole role);

/// 分类规则（纯文本，可单测）：
/// - 含 ♪ → lyric；
/// - 每一行都是 [......] 或 - [......] → sdh（音效/音乐）；
/// - 整条被（...）/(...) 包住 → annotation；
/// - 去掉引导 - 后 ≤2 个词且 ≤14 个可见字符 → interjection；
/// - 其余 → dialogue。
CueRole classify_cue(const std::string& text);

/// 主轨单语行的处理策略（按类别）。
struct SingleLinePolicy {
    std::string sdh = "keep";          ///< keep | drop | glossary
    std::string interjection = "keep";
    std::string dialogue = "keep";
    std::string lyric = "keep";
};

struct ReconcileOptions {
    std::string primary_language = "en";
    std::string secondary_language = "zh";
    SingleLinePolicy policy;
    std::string glossary_path; ///< from<TAB>to；glossary 策略使用
    bool allow_english_merge = true; ///< 译文切不开时，是否允许把两条主轨并成一条
    int max_lines_en = 2;
    int max_chars_en = 42;
    int max_chars_zh = 20;
    double max_cps_en = 25.0;
    double max_cps_zh = 8.0;
    int min_split_chars = 3; ///< 切分后每侧至少几个字
    bool keep_annotations = true;
    /// Optional cross-lingual semantic similarity in [0, 1]. When unset, the
    /// deterministic structural fallback is used.
    std::function<double(const std::string&, const std::string&)> semantic_similarity;
    double semantic_weight = 0.55;
    double min_iou = 0.10;
    double min_alignment_confidence = 0.35;
};

/// 需要人看一眼的问题行。
struct ReconcileRow {
    std::string kind; ///< dropped | split-failed | glossary-miss | cps-zh | cps-en | unlinked
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    std::string text;   ///< 主轨文本
    std::string detail; ///< 副轨文本或补充说明
};

struct ReconcileStats {
    int primary_cues = 0;
    int secondary_cues = 0;
    int paired = 0;            ///< 有译文的主轨事件数
    int merged_secondary = 0;  ///< 1 主轨 ← 2+ 副轨（译文拼接）
    int split_secondary = 0;   ///< 1 副轨 → 2 主轨（按标点/比例切开）
    int split_failed = 0;      ///< 想切但切不动
    int english_merged = 0;    ///< 2 主轨并成 1 条事件
    int secondary_only = 0;    ///< 副轨独有事件（注释）
    int kept = 0;              ///< 单语行保留
    int dropped = 0;           ///< 单语行丢弃
    int glossed = 0;           ///< 单语行经术语表补出译文
    int unlinked_secondary = 0;///< 没找到归属、又不是注释的副轨行
    int cps_over_en = 0;
    int cps_over_zh = 0;
    int alignment_groups = 0;
    int alignment_rejected = 0;
    double alignment_confidence = 0.0;
};

/// 读术语表：`from<TAB>to`，`#` 开头为注释；空行忽略。
bool load_glossary(const std::string& path, std::map<std::string, std::string>& out,
                   std::string& error);

/// 主副轨重分段。out.language = options.primary_language，句子带 role/style；
/// 译文写进 translated_text。review 收集需要人工复核的行（按时间排序）。
bool reconcile(const model::Track& primary, const model::Track& secondary,
               const ReconcileOptions& options, model::Track& out, ReconcileStats& stats,
               std::vector<ReconcileRow>& review, std::string& error);

/// 可见长度（CJK 每字 1，拉丁按字符；忽略 ASS 标签与换行）。
int visible_length(const std::string& text);

} // namespace iris::subtitle
