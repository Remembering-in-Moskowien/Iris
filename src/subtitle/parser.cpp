#include "iris/subtitle/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace iris::subtitle {

namespace {

/// 读整个文件（保留 UTF-8；去掉 BOM）。
bool read_all(const std::string& path, std::string& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开字幕文件：" + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : content) {
        if (c == '\r') continue;
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

/// `hh:mm:ss,mmm` 或 `hh:mm:ss.mmm` → 毫秒；失败返回 -1。
std::int64_t parse_hms(const std::string& s, char ms_sep) {
    int h = 0, m = 0, sec = 0, ms = 0;
    // 允许小时被省略（VTT）
    if (std::sscanf(s.c_str(), "%d:%d:%d%c%d", &h, &m, &sec, &ms_sep, &ms) == 5) {
        return (static_cast<std::int64_t>(h) * 3600 + m * 60 + sec) * 1000 + ms;
    }
    if (std::sscanf(s.c_str(), "%d:%d%c%d", &m, &sec, &ms_sep, &ms) == 4) {
        return (static_cast<std::int64_t>(m) * 60 + sec) * 1000 + ms;
    }
    return -1;
}

} // namespace

std::int64_t parse_ass_time(const std::string& text) {
    // h:mm:ss.cc（百分秒）
    int h = 0, m = 0, s = 0, cs = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d.%d", &h, &m, &s, &cs) != 4) return -1;
    return (static_cast<std::int64_t>(h) * 3600 + m * 60 + s) * 1000 + cs * 10;
}

std::string clean_ass_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_tag = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') {
            in_tag = true;
            continue;
        }
        if (c == '}') {
            in_tag = false;
            continue;
        }
        if (in_tag) continue;
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'N' || text[i + 1] == 'n')) {
            out.push_back(' ');
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == 'h') { // 硬空格
            out.push_back(' ');
            ++i;
            continue;
        }
        out.push_back(c);
    }
    return trim(out);
}

bool parse_srt(const std::string& content, std::vector<Cue>& out, std::string& error) {
    const auto lines = split_lines(content);
    for (std::size_t i = 0; i < lines.size();) {
        // 跳过序号/空行，找时间码行
        if (lines[i].find("-->") == std::string::npos) {
            ++i;
            continue;
        }
        const auto arrow = lines[i].find("-->");
        const std::string start_s = trim(lines[i].substr(0, arrow));
        // 先去掉 "-->" 后的前导空白，再取到第一个空白前（后面可能跟位置信息）
        std::string rest = trim(lines[i].substr(arrow + 3));
        const auto space = rest.find_first_of(" \t");
        const std::string end_s = trim(space == std::string::npos ? rest : rest.substr(0, space));

        Cue cue;
        cue.start_ms = parse_hms(start_s, ',');
        cue.end_ms = parse_hms(end_s, ',');
        if (cue.start_ms < 0 || cue.end_ms < 0) {
            error = "SRT 时间码无法解析：" + lines[i];
            return false;
        }

        // 后续非空行为文本，直到空行或下一条时间码
        ++i;
        std::string text;
        while (i < lines.size() && !trim(lines[i]).empty() &&
               lines[i].find("-->") == std::string::npos) {
            if (!text.empty()) text.push_back(' ');
            text += trim(lines[i]);
            ++i;
        }
        cue.text = trim(text);
        if (!cue.text.empty()) out.push_back(std::move(cue));
    }
    if (out.empty()) {
        error = "SRT 里没有解析到任何字幕";
        return false;
    }
    return true;
}

bool parse_ass(const std::string& content, std::vector<Cue>& out, std::string& error) {
    const auto lines = split_lines(content);
    bool in_events = false;
    for (const auto& raw : lines) {
        const std::string line = trim(raw);
        if (line.empty()) continue;
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            in_events = (line == "[Events]");
            continue;
        }
        if (!in_events || line.rfind("Dialogue:", 0) != 0) continue;

        // Dialogue: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text
        std::vector<std::string> fields;
        std::string cur;
        int field = 0;
        for (std::size_t i = 9; i < line.size(); ++i) { // 跳过 "Dialogue:"
            const char c = line[i];
            if (c == ',' && field < 9) {
                fields.push_back(cur);
                cur.clear();
                ++field;
            } else {
                cur.push_back(c);
            }
        }
        fields.push_back(cur);
        if (fields.size() < 10) continue;

        Cue cue;
        cue.start_ms = parse_ass_time(trim(fields[1]));
        cue.end_ms = parse_ass_time(trim(fields[2]));
        if (cue.start_ms < 0 || cue.end_ms < 0) continue;
        cue.text = clean_ass_text(fields[9]);
        if (!cue.text.empty()) out.push_back(std::move(cue));
    }
    if (out.empty()) {
        error = "ASS 里没有解析到任何 Dialogue 行";
        return false;
    }
    return true;
}

bool parse_vtt(const std::string& content, std::vector<Cue>& out, std::string& error) {
    const auto lines = split_lines(content);
    for (std::size_t i = 0; i < lines.size();) {
        if (lines[i].find("-->") == std::string::npos) {
            ++i;
            continue;
        }
        const auto arrow = lines[i].find("-->");
        const std::string start_s = trim(lines[i].substr(0, arrow));
        std::string rest = trim(lines[i].substr(arrow + 3));
        const auto space = rest.find_first_of(" \t");
        const std::string end_s = trim(space == std::string::npos ? rest : rest.substr(0, space));

        Cue cue;
        cue.start_ms = parse_hms(start_s, '.');
        cue.end_ms = parse_hms(end_s, '.');
        if (cue.start_ms < 0 || cue.end_ms < 0) {
            error = "VTT 时间码无法解析：" + lines[i];
            return false;
        }
        ++i;
        std::string text;
        while (i < lines.size() && !trim(lines[i]).empty() &&
               lines[i].find("-->") == std::string::npos) {
            if (!text.empty()) text.push_back(' ');
            text += trim(lines[i]);
            ++i;
        }
        cue.text = trim(text);
        if (!cue.text.empty()) out.push_back(std::move(cue));
    }
    if (out.empty()) {
        error = "VTT 里没有解析到任何字幕";
        return false;
    }
    return true;
}

bool parse_file(const std::string& path, std::vector<Cue>& out, std::string& error) {
    out.clear();
    std::string content;
    if (!read_all(path, content, error)) return false;

    const std::string ext = std::filesystem::path(path).extension().string();
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == ".srt") return parse_srt(content, out, error);
    if (lower == ".ass" || lower == ".ssa") return parse_ass(content, out, error);
    if (lower == ".vtt") return parse_vtt(content, out, error);
    error = "不支持的字幕格式：" + ext + "（支持 .srt / .ass / .ssa / .vtt）";
    return false;
}

} // namespace iris::subtitle
