#include "iris/infra/pipe_reader.hpp"

#include <sstream>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace iris::infra {

namespace {
#ifdef _WIN32
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
        for (char c : a) {
            if (c == '"') cmd << L"\\\"";
            else cmd << static_cast<wchar_t>(static_cast<unsigned char>(c));
        }
        cmd << L'"';
    }
    return cmd.str();
}
#endif
} // namespace

StreamProcess::~StreamProcess() { close(); }

bool StreamProcess::start(const std::vector<std::string>& args) {
    if (args.empty()) return false;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};

    HANDLE h_stdout_read = nullptr, h_stdout_write = nullptr;
    HANDLE h_stderr_read = nullptr, h_stderr_write = nullptr;
    if (!CreatePipe(&h_stdout_read, &h_stdout_write, &sa, 0)) return false;
    if (!CreatePipe(&h_stderr_read, &h_stderr_write, &sa, 0)) {
        CloseHandle(h_stdout_read); CloseHandle(h_stdout_write); return false;
    }
    SetHandleInformation(h_stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_stderr_read, HANDLE_FLAG_INHERIT, 0);

    const std::wstring cmdline = build_command_line(args);
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = h_stdout_write;
    si.hStdError = h_stderr_write;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(h_stdout_read); CloseHandle(h_stdout_write);
        CloseHandle(h_stderr_read); CloseHandle(h_stderr_write);
        return false;
    }
    // 父进程关闭写端，读端 EOF 由子进程退出触发。
    CloseHandle(h_stdout_write);
    CloseHandle(h_stderr_write);

    // 后台排空 stderr（防止子进程 stderr 写满管道阻塞）。
    std::thread([h_stderr_read] {
        char b[4096]; DWORD n = 0;
        while (ReadFile(h_stderr_read, b, sizeof(b), &n, nullptr) && n > 0) {}
        CloseHandle(h_stderr_read);
    }).detach();

    h_read_stdout_ = h_stdout_read;
    h_process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
#else
    // POSIX 简化：pipe + fork/exec（本项目当前仅 Windows，POSIX 留占位）。
    (void)args;
    return false;
#endif
}

std::size_t StreamProcess::read_exact(void* buf, std::size_t n) {
#ifdef _WIN32
    if (!h_read_stdout_) return 0;
    std::size_t got = 0;
    while (got < n) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(n - got > 65536 ? 65536 : n - got);
        if (!ReadFile(h_read_stdout_, static_cast<char*>(buf) + got, want, &chunk, nullptr))
            break;
        if (chunk == 0) break;
        got += chunk;
    }
    return got;
#else
    (void)buf; (void)n;
    return 0;
#endif
}

int StreamProcess::exit_code() const {
#ifdef _WIN32
    if (!h_process_) return -1;
    DWORD code = 0;
    GetExitCodeProcess(h_process_, &code);
    return static_cast<int>(code);
#else
    return -1;
#endif
}

void StreamProcess::close() {
#ifdef _WIN32
    if (h_read_stdout_) { CloseHandle(h_read_stdout_); h_read_stdout_ = nullptr; }
    if (h_process_) {
        WaitForSingleObject(h_process_, 2000);
        DWORD code = 0; GetExitCodeProcess(h_process_, &code);
        exit_code_ = static_cast<int>(code);
        CloseHandle(h_process_); h_process_ = nullptr;
    }
#endif
}

} // namespace iris::infra
