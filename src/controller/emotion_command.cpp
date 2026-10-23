#include "iris/controller/commands.hpp"

#include <iostream>
#include <string>

#include "iris/emotion/emotion.hpp"

namespace iris::controller {

int run_emotion(const std::string& onnx_path, const std::string& wav_path) {
    emotion::EmotionResult r;
    std::string err;
    if (!emotion::analyze_emotion(onnx_path, wav_path, r, err)) {
        std::cerr << "emotion: " << err << "\n";
        return 1;
    }
    std::cout << "emotion: " << wav_path << "\n"
              << "  arousal    = " << r.arousal << "\n"
              << "  dominance  = " << r.dominance << "\n"
              << "  valence    = " << r.valence << "\n";
    return 0;
}

} // namespace iris::controller
