#pragma once

namespace iris::infra {

// ---------------------------------------------------------------------------
// 推理线程数解析。
//
// 实测（14 逻辑核的混合架构 CPU，whisper-tiny / 11 s 音频）：
//   threads= 1 → 3.7 s    4 → 1.3 s    8 → 1.0 s    10 → 0.9 s    12 → 0.9 s
//   threads=14 → 75 s    16 → 94 s    32 → 382 s
// 即：超过某个点之后线程过订阅会把推理拖慢 70–400 倍。
// 因此「自动」不再直接用 hardware_concurrency，而是封顶在 8；
// 用户仍可用 --threads / IRIS_THREADS 显式覆盖。
// ---------------------------------------------------------------------------

/// 自动线程数上限（不含用户显式指定）。
constexpr int kMaxAutoThreads = 8;

/// requested > 0 时原样返回；否则取 min(kMaxAutoThreads, hardware_concurrency)。
/// 环境变量 IRIS_THREADS 优先于「自动」，低于显式参数。
int resolve_threads(int requested = 0);

} // namespace iris::infra
