#pragma once

#include "iris/model/ir_document.hpp"

namespace iris::text {

// ---------------------------------------------------------------------------
// 把 karaoke 打轴应用到整个 IR（批处理与 CLI 共用）。
//   - 先按词级时间（align 产物）；
//   - 再按同句译文投影；
//   - 再按同码兄弟行投影；
//   - 最后 --even 兜底。
// ---------------------------------------------------------------------------

struct KaraokeApplyOptions {
    bool fill_even = false;
    bool even_syllables = false; ///< 兜底均分时按音节加权（英文歌词没有词级时间时必用）
    bool overwrite = false;
    int tolerance_ms = 30;
    std::string only_role; ///< 只处理该角色的句子（如 lyric）；空 = 全部
};

struct KaraokeApplyStats {
    int translated = 0;
    int from_words = 0;
    int projected = 0;
    int even = 0;
    int skipped = 0;
};

void apply_karaoke(model::IrDocument& doc, const KaraokeApplyOptions& options,
                   KaraokeApplyStats& stats);

} // namespace iris::text
