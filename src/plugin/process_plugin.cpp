#include "iris/plugin/process_plugin.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "iris/infra/log.hpp"

namespace iris::plugin {
namespace {

#ifdef _WIN32
std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size);
    return out;
}

std::wstring build_command_line(const std::vector<std::string>& args) {
    std::wstring line;
    for (const auto& arg : args) {
        if (!line.empty()) line.push_back(L' ');
        line.push_back(L'"');
        const std::wstring wide = to_wide(arg);
        for (const wchar_t c : wide) {
            if (c == L'"') line += L"\\\"";
            else line.push_back(c);
        }
        line.push_back(L'"');
    }
    return line;
}
#endif

class ChildProcess {
public:
    ChildProcess(std::string id, std::string command, std::vector<std::string> args,
                 std::string cwd)
        : id_(std::move(id)), command_(std::move(command)), args_(std::move(args)),
          cwd_(std::move(cwd)) {}

    ~ChildProcess() {
        terminate();
        close_stdin();
        join_readers();
#ifdef _WIN32
        if (stdout_read_ != nullptr) CloseHandle(stdout_read_);
        if (stderr_read_ != nullptr) CloseHandle(stderr_read_);
        if (process_ != nullptr) CloseHandle(process_);
#else
        if (stdout_read_ >= 0) ::close(stdout_read_);
        if (stderr_read_ >= 0) ::close(stderr_read_);
#endif
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(std::string& error) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE in_read = nullptr, in_write = nullptr;
        HANDLE out_read = nullptr, out_write = nullptr;
        HANDLE err_read = nullptr, err_write = nullptr;
        const auto close_all = [&] {
            for (HANDLE* handle : {&in_read, &in_write, &out_read, &out_write, &err_read,
                                   &err_write}) {
                if (*handle != nullptr) {
                    CloseHandle(*handle);
                    *handle = nullptr;
                }
            }
        };
        if (!CreatePipe(&in_read, &in_write, &sa, 0) ||
            !CreatePipe(&out_read, &out_write, &sa, 0) ||
            !CreatePipe(&err_read, &err_write, &sa, 0)) {
            close_all();
            error = "cannot create plugin pipes";
            return false;
        }
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = in_read;
        startup.hStdOutput = out_write;
        startup.hStdError = err_write;

        std::vector<std::string> command_line{command_};
        command_line.insert(command_line.end(), args_.begin(), args_.end());
        std::wstring command = build_command_line(command_line);
        std::vector<wchar_t> command_buffer(command.begin(), command.end());
        command_buffer.push_back(0);
        const std::wstring cwd = to_wide(cwd_);

        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command_buffer.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                            &startup, &process)) {
            close_all();
            error = "cannot start plugin process: " + command_;
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(in_read);
        CloseHandle(out_write);
        CloseHandle(err_write);
        in_read = out_write = err_write = nullptr;
        stdin_write_ = in_write;
        stdout_read_ = out_read;
        stderr_read_ = err_read;
        process_ = process.hProcess;
        stdout_thread_ = std::thread([this] { read_stdout(); });
        stderr_thread_ = std::thread([this] { read_stderr(); });
        return true;
#else
        int in_pipe[2]{-1, -1};
        int out_pipe[2]{-1, -1};
        int err_pipe[2]{-1, -1};
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
            error = "cannot create plugin pipes";
            return false;
        }
        const pid_t pid = fork();
        if (pid < 0) {
            error = "cannot fork plugin process";
            return false;
        }
        if (pid == 0) {
            if (!cwd_.empty()) {
                if (chdir(cwd_.c_str()) != 0) _exit(126);
            }
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0],
                           err_pipe[1]}) {
                ::close(fd);
            }
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(command_.c_str()));
            for (const auto& arg : args_) argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            execvp(command_.c_str(), argv.data());
            _exit(127);
        }
        ::close(in_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        stdin_write_ = in_pipe[1];
        stdout_read_ = out_pipe[0];
        stderr_read_ = err_pipe[0];
        pid_ = pid;
        stdout_thread_ = std::thread([this] { read_stdout(); });
        stderr_thread_ = std::thread([this] { read_stderr(); });
        return true;
#endif
    }

    bool write_line(const std::string& line, std::string& error) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (stdin_write_ == nullptr
#ifdef _WIN32
            || stdin_write_ == INVALID_HANDLE_VALUE
#else
            || stdin_write_ < 0
#endif
        ) {
            error = "plugin stdin is closed";
            return false;
        }
        const std::string data = line + "\n";
