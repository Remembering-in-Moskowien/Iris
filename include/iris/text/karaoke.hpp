#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iris::text {

// ---------------------------------------------------------------------------
// 卡拉 OK 打轴：把「已经有了逐词/逐字节奏的源行」投影成译文的逐字 \K。
//
// 源行 = 英文歌词（手工打好的 \K），译文 = 中文（字数不同）。做法是把源行的
// 每个 \K 段落当作一段时长，按累计时长把译文字符插值进去；句首的引导 \K 和
// 句尾的留白 \K 原样保留，因此 Σ\K 与行时长严格相等。
// ---------------------------------------------------------------------------

struct KaraokeSegment {
    int cs = 0;          ///< 时长（百分秒）
    std::string text;    ///< 该段的文本（引导/留白为空）
};

struct KaraokeResult {
    std::string raw;   ///< 生成的 ASS 文本（含 \K 标签）
    int total_cs = 0;  ///< Σ 时长
    int tokens = 0;    ///< token 数
};

/// 解析 ASS 文本里的 \K/\k/\kf/\ko 段落（保留每段文本，忽略其它覆盖标签）。
std::vector<KaraokeSegment> parse_karaoke_segments(const std::string& raw);

/// 文本里是否有卡拉 OK 标签。
bool has_karaoke(const std::string& raw);

/// 切成逐字/逐词 token：CJK 单字、拉丁词；空格与标点并入前一个 token。
std::vector<std::string> lyric_tokens(const std::string& text);

/// 按源行节奏投影译文；源行没有 \K 时返回 false。
/// target_cs 是译文行应有的总时长（一般与源行相同）。
bool project_translation(const std::string& source_raw, const std::string& target_text,
                         int target_cs, KaraokeResult& out, std::string* error = nullptr);

/// 无源节奏时按 token 均分整行。
KaraokeResult distribute_even(const std::string& text, int total_cs);

/// 无源节奏时按「音节权重」分配整行：英文按元音组估音节、中文按字。
/// 歌词没有词级时间时用这个，比逐词均分更贴演唱节奏。
KaraokeResult distribute_even_weighted(const std::string& text, int total_cs);

/// 词级时间（align 产物）：每个 token 的绝对起止（秒）。
struct TimedToken {
    std::string text;
    double start = 0.0;
    double end = 0.0;
};

/// 用词级时间生成 \K（词与词之间的空隙变成停顿）；失败返回空串。
std::string karaoke_from_timed_tokens(const std::vector<TimedToken>& tokens, double line_start_s,
                                      int total_cs);

} // namespace iris::text
