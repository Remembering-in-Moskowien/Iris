#include "iris/model/subtitle.hpp"

#include <cstdio>
#include <sstream>

namespace iris::model {

// ASS 色彩 &HAABBGGRR（8 位：alpha + BGR，符合 v4.00+ 规范）。
static std::string ass_colour(std::uint32_t argb) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "&H%08X", static_cast<unsigned int>(argb));
    return buf;
}

std::string AssStyle::to_ass() const {
    // Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour,
    //         BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing,
    //         Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
    std::ostringstream os;
    os << "Style: " << name << ',' << font_name << ',' << font_size << ','
       << ass_colour(primary_colour) << ',' << ass_colour(0x00FFFFFF) << ','
       << ass_colour(outline_colour) << ',' << ass_colour(0x00000000) << ','
       << (bold ? -1 : 0) << ",0,0,0,100,100,0,0,1," << outline << ',' << shadow
       << ",2," << margin_l << ',' << margin_r << ',' << margin_v << ",1";
    return os.str();
}

// ASS 花括号是控制代码；普通文本中的 { } 需转义。
static std::string ass_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '{') {
            out += "\\{";
        } else if (c == '}') {
            out += "\\}";
        } else {
            out += c;
        }
    }
    return out;
}

std::string SubtitleDocument::render_ass() const {
    std::ostringstream os;
    os << "[Script Info]\n";
    os << "Title: " << title << '\n';
    os << "ScriptType: v4.00+\n";
    os << "Collisions: Normal\n";
    os << "PlayResX: 384\nPlayResY: 288\n";
    os << "\n[V4+ Styles]\n";
    os << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
          "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
          "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
          "MarginR, MarginV, Encoding\n";
    for (const auto& st : styles) {
        os << st.to_ass() << '\n';
    }
    os << "\n[Events]\n";
    os << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    for (const auto& ln : lines) {
        os << "Dialogue: 0," << ln.start.to_ass() << ',' << ln.end.to_ass() << ','
           << ln.style << ",,0,0,0,," << ass_escape(ln.text) << '\n';
    }
    return os.str();
}

std::string SubtitleDocument::render_srt() const {
    std::ostringstream os;
    int idx = 1;
    for (const auto& ln : lines) {
        os << idx++ << '\n';
        os << ln.start.to_srt() << " --> " << ln.end.to_srt() << '\n';
        os << ln.text << "\n\n";
    }
    return os.str();
}

} // namespace iris::model