#ifdef _WIN32
        std::size_t offset = 0;
        while (offset < data.size()) {
            DWORD written = 0;
            if (!WriteFile(stdin_write_, data.data() + offset,
                           static_cast<DWORD>(data.size() - offset), &written, nullptr)) {
                error = "cannot write to plugin stdin";
                return false;
            }
            offset += written;
        }
#else
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t written = ::write(stdin_write_, data.data() + offset,
                                            data.size() - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                error = "cannot write to plugin stdin";
                return false;
            }
            offset += static_cast<std::size_t>(written);
        }
#endif
        return true;
    }

    bool read_line(std::string& line, int timeout_ms, std::string& error) {
        std::unique_lock<std::mutex> lock(read_mutex_);
        const auto ready = [this] { return !lines_.empty() || stdout_closed_; };
        if (timeout_ms <= 0) {
            read_cv_.wait(lock, ready);
        } else if (!read_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            error = "plugin call timed out";
            return false;
        }
        if (lines_.empty()) {
            error = "plugin closed stdout";
            return false;
        }
        line = std::move(lines_.front());
        lines_.pop_front();
        return true;
    }

    void close_stdin() {
        std::lock_guard<std::mutex> lock(write_mutex_);
#ifdef _WIN32
        if (stdin_write_ != nullptr && stdin_write_ != INVALID_HANDLE_VALUE) {
            CloseHandle(stdin_write_);
        }
        stdin_write_ = nullptr;
#else
        if (stdin_write_ >= 0) ::close(stdin_write_);
        stdin_write_ = -1;
#endif
    }

    bool running() {
#ifdef _WIN32
        return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
#else
        if (pid_ <= 0) return false;
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            pid_ = -1;
            return false;
        }
        return result == 0;
#endif
    }

    void terminate() {
#ifdef _WIN32
        if (process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 2000);
        }
#else
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                if (waitpid(pid_, &status, WNOHANG) == pid_) {
                    pid_ = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (pid_ > 0) {
                kill(pid_, SIGKILL);
                int status = 0;
                waitpid(pid_, &status, 0);
                pid_ = -1;
            }
        }
#endif
    }

private:
    void join_readers() {
        if (stdout_thread_.joinable()) stdout_thread_.join();
        if (stderr_thread_.joinable()) stderr_thread_.join();
    }

    void push_line(std::string line) {
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            lines_.push_back(std::move(line));
        }
        read_cv_.notify_all();
    }

    void read_stdout() {
#ifdef _WIN32
        char buffer[4096];
        std::string pending;
        DWORD read = 0;
        while (ReadFile(stdout_read_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            consume(buffer, read, pending, false);
        }
        if (!pending.empty()) push_line(std::move(pending));
#else
        char buffer[4096];
        std::string pending;
        while (true) {
            const ssize_t read = ::read(stdout_read_, buffer, sizeof(buffer));
            if (read <= 0) break;
            consume(buffer, static_cast<std::size_t>(read), pending, false);
        }
        if (!pending.empty()) push_line(std::move(pending));
#endif
        {
            std::lock_guard<std::mutex> lock(read_mutex_);
            stdout_closed_ = true;
        }
        read_cv_.notify_all();
    }

    void read_stderr() {
#ifdef _WIN32
        char buffer[4096];
        std::string pending;
        DWORD read = 0;
        while (ReadFile(stderr_read_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            consume(buffer, read, pending, true);
        }
#else
        char buffer[4096];
        std::string pending;
        while (true) {
            const ssize_t read = ::read(stderr_read_, buffer, sizeof(buffer));
            if (read <= 0) break;
            consume(buffer, static_cast<std::size_t>(read), pending, true);
        }
#endif
        if (!pending.empty()) spdlog::warn("[plugin {}] {}", id_, pending);
    }

    void consume(const char* data, std::size_t size, std::string& pending, bool stderr_stream) {
        pending.append(data, size);
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (stderr_stream) spdlog::warn("[plugin {}] {}", id_, line);
            else push_line(std::move(line));
        }
        if (pending.size() > (8u << 20)) {
            spdlog::warn("[plugin {}] dropping oversized output buffer", id_);
            pending.clear();
        }
    }

    std::string id_;
    std::string command_;
    std::vector<std::string> args_;
    std::string cwd_;

#ifdef _WIN32
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;
    HANDLE process_ = nullptr;
#else
    int stdin_write_ = -1;
    int stdout_read_ = -1;
    int stderr_read_ = -1;
    pid_t pid_ = -1;
#endif

    std::thread stdout_thread_;
    std::thread stderr_thread_;
    std::mutex write_mutex_;
    std::mutex read_mutex_;
    std::condition_variable read_cv_;
    std::deque<std::string> lines_;
    bool stdout_closed_ = false;
};

