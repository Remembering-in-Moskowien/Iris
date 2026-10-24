#include "iris/controller/commands.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/subtitle/combine.hpp"
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
        if (!fs::exists(path)) {
            std::cerr << _("subtitle: input not found: ", "subtitle: 输入文件不存在：") << path << '\n';
            return false;
        }
    }
    return true;
}

std::string default_out(const std::string& first_input, const std::string& suffix) {
    return first_input + "." + suffix + ".iris.json";
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

    auto doc = make_doc("convert", inputs, language);
    doc.state.tracks.push_back(to_track(cues, language));
    doc.provenance.push_back({"convert", inputs.front(), std::nullopt, std::nullopt});

    const std::string out = out_path.empty() ? default_out(inputs.front(), "iris") : out_path;
    doc.save(out);

    std::cout << _("convert: cues ", "convert: 字幕条数 ") << stats.cues_out
              << _(", duplicates dropped ", "，去重 ") << stats.duplicates
              << _(" → ", " → ") << out << '\n';
    return 0;
}

int run_combine(const std::vector<std::string>& inputs, const std::string& language,
                const std::string& out_path, bool bilingual) {
    if (!check_inputs(inputs)) return 1;
    if (inputs.size() < 2) {
        std::cerr << _("combine: needs at least two subtitle files\n",
                       "combine: 至少需要两个字幕文件\n");
        return 1;
    }

    std::string error;
    subtitle::CombineStats stats;
    std::vector<subtitle::Cue> primary;
    if (!subtitle::parse_file(inputs.front(), primary, error)) {
        std::cerr << "combine: " << error << '\n';
        return 1;
    }

    if (bilingual) {
        if (inputs.size() > 2) {
            std::cerr << _("combine: --bilingual takes exactly two files (first=primary)\n",
                           "combine: --bilingual 只支持两个文件（第一个为主轨）\n");
            return 1;
        }
        std::vector<subtitle::Cue> second;
        if (!subtitle::parse_file(inputs[1], second, error)) {
            std::cerr << "combine: " << error << '\n';
            return 1;
        }
        if (!subtitle::merge_cues(primary, 0, stats, error)) {
            std::cerr << "combine: " << error << '\n';
            return 1;
        }
        subtitle::align_translations(primary, second, 0.1, stats);
    } else {
        // 同语言多段：拼接后去重
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
    }

    auto doc = make_doc("combine", inputs, language);
    doc.state.tracks.push_back(to_track(primary, language));
    for (const auto& input : inputs) {
        doc.provenance.push_back({"combine", input, std::nullopt, std::nullopt});
    }

    const std::string out = out_path.empty() ? default_out(inputs.front(), "combined") : out_path;
    doc.save(out);

    if (bilingual) {
        std::cout << _("combine: cues ", "combine: 字幕条数 ") << stats.cues_out
                  << _(", paired translations ", "，配上译文 ") << stats.paired
                  << _(", unpaired ", "，未配上 ") << stats.unpaired
                  << _(" → ", " → ") << out << '\n';
    } else {
        std::cout << _("combine: cues in ", "combine: 输入条数 ") << stats.cues_in
                  << _(", kept ", "，保留 ") << stats.cues_out
                  << _(", duplicates dropped ", "，去重 ") << stats.duplicates
                  << _(" → ", " → ") << out << '\n';
    }
    return 0;
}

} // namespace iris::controller
