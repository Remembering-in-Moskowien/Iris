#include "iris/infra/process.hpp"

#include <cstdio>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#define iris_popen popen
#define iris_pclose pclose
#endif

namespace iris::infra {

namespace {

#ifdef _WIN32
// CommandLineToArgvW 兼容的引号规则：每个参数用双引号包裹，内部双引号以 \" 转义。
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring build_command_line(const std::vector<std::string>& args) {
    std::wostringstream cmd;
    for (const auto& a : args) {
        if (cmd.tellp() != std::streampos(0)) cmd << L' ';
        cmd << L'"';
        for (const char c : a) {
            if (c == '"') {
                cmd << L"\\\"";
            } else {
                cmd << static_cast<wchar_t>(static_cast<unsigned char>(c));
            }
        }
        cmd << L'"';
    }
    return cmd.str();
}
#else
std::string quote_arg(const std::string& arg) {
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
}
#endif

} // namespace

Result<ProcessResult> run_process(const std::vector<std::string>& args,
                                  const std::string& cwd,
                                  int timeout_seconds) {
    if (args.empty()) return Result<ProcessResult>::Err("空命令");

#ifdef _WIN32
    // Windows：CreateProcess 直接执行（不经 cmd.exe，避免 shell 引号/元字符解析）。
    HANDLE h_read = nullptr, h_write = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&h_read, &h_write, &sa, 0)) {
        return Result<ProcessResult>::Err("无法创建输出管道");
    }
    // 读端不继承，写端由子进程继承。
    if (!SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(h_read);
        CloseHandle(h_write);
        return Result<ProcessResult>::Err("无法设置管道句柄");
    }

    const std::wstring cmdline = build_command_line(args);
    // CreateProcess 需要可写缓冲区。
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = h_write;
    si.hStdError = h_write;

    PROCESS_INFORMATION pi{};
    std::wstring wcwd = to_wide(cwd);

    // lpApplicationName 传 NULL：由 lpCommandLine 解析第一个 token 并按 PATH 搜索可执行文件。
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, wcwd.empty() ? nullptr : wcwd.c_str(),
                        &si, &pi)) {
        CloseHandle(h_read);
        CloseHandle(h_write);
        return Result<ProcessResult>::Err("无法启动进程: " + args.front());
    }
    CloseHandle(h_write); // 父进程关闭写端，读端 EOF 由子进程退出触发。

    // 超时等待：timeout_seconds <= 0 表示不限时。
    DWORD wait_ms = timeout_seconds > 0 ? static_cast<DWORD>(timeout_seconds) * 1000
                                        : INFINITE;
    const DWORD wait_rc = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }

    std::string output;
    char buf_out[4096];
    DWORD n = 0;
    while (ReadFile(h_read, buf_out, sizeof(buf_out), &n, nullptr) && n > 0) {
        output.append(buf_out, n);
    }
    CloseHandle(h_read);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ProcessResult pr;
    pr.exit_code = static_cast<int>(exit_code);
    pr.stdout_text = output;
    pr.stderr_text.clear(); // 已合并（hStdError 指向同一管道）
    return Result<ProcessResult>::Ok(std::move(pr));
#else
    // POSIX：popen 经 shell 执行（参数单引号包裹，无注入面）。
    std::ostringstream cmd;
    for (const auto& a : args) {
        if (cmd.tellp() != std::streampos(0)) cmd << ' ';
        cmd << quote_arg(a);
    }
    cmd << " 2>&1";

    FILE* pipe = iris_popen(cmd.str().c_str(), "r");
    if (!pipe) return Result<ProcessResult>::Err("无法启动进程: " + args.front());

    std::string output;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) {
        output.append(buf, n);
    }
    const int rc = iris_pclose(pipe);

    ProcessResult pr;
    pr.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    pr.stdout_text = output;
    pr.stderr_text.clear(); // 已合并到 stdout（2>&1）
    return Result<ProcessResult>::Ok(std::move(pr));
#endif
}

} // namespace iris::infra