bool rpc_call(ChildProcess& child, const PluginManifest& manifest, const std::string& method,
              const nlohmann::json& params, nlohmann::json& result, std::string& error,
              int timeout_ms) {
    const int id = [&] {
        static std::mutex mutex;
        static int next = 1;
        std::lock_guard<std::mutex> lock(mutex);
        return next++;
    }();

    nlohmann::json request{{"id", id}, {"method", method}, {"params", params}};
    if (!child.write_line(request.dump(), error)) return false;

    const int effective_timeout = timeout_ms > 0 ? timeout_ms : manifest.timeout_ms;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(effective_timeout);
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            error = "plugin '" + manifest.id + "' timed out";
            return false;
        }
        std::string line;
        if (!child.read_line(line, static_cast<int>(remaining.count()), error)) {
            if (error == "plugin call timed out") error = "plugin '" + manifest.id + "' timed out";
            return false;
        }

        nlohmann::json response;
        try {
            response = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception&) {
            spdlog::warn("[plugin {}] ignoring non-JSON stdout line", manifest.id);
            continue;
        }
        if (!response.is_object()) continue;

        if (response.contains("method") && !response.contains("id")) {
            if (response["method"] == "log" && response.contains("params") &&
                response["params"].is_object()) {
                const auto& params_doc = response["params"];
                const std::string level = params_doc.value("level", "info");
                const std::string message = params_doc.value("message", "");
                if (level == "error") spdlog::error("[plugin {}] {}", manifest.id, message);
                else if (level == "warn" || level == "warning") {
                    spdlog::warn("[plugin {}] {}", manifest.id, message);
                } else if (level == "debug") {
                    spdlog::debug("[plugin {}] {}", manifest.id, message);
                } else {
                    spdlog::info("[plugin {}] {}", manifest.id, message);
                }
            }
            continue;
        }
        if (!response.contains("id") || !response["id"].is_number_integer()) continue;
        if (response["id"].get<int>() != id) continue;

        if (response.contains("error")) {
            const auto& error_doc = response["error"];
            if (error_doc.is_object()) {
                const std::string code = error_doc.value("code", "");
                const std::string message = error_doc.value("message", error_doc.dump());
                error = "plugin '" + manifest.id + "' " + method + " failed";
                if (!code.empty()) error += " [" + code + "]";
                error += ": " + message;
            } else {
                error = "plugin '" + manifest.id + "' " + method + "' failed: " +
                        error_doc.dump();
            }
            return false;
        }
        if (!response.contains("result")) {
            error = "plugin '" + manifest.id + "' returned no result";
            return false;
        }
        result = response["result"];
        return true;
    }
}

} // namespace

struct ProcessPlugin::Impl {
    PluginManifest manifest;
    std::unique_ptr<ChildProcess> child;
    nlohmann::json description = nlohmann::json::object();
    int restarts = 0;
    bool ever_started = false;
    bool degraded = false;
};

