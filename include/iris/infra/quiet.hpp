#pragma once

namespace iris::infra {

// ---------------------------------------------------------------------------
// 临时静音 stderr（作用域内）。
//
// 推理运行时（CrispASR/whisper 等）会直接把模型加载细节写到 stderr，
// 与 iris 自己的输出风格混在一起。默认把它吞掉；设 IRIS_VERBOSE=1 时保留。
// iris 自身的日志走 spdlog（stdout），错误通过返回值上报，所以静音 stderr
// 不影响任何有用的信息。
// ---------------------------------------------------------------------------

class StderrSilencer {
public:
    StderrSilencer();
    ~StderrSilencer();
    StderrSilencer(const StderrSilencer&) = delete;
    StderrSilencer& operator=(const StderrSilencer&) = delete;

private:
    bool active_ = false;
    int saved_fd_ = -1;
};

} // namespace iris::infra
