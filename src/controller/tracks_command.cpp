#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "iris/infra/i18n.hpp"
#include "iris/subtitle/container.hpp"
#include "iris/ui/console.hpp"

namespace iris::controller {

namespace {

std::string flags_of(const subtitle::MediaTrack& track) {
    std::string flags;
    if (track.is_default) flags += "default";
    if (track.forced) {
        if (!flags.empty()) flags += ",";
        flags += "forced";
    }
    return flags;
}

} // namespace

int run_tracks(const std::string& media) {
    std::vector<subtitle::MediaTrack> tracks;
    std::string error;
    if (!subtitle::probe_tracks(media, tracks, error)) {
        std::cerr << "tracks: " << error << '\n';
        return 1;
    }

    std::vector<std::vector<std::string>> rows;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& t : tracks) {
        const bool text = t.type == "subtitle" && subtitle::is_text_subtitle_codec(t.codec);
        rows.push_back({std::to_string(t.index), t.type, t.codec, t.language, t.title,
                        flags_of(t), t.type == "subtitle" ? (text ? "text" : "image") : ""});
        list.push_back({{"index", t.index},
                        {"type", t.type},
                        {"codec", t.codec},
                        {"language", t.language},
                        {"title", t.title},
                        {"default", t.is_default},
                        {"forced", t.forced},
                        {"textSubtitle", text}});
    }

    ui::table({"#", "type", "codec", "lang", "title", "flags", "subs"}, rows);
    ui::note(_("convert --track <index|lang=xx|title=xx> imports one of these subtitle tracks",
               "convert --track <索引|lang=xx|title=xx> 可以直接导入其中一条字幕轨"));

    ui::json_out(nlohmann::json({{"ok", true},
                                 {"command", "tracks"},
                                 {"media", media},
                                 {"tracks", list}})
                     .dump(2));
    return 0;
}

} // namespace iris::controller