ProcessPlugin::ProcessPlugin(PluginManifest manifest)
    : impl_(std::make_unique<Impl>()) {
    impl_->manifest = std::move(manifest);
}

ProcessPlugin::~ProcessPlugin() = default;

bool ProcessPlugin::start(std::string& error) {
    if (!impl_->manifest.enabled) {
        error = "plugin '" + impl_->manifest.id + "' is disabled";
        return false;
    }
    if (impl_->child && impl_->child->running()) return true;

    impl_->child = std::make_unique<ChildProcess>(
        impl_->manifest.id, impl_->manifest.resolved_command(), impl_->manifest.args,
        impl_->manifest.working_directory());
    if (!impl_->child->start(error)) {
        impl_->child.reset();
        impl_->degraded = true;
        return false;
    }

    nlohmann::json description;
    if (!rpc_call(*impl_->child, impl_->manifest, "describe", nlohmann::json::object(),
                  description, error, impl_->manifest.timeout_ms)) {
        impl_->child->terminate();
        impl_->child.reset();
        impl_->degraded = true;
        return false;
    }
    if (!description.is_object() || !description.contains("id") ||
        !description["id"].is_string() ||
        description["id"].get<std::string>() != impl_->manifest.id) {
        error = "plugin '" + impl_->manifest.id + "' describe id mismatch";
        impl_->child->terminate();
        impl_->child.reset();
        impl_->degraded = true;
        return false;
    }
    if (!description.contains("kind") ||
        !description["kind"].is_string() ||
        description["kind"].get<std::string>() != impl_->manifest.kind) {
        error = "plugin '" + impl_->manifest.id + "' describe kind mismatch";
        impl_->child->terminate();
        impl_->child.reset();
        impl_->degraded = true;
        return false;
    }
    if (!description.contains("apiVersion") ||
        !description["apiVersion"].is_number_integer() ||
        description["apiVersion"].get<int>() != 1) {
        error = "plugin '" + impl_->manifest.id + "' describe apiVersion mismatch";
        impl_->child->terminate();
        impl_->child.reset();
        impl_->degraded = true;
        return false;
    }

    if (impl_->ever_started) ++impl_->restarts;
    impl_->ever_started = true;
    impl_->degraded = false;
    impl_->description = std::move(description);
    return true;
}

bool ProcessPlugin::restart(std::string& error) {
    stop();
    impl_->degraded = false;
    return start(error);
}

void ProcessPlugin::stop() {
    if (impl_->child) {
        impl_->child->close_stdin();
        impl_->child->terminate();
        impl_->child.reset();
    }
}

bool ProcessPlugin::describe(nlohmann::json& result, std::string& error) {
    if (!start(error)) return false;
    result = impl_->description;
    return true;
}

bool ProcessPlugin::invoke(const nlohmann::json& input, const nlohmann::json& params,
                           nlohmann::json& result, std::string& error) {
    const nlohmann::json request_params{{"kind", impl_->manifest.kind},
                                        {"input", input},
                                        {"params", params}};
    return call("invoke", request_params, result, error);
}

bool ProcessPlugin::call(const std::string& method, const nlohmann::json& params,
                         nlohmann::json& result, std::string& error, int timeout_ms) {
    if (!impl_->child || !impl_->child->running()) {
        if (!start(error)) return false;
    }
    if (!rpc_call(*impl_->child, impl_->manifest, method, params, result, error, timeout_ms)) {
        if (impl_->child) {
            impl_->child->terminate();
            impl_->child.reset();
        }
        impl_->degraded = true;
        return false;
    }
    return true;
}

PluginStatus ProcessPlugin::status() const {
    if (impl_->child && impl_->child->running()) return PluginStatus::Running;
    if (impl_->degraded) return PluginStatus::Degraded;
    return PluginStatus::Stopped;
}

const PluginManifest& ProcessPlugin::manifest() const { return impl_->manifest; }

int ProcessPlugin::restarts() const { return impl_->restarts; }

bool ProcessPlugin::degraded() const { return impl_->degraded; }

} // namespace iris::plugin
