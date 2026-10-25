#include "iris/controller/commands.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#include "iris/asr/align.hpp"
#include "iris/infra/i18n.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"

namespace iris::controller {

namespace fs = std::filesystem;

int run_align(const std::string& media, const std::string& ir_path, const std::string& out_path,
              const asr::AlignOptions& options) {
    if (!fs::exists(infra::utf8_path(ir_path))) {
        std::cerr << _("align: IR not found: ", "align: IR 文件不存在：") << ir_path << '\n';
        return 1;
    }

    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        std::cerr << "align: " << e.what() << '\n';
        return 1;
    }

    model::Track* track = nullptr;
    for (auto& t : doc.state.tracks) {
        if (t.type == "transcribe") {
            track = &t;
            break;
        }
    }
    if (track == nullptr && !doc.state.tracks.empty()) track = &doc.state.tracks.front();
    if (track == nullptr) {
        std::cerr << _("align: IR has no track\n", "align: IR 里没有可用轨道\n");
        return 1;
    }

    std::vector<float> pcm;
    std::string error;
    if (!asr::decode_media_16k(media, pcm, error)) {
        std::cerr << "align: " << error << '\n';
        return 1;
    }

    asr::AlignStats stats;
    if (!asr::align_track(*track, pcm, 16000, options, stats, error)) {
        std::cerr << "align: " << error << '\n';
        return 1;
    }

    std::cout << _("align: audio ", "align: 音频 ") << stats.audio_seconds
              << _("s, sentences ", " 秒，句子 ") << stats.sentences
              << _(", aligned ", "，对齐 ") << stats.aligned << _(", tokens ", "，词/字 ")
              << stats.tokens << _(", failed ", "，失败 ") << stats.failed << '\n';

    if (options.dry_run) {
        std::cout << _("align: dry run, nothing written\n", "align: 预演模式，未写入文件\n");
        return 0;
    }

    const std::string target =
        out_path.empty() ? ir_path + ".aligned.iris.json" : out_path;
    try {
        doc.save(target);
    } catch (const std::exception& e) {
        std::cerr << "align: " << e.what() << '\n';
        return 1;
    }
    std::cout << _("align: word timings written → ", "align: 词级时间已写入 → ") << target << '\n';
    return stats.failed == 0 ? 0 : 2;
}

} // namespace iris::controller
