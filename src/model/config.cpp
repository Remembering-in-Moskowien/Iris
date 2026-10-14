#include "daisy/model/config.hpp"

#include <fstream>

namespace daisy::model {

nlohmann::json AppConfig::to_json() const {
    return {{"downloadProxy", download_proxy},
            {"toolsDir", tools_dir},
            {"modelsDir", models_dir},
            {"logLevel", log_level}};
}

AppConfig AppConfig::from_json(const nlohmann::json& j) {
    AppConfig c;
    c.download_proxy = j.value("downloadProxy", std::string());
    c.tools_dir = j.value("toolsDir", std::string("tools"));
    c.models_dir = j.value("modelsDir", std::string("models"));
    c.log_level = j.value("logLevel", std::string("info"));
    return c;
}

AppConfig AppConfig::load_or_default(const std::string& path) {
    try {
        std::ifstream in(path);
        if (!in) return AppConfig{};
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded()) return AppConfig{};
        return from_json(j);
    } catch (...) {
        return AppConfig{};
    }
}

void AppConfig::save(const std::string& path) const {
    std::ofstream out(path);
    out << to_json().dump(2) << '\n';
}

} // namespace daisy::model
