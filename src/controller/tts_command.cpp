#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>

#include "iris/tts/indextts.hpp"

namespace iris::controller {

int run_tts(const std::string& engine, const std::string& model_dir,
            const std::string& text, const std::string& ref_wav,
            const std::string& out_wav) {
    std::string err;
    bool ok = false;
    if (engine == "indextts") {
        ok = tts::synth_indextts(model_dir, text, ref_wav, out_wav, err);
    } else {
        std::cerr << "tts: 未知引擎 '" << engine << "'（支持: indextts）\n";
        return 1;
    }
    if (!ok) {
        std::cerr << "tts: " << err << "\n";
        return 1;
    }
    std::cout << "tts: 写出 " << out_wav << "\n";
    return 0;
}

} // namespace iris::controller
