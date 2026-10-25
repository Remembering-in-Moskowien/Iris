#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"
#include "iris/text/karaoke_apply.hpp"

namespace iris::controller {

int run_karaoke(const std::string& ir_path, const std::string& out_path, bool fill_even,
                bool overwrite, int tolerance_ms) {
    model::IrDocument doc;
    try {
        doc = model::IrDocument::load(ir_path);
    } catch (const std::exception& e) {
        std::cerr << "karaoke: " << e.what() << '\n';
        return 1;
    }

    text::KaraokeApplyOptions options;
    options.fill_even = fill_even;
    options.overwrite = overwrite;
    options.tolerance_ms = tolerance_ms;
    text::KaraokeApplyStats stats;
    text::apply_karaoke(doc, options, stats);

    const std::string out = out_path.empty() ? ir_path + ".karaoke.iris.json" : out_path;
    try {
        doc.save(out);
    } catch (const std::exception& e) {
        std::cerr << "karaoke: " << e.what() << '\n';
        return 1;
    }

    std::cout << _("karaoke: translated ", "karaoke: 译文打轴 ") << stats.translated
              << _(", from word timings ", "，按词级时间 ") << stats.from_words
              << _(", projected from sibling ", "，同码投影 ") << stats.projected
              << _(", even-split ", "，均分 ") << stats.even << _(", skipped ", "，跳过 ")
              << stats.skipped << _(" → ", " → ") << out << '\n';
    return 0;
}

} // namespace iris::controller
