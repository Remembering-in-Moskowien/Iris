#include "iris/controller/commands.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/paths.hpp"
#include "iris/subtitle/ass_script.hpp"
#include <nlohmann/json.hpp>

namespace iris::controller {

namespace fs = std::filesystem;

int run_style_extract(const std::string& ass_path, const std::string& out_json,
                      const std::string& name) {
    subtitle::AssScript script;
    std::string error;
    if (!subtitle::load_ass_script(ass_path, script, error)) {
        std::cerr << "style: " << error << '\n';
        return 1;
    }
    if (script.styles.empty()) {
        std::cerr << _("style: no styles in ", "style: 文件里没有样式：") << ass_path << '\n';
        return 1;
    }

    nlohmann::json info = nlohmann::json::array();
    for (const auto& [k, v] : script.info) info.push_back({k, v});

    nlohmann::json styles = nlohmann::json::array();
    for (const auto& s : script.styles) styles.push_back(s.to_json());

    nlohmann::json j;
    j["name"] = name.empty()
                    ? script.info_value(
                          "Title", infra::path_to_utf8(infra::utf8_path(ass_path).stem()))
                    : name;
    j["scriptInfo"] = info;
    j["styles"] = styles;

    const std::string out = out_json.empty() ? ass_path + ".theme.json" : out_json;
    std::ofstream f(infra::utf8_path(out), std::ios::binary);
    if (!f) {
        std::cerr << _("style: cannot write ", "style: 无法写入 ") << out << '\n';
        return 1;
    }
    f << j.dump(2) << '\n';

    std::cout << _("style: extracted ", "style: 已提取 ") << script.styles.size()
              << _(" styles, PlayRes ", " 个样式，画布 ") << script.play_res_x() << 'x'
              << script.play_res_y() << _(" → ", " → ") << out << '\n';
    return 0;
}

} // namespace iris::controller
