#include "iris/controller/commands.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "iris/infra/i18n.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/subtitle/ass_script.hpp"
#include "iris/subtitle/combine.hpp"
#include "iris/subtitle/ir_bridge.hpp"
#include "iris/subtitle/parser.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

namespace {

/// 把解析出的 cue 转成 IR 轨道。
model::Track to_track(const std::vector<subtitle::Cue>& cues, const std::string& language) {
    model::Track track;
    track.type = "transcribe";
    track.language = language;
    track.sentences.reserve(cues.size());
    for (const auto& cue : cues) {
        model::Sentence s;
        s.start_ms = cue.start_ms;
        s.end_ms = cue.end_ms;
        s.text = cue.text;
        if (!cue.translation.empty()) s.translated_text = cue.translation;
        track.sentences.push_back(std::move(s));
    }
    return track;
}

model::IrDocument make_doc(const std::string& command, const std::vector<std::string>& inputs,
                           const std::string& language) {
    model::IrDocument doc;
    doc.generator.version = kVersion;
    doc.generator.command = command;
    doc.generator.generated_at = "now";
    doc.generator.input_file = inputs.front();
    doc.config.input_file_path = inputs.front();
    doc.config.command_name = command;
    doc.config.language = language;
    doc.generator.tool = "iris";
    return doc;
}

bool check_inputs(const std::vector<std::string>& inputs) {
    if (inputs.empty()) {
        std::cerr << _("subtitle: no input file\n", "subtitle: 没有输入文件\n");
        return false;
    }
    for (const auto& path : inputs) {
        if (!fs::exists(infra::utf8_path(path))) {
            std::cerr << _("subtitle: input not found: ", "subtitle: 输入文件不存在：") << path << '\n';
            return false;
        }
    }
    return true;
}

std::string default_out(const std::string& first_input, const std::string& suffix) {
    return first_input + "." + suffix + ".iris.json";
}

bool is_ass_path(const std::string& path) {
    std::string ext = infra::path_to_utf8(infra::utf8_path(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".ass" || ext == ".ssa";
}

std::vector<subtitle::Cue> cues_from_script(const subtitle::AssScript& script) {
    std::vector<subtitle::Cue> cues;
    cues.reserve(script.events.size());
    for (const auto& ev : script.events) {
        if (ev.comment) continue;
        subtitle::Cue cue;
        cue.start_ms = ev.start_ms;
        cue.end_ms = ev.end_ms;
        cue.text = subtitle::strip_ass_text(ev.text);
        if (!cue.text.empty()) cues.push_back(std::move(cue));
    }
    return cues;
}

/// 按时间重叠把 second 的文本配给 primary 的句子（双语合并）。
int align_translations(std::vector<model::Sentence>& primary,
                       const std::vector<subtitle::Cue>& second) {
    int paired = 0;
    for (auto& s : primary) {
        if (s.comment || s.text.empty()) continue;
        const std::int64_t dur = std::max<std::int64_t>(s.end_ms - s.start_ms, 1);
        std::int64_t best_overlap = 0;
        const subtitle::Cue* best = nullptr;
        for (const auto& other : second) {
            if (other.text.empty()) continue;
            const auto lo = std::max(s.start_ms, other.start_ms);
            const auto hi = std::min(s.end_ms, other.end_ms);
            const auto overlap = hi - lo;
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best = &other;
            }
        }
        if (best != nullptr && static_cast<double>(best_overlap) / static_cast<double>(dur) >= 0.1) {
            s.translated_text = best->text;
            ++paired;
        }
    }
    return paired;
}

/// 译文行（可带时间码：从 ASS 片段里读出来）。
struct TranslationLine {
    std::string text;
    std::int64_t start_ms = -1;
    std::int64_t end_ms = -1;
    bool timed() const { return start_ms >= 0 && end_ms > start_ms; }
};

/// 读取「纯文本译文」：一行一条；若文件本身是 ASS 片段，则取 Dialogue 的文本列与时间码。
bool load_translation_lines(const std::string& path, std::vector<TranslationLine>& out,
                            std::string& error) {
    std::ifstream f(infra::utf8_path(path), std::ios::binary);
    if (!f) {
        error = "无法打开译文文件：" + path;
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t b = 0, e = line.size();
        while (b < e && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
        const std::string trimmed = line.substr(b, e - b);
        if (trimmed.empty()) continue;
        if (trimmed.front() == '[' || trimmed.front() == ';') continue;
        if (trimmed.rfind("Format:", 0) == 0 || trimmed.rfind("Style:", 0) == 0 ||
            trimmed.rfind("Title:", 0) == 0 || trimmed.rfind("ScriptType:", 0) == 0 ||
            trimmed.rfind("WrapStyle:", 0) == 0 || trimmed.rfind("PlayRes", 0) == 0 ||
            trimmed.rfind("ScaledBorder", 0) == 0)
            continue;
        if (trimmed.rfind("Dialogue:", 0) == 0) {
            const auto body = trimmed.substr(9);
            std::vector<std::string> fields;
            std::string cur;
            int field = 0;
            for (char c : body) {
                if (c == ',' && field < 9) {
                    fields.push_back(cur);
                    cur.clear();
                    ++field;
                } else {
                    cur.push_back(c);
                }
            }
            fields.push_back(cur);
            if (fields.size() == 10) {
                TranslationLine tl;
                tl.text = fields[9];
                tl.start_ms = subtitle::parse_ass_time(fields[1]);
                tl.end_ms = subtitle::parse_ass_time(fields[2]);
                out.push_back(std::move(tl));
                continue;
            }
        }
        TranslationLine tl;
        tl.text = trimmed;
        out.push_back(std::move(tl));
    }
    if (out.empty()) {
        error = "译文文件里没有有效行：" + path;
        return false;
    }
    return true;
}

} // namespace

int run_convert(const std::vector<std::string>& inputs, const std::string& language,
                const std::string& out_path) {
    if (!check_inputs(inputs)) return 1;
    if (inputs.size() != 1) {
        std::cerr << _("convert: expects exactly one subtitle file (use `combine` for several)\n",
                       "convert: 只接受一个字幕文件（多个请用 combine）\n");
        return 1;
    }

    auto doc = make_doc("convert", inputs, language);
    std::size_t cue_count = 0;

    if (is_ass_path(inputs.front())) {
        subtitle::AssScript script;
        std::string error;
        if (!subtitle::load_ass_script(inputs.front(), script, error)) {
            std::cerr << "convert: " << error << '\n';
            return 1;
        }
        doc.state.tracks.push_back(subtitle::track_from_script(script, language));
        subtitle::apply_presentation(script, doc.state);
        cue_count = script.events.size();
    } else {
        std::vector<subtitle::Cue> cues;
        std::string error;
        if (!subtitle::parse_file(inputs.front(), cues, error)) {
            std::cerr << "convert: " << error << '\n';
            return 1;
        }
        subtitle::CombineStats stats;
        if (!subtitle::merge_cues(cues, 0, stats, error)) {
            std::cerr << "convert: " << error << '\n';
            return 1;
        }
        cue_count = cues.size();
        doc.state.tracks.push_back(to_track(cues, language));
    }

    doc.provenance.push_back({"convert", inputs.front(), std::nullopt, std::nullopt});

    const std::string out = out_path.empty() ? default_out(inputs.front(), "iris") : out_path;
    doc.save(out);

    std::cout << _("convert: cues ", "convert: 字幕条数 ") << cue_count
              << _(", styles ", "，样式 ") << doc.state.styles.size() << _(" → ", " → ") << out
              << '\n';
    return 0;
}

int run_combine(const std::vector<std::string>& inputs, const std::string& language,
                const std::string& out_path, bool bilingual, bool text_mode) {
    if (!check_inputs(inputs)) return 1;
    if (inputs.size() < 2) {
        std::cerr << _("combine: needs at least two subtitle files\n",
                       "combine: 至少需要两个字幕文件\n");
        return 1;
    }

    std::string error;
    subtitle::CombineStats stats;
    auto doc = make_doc("combine", inputs, language);

    if (!bilingual) {
        std::vector<subtitle::Cue> primary;
        if (!subtitle::parse_file(inputs.front(), primary, error)) {
            std::cerr << "combine: " << error << '\n';
            return 1;
        }
        for (std::size_t i = 1; i < inputs.size(); ++i) {
            std::vector<subtitle::Cue> more;
            if (!subtitle::parse_file(inputs[i], more, error)) {
                std::cerr << "combine: " << error << '\n';
                return 1;
            }
            primary.insert(primary.end(), more.begin(), more.end());
        }
        if (!subtitle::merge_cues(primary, 500, stats, error)) {
            std::cerr << "combine: " << error << '\n';
            return 1;
        }
        doc.state.tracks.push_back(to_track(primary, language));
    } else {
        if (inputs.size() > 2) {
            std::cerr << _("combine: --bilingual takes exactly two files (first=primary)\n",
                           "combine: --bilingual 只支持两个文件（第一个为主轨）\n");
            return 1;
        }

        // 主轨：ASS 走保真路径（样式 + 原始标签），其它格式走 cue 路径。
        model::Track track;
        subtitle::AssScript primary_script;
        const bool primary_is_ass = is_ass_path(inputs.front());
        if (primary_is_ass) {
            if (!subtitle::load_ass_script(inputs.front(), primary_script, error)) {
                std::cerr << "combine: " << error << '\n';
                return 1;
            }
            track = subtitle::track_from_script(primary_script, language);
        } else {
            std::vector<subtitle::Cue> primary;
            if (!subtitle::parse_file(inputs.front(), primary, error)) {
                std::cerr << "combine: " << error << '\n';
                return 1;
            }
            track = to_track(primary, language);
        }

        // 译文轨：文本译文按行序配对；字幕文件按时间重叠配对。
        if (text_mode) {
            std::vector<TranslationLine> lines;
            if (!load_translation_lines(inputs[1], lines, error)) {
                std::cerr << "combine: " << error << '\n';
                return 1;
            }
            int index = 0;
            int paired = 0;
            for (auto& s : track.sentences) {
                if (s.comment || s.text.empty()) continue;
                if (index >= static_cast<int>(lines.size())) break;
                s.translated_text = lines[static_cast<std::size_t>(index++)].text;
                ++paired;
            }
            // 多出来的译文行如果自带时间码（ASS 片段），作为独立的译文事件补进来。
            int appended = 0;
            for (; index < static_cast<int>(lines.size()); ++index) {
                const auto& tl = lines[static_cast<std::size_t>(index)];
                if (!tl.timed()) continue;
                model::Sentence s;
                s.start_ms = tl.start_ms;
                s.end_ms = tl.end_ms;
                s.text = tl.text;
                s.style = "chi";
                track.sentences.push_back(std::move(s));
                ++appended;
            }
            stats.paired = paired;
            stats.cues_in = static_cast<int>(track.sentences.size());
            stats.unpaired = static_cast<int>(lines.size()) - paired - appended;
            stats.cues_out = static_cast<int>(track.sentences.size());
            if (appended > 0)
                std::cout << _("combine: appended ", "combine: 追加带时码译文 ") << appended
                          << _(" timed translation line(s)\n", " 条\n");
            if (stats.unpaired > 0)
                std::cerr << _("combine: ", "combine: ") << stats.unpaired
                          << _(" translation line(s) left over (timeline has ", " 行译文没有时码也没有位置（时间轴只有 ")
                          << paired << _(" sentences)", " 句）") << '\n';
            if (primary_is_ass) subtitle::apply_presentation(primary_script, doc.state);
        } else {
            std::vector<subtitle::Cue> second;
            subtitle::AssScript second_script;
            const bool second_is_ass = is_ass_path(inputs[1]);
            if (second_is_ass) {
                if (!subtitle::load_ass_script(inputs[1], second_script, error)) {
                    std::cerr << "combine: " << error << '\n';
                    return 1;
                }
                second = cues_from_script(second_script);
            } else if (!subtitle::parse_file(inputs[1], second, error)) {
                std::cerr << "combine: " << error << '\n';
                return 1;
            }

            stats.paired = align_translations(track.sentences, second);
            stats.cues_in = static_cast<int>(track.sentences.size());
            stats.unpaired = stats.cues_in - stats.paired;
            stats.cues_out = stats.cues_in;

            if (second_is_ass) subtitle::merge_styles(primary_script, second_script, doc.state);
            else if (primary_is_ass) subtitle::apply_presentation(primary_script, doc.state);
        }
        doc.state.tracks.push_back(std::move(track));
    }

    for (const auto& input : inputs) {
        doc.provenance.push_back({"combine", input, std::nullopt, std::nullopt});
    }

    const std::string out = out_path.empty() ? default_out(inputs.front(), "combined") : out_path;
    doc.save(out);

    if (bilingual) {
        if (text_mode) {
            std::cout << _("combine: cues ", "combine: 字幕条数 ") << stats.cues_out
                      << _(", paired by order ", "，按行序配对 ") << stats.paired
                      << _(", leftover lines ", "，多出译文行 ") << stats.unpaired
                      << _(" → ", " → ") << out << '\n';
        } else {
            std::cout << _("combine: cues ", "combine: 字幕条数 ") << stats.cues_out
                      << _(", paired translations ", "，配上译文 ") << stats.paired
                      << _(", unpaired ", "，未配上 ") << stats.unpaired
                      << _(" → ", " → ") << out << '\n';
        }
    } else {
        std::cout << _("combine: cues in ", "combine: 输入条数 ") << stats.cues_in
                  << _(", kept ", "，保留 ") << stats.cues_out
                  << _(", duplicates dropped ", "，去重 ") << stats.duplicates
                  << _(" → ", " → ") << out << '\n';
    }
    return 0;
}

} // namespace iris::controller
