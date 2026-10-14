#include "daisy/infra/process.hpp"

#include <cstdio>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#define daisy_popen _popen
#define daisy_pclose _pclose
#else
#include <sys/wait.h>
#include <unistd.h>
#define daisy_popen popen
#define daisy_pclose pclose
#endif

namespace daisy::infra {

namespace {

std::string quote_arg(const std::string& arg) {
#ifdef _WIN32
    // Windows: 双引号包裹（简化；生产实现需处理内部引号转义）。
    return "\"" + arg + "\"";
#else
    // POSIX: 单引号包裹，内部单引号转义。
    std::string out = "'";
    for (const char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
#endif
}

} // namespace

Result<ProcessResult> run_process(const std::vector<std::string>& args,
                                  const std::string& /*cwd*/,
                                  int /*timeout_seconds*/) {
    if (args.empty()) return Result<ProcessResult>::Err("空命令");

    std::ostringstream cmd;
    for (const auto& a : args) {
        if (cmd.tellp() != std::streampos(0)) cmd << ' ';
        cmd << quote_arg(a);
    }
    cmd << " 2>&1";

    FILE* pipe = daisy_popen(cmd.str().c_str(), "r");
    if (!pipe) return Result<ProcessResult>::Err("无法启动进程: " + args.front());

    std::string output;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) {
        output.append(buf, n);
    }
    const int rc = daisy_pclose(pipe);

    ProcessResult pr;
#ifndef _WIN32
    // POSIX: pclose 返回 wait status，提取退出码。
    pr.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#else
    pr.exit_code = rc;
#endif
    pr.stdout_text = output;
    pr.stderr_text.clear(); // 已合并到 stdout（2>&1）
    return Result<ProcessResult>::Ok(std::move(pr));
}

} // namespace daisy::infra
