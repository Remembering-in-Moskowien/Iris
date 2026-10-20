#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>

#include "iris/model/ir_document.hpp"

namespace iris::controller {

int run_validate(const std::string& ir_path) {
    const auto r = model::validate_file(ir_path);
    std::cout << (r.is_valid ? "OK: " : "FAIL: ") << ir_path << " (version " << r.version
              << ")\n";
    for (const auto& issue : r.issues) std::cout << "  - " << issue << '\n';
    return r.is_valid ? 0 : 1;
}

} // namespace iris::controller
