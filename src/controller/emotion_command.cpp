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
              << "  " << r.label << "  (" << r.score * 100.0f << "%)\n"
              << "  scores: SAD=" << r.scores[0]
              << " ANGRY=" << r.scores[1]
              << " DISGUST=" << r.scores[2]
              << " FEAR=" << r.scores[3]
              << " HAPPY=" << r.scores[4]
              << " NEUTRAL=" << r.scores[5] << "\n";
    return 0;
}

} // namespace iris::controller
