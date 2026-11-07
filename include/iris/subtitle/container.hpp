#pragma once

#include <string>
#include <vector>

namespace iris::subtitle {

// ---------------------------------------------------------------------------
// 容器轨探测与抽取（ffprobe / ffmpeg 子进程，与 vsf / ocr 的做法一致）。
//
// 用途：MKV/MP4 里内封的字幕轨不再是"必须手工 ffmpeg 先抽出来"的前置步骤，
// 而是 iris 的一个入口：`iris tracks` 看清单，`iris convert --track` 直接导入，
// `iris merge` 再把两条轨重分段成一条双语 IR。
// ---------------------------------------------------------------------------

/// 容器里的一条流（只保留选题需要的字段）。
struct MediaTrack {
    int index = -1;              ///< 容器内绝对流索引（ffmpeg -map 0:<index>）
    std::string type;            ///< video | audio | subtitle | attachment
    std::string codec;           ///< subrip / ass / hdmv_pgs_subtitle / h264 ...
    std::string language;        ///< ISO 639-2（chi / eng ...），可能为空
    std::string title;           ///< 轨道标题（"SDH" / "Simplified" ...），可能为空
    bool is_default = false;
    bool forced = false;
};

/// 文本字幕编码（可以直接抽成 SRT/ASS）；图形字幕（PGS/VobSub）返回 false。
bool is_text_subtitle_codec(const std::string& codec);

/// 用 ffprobe 读流清单。失败时 error 里带 ffprobe 的 stderr 摘要。
bool probe_tracks(const std::string& media, std::vector<MediaTrack>& out, std::string& error);

/// 抽取一条字幕轨到 out_base + 自动扩展名（文本轨 .srt，ASS 轨 .ass）。
/// 图形字幕轨会失败并给出明确提示。成功时 out_path 是实际写出的文件。
bool extract_track(const std::string& media, const MediaTrack& track, const std::string& out_base,
                   std::string& out_path, std::string& error);

/// 按选择器挑一条字幕轨：
///   ""            默认字幕轨（优先 disposition.default，否则第一条文本字幕）
///   "3"           容器流索引
///   "lang=chi"    语言代码（eng/chi/zh/en，大小写不敏感）
///   "title=SDH"   标题包含子串
/// 只考虑文本字幕轨；找不到时 error 说明原因。
bool select_track(const std::vector<MediaTrack>& tracks, const std::string& selector,
                  MediaTrack& out, std::string& error);

/// 轨道语言 → iris/IR 语言代码（chi→zh，eng→en；其它原样小写）。
std::string normalize_language(const std::string& track_language);

} // namespace iris::subtitle
