#include "iris/infra/process.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

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
/// 宽字符转换（UTF-8 → UTF-16）。
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

/// 按 CommandLineToArgvW 的规则拼命令行：每个参数加双引号，内部引号转义。
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

/// 持续把管道里的字节读干（必须在等待子进程期间进行，否则 64 KB 后子进程会阻塞）。
class PipeDrain {
public:
    void start(HANDLE pipe) {
        thread_ = std::thread([this, pipe] {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
                data_.append(buf, n);
            }
        });
    }
    void join() {
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] const std::string& data() const { return data_; }

private:
    HANDLE pipe_ = nullptr;
    std::thread thread_;
    std::string data_;
};

/// 建立一对 (read, write) 管道，读端不继承。
bool make_pipe(HANDLE& read_end, HANDLE& write_end) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return false;
    if (!SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return false;
    }
    return true;
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

std::string make_temp_path() {
    char tmpl[] = "/tmp/iris-proc-XXXXXX";
    const int fd = mkstemp(tmpl);
    if (fd >= 0) ::close(fd);
    return tmpl;
}
#endif

} // namespace

Result<ProcessResult> run_process(const std::vector<std::string>& args, const std::string& cwd,
                                  int timeout_seconds) {
    if (args.empty()) return Result<ProcessResult>::Err("空命令");

#ifdef _WIN32
    HANDLE h_read = nullptr, h_write = nullptr;
    if (!make_pipe(h_read, h_write)) return Result<ProcessResult>::Err("无法创建输出管道");

    const std::wstring cmdline = build_command_line(args);
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = h_write;
    si.hStdError = h_write; // 合并：日志与结果共用一条流

    PROCESS_INFORMATION pi{};
    std::wstring wcwd = to_wide(cwd);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        CloseHandle(h_read);
        CloseHandle(h_write);
        return Result<ProcessResult>::Err("无法启动进程: " + args.front());
    }
    CloseHandle(h_write);

    // 先起排空线程，再等进程退出：否则子进程写满管道会死锁。
    PipeDrain drain;
    drain.start(h_read);

    const DWORD wait_ms = timeout_seconds > 0 ? static_cast<DWORD>(timeout_seconds) * 1000 : INFINITE;
    const DWORD wait_rc = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    drain.join();
    CloseHandle(h_read);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ProcessResult pr;
    pr.exit_code = static_cast<int>(exit_code);
    pr.stdout_text = drain.data();
    pr.stderr_text.clear(); // 已合并
    return Result<ProcessResult>::Ok(std::move(pr));
#else
    std::ostringstream cmd;
    for (const auto& a : args) {
        if (cmd.tellp() != std::streampos(0)) cmd << ' ';
        cmd << quote_arg(a);
    }
    cmd << " 2>&1";
    FILE* pipe = iris_popen(cmd.str().c_str(), "r");
    if (pipe == nullptr) return Result<ProcessResult>::Err("无法启动进程: " + args.front());
    std::string output;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe) != nullptr) output += buf;
    const int status = iris_pclose(pipe);
    ProcessResult pr;
    pr.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    pr.stdout_text = output;
    return Result<ProcessResult>::Ok(std::move(pr));
#endif
}

Result<ProcessResult> run_process_split(const std::vector<std::string>& args,
                                        const std::string& cwd, int timeout_seconds) {
    if (args.empty()) return Result<ProcessResult>::Err("空命令");

#ifdef _WIN32
    HANDLE out_read = nullptr, out_write = nullptr;
    HANDLE err_read = nullptr, err_write = nullptr;
    if (!make_pipe(out_read, out_write)) return Result<ProcessResult>::Err("无法创建输出管道");
    if (!make_pipe(err_read, err_write)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        return Result<ProcessResult>::Err("无法创建错误管道");
    }

    const std::wstring cmdline = build_command_line(args);
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_write;
    si.hStdError = err_write;

    PROCESS_INFORMATION pi{};
    std::wstring wcwd = to_wide(cwd);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        CloseHandle(err_read);
        CloseHandle(err_write);
        return Result<ProcessResult>::Err("无法启动进程: " + args.front());
    }
    CloseHandle(out_write);
    CloseHandle(err_write);

    PipeDrain out_drain;
    PipeDrain err_drain;
    out_drain.start(out_read);
    err_drain.start(err_read);

    const DWORD wait_ms = timeout_seconds > 0 ? static_cast<DWORD>(timeout_seconds) * 1000 : INFINITE;
    const DWORD wait_rc = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_rc == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    out_drain.join();
    err_drain.join();
    CloseHandle(out_read);
    CloseHandle(err_read);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    ProcessResult pr;
    pr.exit_code = static_cast<int>(exit_code);
    pr.stdout_text = out_drain.data();
    pr.stderr_text = err_drain.data();
    return Result<ProcessResult>::Ok(std::move(pr));
#else
    // POSIX：stderr 重定向到临时文件，stdout 走 popen。
    const std::string err_file = make_temp_path();
    std::ostringstream cmd;
    for (const auto& a : args) {
        if (cmd.tellp() != std::streampos(0)) cmd << ' ';
        cmd << quote_arg(a);
    }
    cmd << " 2>" << quote_arg(err_file);
    FILE* pipe = iris_popen(cmd.str().c_str(), "r");
    if (pipe == nullptr) return Result<ProcessResult>::Err("无法启动进程: " + args.front());
    std::string output;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe) != nullptr) output += buf;
    const int status = iris_pclose(pipe);

    std::string err_text;
    {
        std::FILE* f = std::fopen(err_file.c_str(), "rb");
        if (f != nullptr) {
            char chunk[4096];
            std::size_t n = 0;
            while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) err_text.append(chunk, n);
            std::fclose(f);
        }
        std::remove(err_file.c_str());
    }

    ProcessResult pr;
    pr.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    pr.stdout_text = output;
    pr.stderr_text = err_text;
    return Result<ProcessResult>::Ok(std::move(pr));
#endif
}

} // namespace iris::infra
