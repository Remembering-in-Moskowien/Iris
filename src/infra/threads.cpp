#include "iris/infra/threads.hpp"

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace iris::infra {

int resolve_threads(int requested) {
    if (requested > 0) return requested;

    // 环境变量覆盖（便于在 CI / 低功耗机器上固定）
    if (const char* env = std::getenv("IRIS_THREADS"); env != nullptr && *env != '\0') {
        const int v = std::atoi(env);
        if (v > 0) return v;
    }

    const unsigned hw = std::thread::hardware_concurrency();
    const int cores = static_cast<int>(std::max(1u, hw));
    return std::min(cores, kMaxAutoThreads);
}

} // namespace iris::infra
