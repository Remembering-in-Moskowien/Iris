#include "iris/subtitle/container.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "iris/infra/paths.hpp"
#include "iris/infra/process.hpp"

namespace iris::subtitle {

namespace fs = std::filesystem;

namespace {

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// ffprobe 的语言代码是 ISO 639-2（chi/eng），项目内部常用 ISO 639-1（zh/en）。
std::string tag(const nlohmann::json& stream, const char* key) {
    if (!stream.contains("tags") || !stream["tags"].is_object()) return {};
    const auto& tags = stream["tags"];
    if (!tags.contains(key)) return {};
    const auto& v = tags[key];
    return v.is_string() ? v.get<std::string>() : std::string();
}

bool disposition(const nlohmann::json& stream, const char* key) {
    if (!stream.contains("disposition") || !stream["disposition"].is_object()) return false;
    const auto& d = stream["disposition"];
    if (!d.contains(key)) return false;
    return d[key].is_number() && d[key].get<int>() != 0;
}

} // namespace

bool is_text_subtitle_codec(const std::string& codec) {
    static const char* kText[] = {"subrip", "srt", "ass", "ssa", "webvtt", "mov_text",
                                  "text",   "microdvd", "subviewer", "jacosub"};
    const std::string c = lower_ascii(codec);
    for (const char* t : kText) {
        if (c == t) return true;
    }
    return false;
}

bool probe_tracks(const std::string& media, std::vector<MediaTrack>& out, std::string& error) {
    out.clear();
    if (!fs::exists(infra::utf8_path(media))) {
        error = "媒体文件不存在：" + media;
        return false;
    }

    auto r = infra::run_process_split({"ffprobe", "-v", "error", "-show_streams", "-of", "json",
                                       media});
    if (!r.ok()) {
        error = "ffprobe 启动失败（ffmpeg/ffprobe 是否在 PATH？）：" + r.error();
        return false;
    }
    if (!r.value().success()) {
        std::string detail = r.value().stderr_text;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
        error = "ffprobe 读取失败：" + (detail.empty() ? std::string("退出码非 0") : detail);
        return false;
    }

    auto j = nlohmann::json::parse(r.value().stdout_text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("streams") || !j["streams"].is_array()) {
        error = "ffprobe 输出不是预期的 JSON";
        return false;
    }

    for (const auto& s : j["streams"]) {
        if (!s.is_object()) continue;
        MediaTrack t;
        t.index = s.value("index", -1);
        t.type = s.value("codec_type", std::string());
        t.codec = s.value("codec_name", std::string());
        t.language = tag(s, "language");
        t.title = tag(s, "title");
        t.is_default = disposition(s, "default");
        t.forced = disposition(s, "forced");
        out.push_back(std::move(t));
    }
    if (out.empty()) {
        error = "容器里没有任何流：" + media;
        return false;
    }
    return true;
}

bool extract_track(const std::string& media, const MediaTrack& track, const std::string& out_base,
                   std::string& out_path, std::string& error) {
    if (track.type != "subtitle") {
        error = "流 " + std::to_string(track.index) + " 不是字幕轨（" + track.type + "）";
        return false;
    }
    if (!is_text_subtitle_codec(track.codec)) {
        error = "流 " + std::to_string(track.index) + " 是图形字幕（" + track.codec +
                "），需要 OCR，暂不支持直接抽取";
        return false;
    }

    const bool ass = track.codec == "ass" || track.codec == "ssa";
    out_path = out_base + (ass ? ".ass" : ".srt");

    std::vector<std::string> args{"ffmpeg", "-v", "error", "-y", "-i", media,
                                  "-map", "0:" + std::to_string(track.index)};
    if (ass) {
        args.insert(args.end(), {"-c:s", "copy"});
    } else {
        args.insert(args.end(), {"-c:s", "srt"});
    }
    args.push_back(out_path);

    auto r = infra::run_process_split(args);
    if (!r.ok()) {
        error = "ffmpeg 启动失败：" + r.error();
        return false;
    }
    if (!r.value().success()) {
        std::string detail = r.value().stderr_text;
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
        error = "抽取字幕轨失败：" + (detail.empty() ? std::string("退出码非 0") : detail);
        return false;
    }
    if (!fs::exists(infra::utf8_path(out_path))) {
        error = "抽取后没有生成文件：" + out_path;
        return false;
    }
    return true;
}

bool select_track(const std::vector<MediaTrack>& tracks, const std::string& selector,
                  MediaTrack& out, std::string& error) {
    std::vector<const MediaTrack*> subs;
    for (const auto& t : tracks) {
        if (t.type == "subtitle") subs.push_back(&t);
    }
    if (subs.empty()) {
        error = "容器里没有字幕轨";
        return false;
    }

    std::string sel = selector;
    while (!sel.empty() && std::isspace(static_cast<unsigned char>(sel.front()))) sel.erase(sel.begin());
    while (!sel.empty() && std::isspace(static_cast<unsigned char>(sel.back()))) sel.pop_back();

    if (sel.empty()) {
        for (const auto* t : subs) {
            if (t->is_default && is_text_subtitle_codec(t->codec)) {
                out = *t;
                return true;
            }
        }
        for (const auto* t : subs) {
            if (is_text_subtitle_codec(t->codec)) {
                out = *t;
                return true;
            }
        }
        error = "没有可用的文本字幕轨（图形字幕需要 OCR）";
        return false;
    }

    const auto eq = sel.find('=');
    if (eq == std::string::npos) {
        // 纯数字 = 容器流索引
        const bool digits = std::all_of(sel.begin(), sel.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
        if (digits) {
            const int index = std::atoi(sel.c_str());
            for (const auto* t : subs) {
                if (t->index == index) {
                    if (!is_text_subtitle_codec(t->codec)) {
                        error = "流 " + sel + " 是图形字幕（" + t->codec + "），需要 OCR";
                        return false;
                    }
                    out = *t;
                    return true;
                }
            }
            error = "容器里没有索引为 " + sel + " 的字幕轨";
            return false;
        }
        error = "无法识别的轨选择器：" + sel + "（用 3 / lang=chi / title=SDH）";
        return false;
    }

    const std::string key = lower_ascii(sel.substr(0, eq));
    const std::string want = lower_ascii(sel.substr(eq + 1));

    for (const auto* t : subs) {
        const std::string value = key == "lang" ? lower_ascii(t->language) : lower_ascii(t->title);
        const bool hit = key == "lang" ? (value == want || normalize_language(value) == want)
                                       : value.find(want) != std::string::npos;
        if (!hit) continue;
        if (!is_text_subtitle_codec(t->codec)) {
            error = "命中的字幕轨是图形字幕（" + t->codec + "），需要 OCR";
            return false;
        }
        out = *t;
        return true;
    }
    error = "没有匹配 " + sel + " 的字幕轨";
    return false;
}

std::string normalize_language(const std::string& track_language) {
    const std::string s = lower_ascii(track_language);
    if (s == "chi" || s == "zho" || s == "chs" || s == "cht" || s == "cmn" || s == "zh") return "zh";
    if (s == "eng" || s == "en") return "en";
    if (s == "jpn" || s == "ja") return "ja";
    if (s == "kor" || s == "ko") return "ko";
    return s;
}

} // namespace iris::subtitle
