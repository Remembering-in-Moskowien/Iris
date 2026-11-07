#include "iris/controller/commands.hpp"

#include <filesystem>
#include <iostream>

#include "iris/infra/paths.hpp"
#include "iris/model/ir_document.hpp"

namespace iris::controller {

int run_compact(const std::string& ir_path, const std::string& out_path) {
    if (!std::filesystem::exists(infra::utf8_path(ir_path))) {
        std::cerr << "compact: IR 文件不存在：" << ir_path << '\n';
        return 1;
    }
    try {
        const auto doc = model::IrDocument::load(ir_path);
        const std::string target =
            out_path.empty() ? ir_path + ".compact.iris.json" : out_path;
        doc.save(target, -1);
        std::cout << "compact: 已生成紧凑 IR → " << target << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "compact: " << e.what() << '\n';
        return 1;
    }
}

} // namespace iris::controller
