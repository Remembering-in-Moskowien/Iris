#pragma once

#include <string>
#include <vector>

#include "iris/core/result.hpp"

namespace iris::infra {

/// 子进程执行结果。
struct ProcessResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;

    [[nodiscard]] bool success() const noexcept { return exit_code == 0; }
};

/// 运行外部命令（参数列表，自动引用），返回合并 stdout+stderr。
/// timeout_seconds：0 表示不限制。
Result<ProcessResult> run_process(const std::vector<std::string>& args,
                                  const std::string& cwd = "",
                                  int timeout_seconds = 0);

/// 同 run_process，但 stdout 与 stderr **分开**捕获（JSON 模式下 stdout 是结果文档、
/// stderr 是日志，两者不能混）。两侧都会在子进程运行期间被持续排空，不会因管道写满而死锁。
Result<ProcessResult> run_process_split(const std::vector<std::string>& args,
                                        const std::string& cwd = "",
                                        int timeout_seconds = 0);

} // namespace iris::infra
