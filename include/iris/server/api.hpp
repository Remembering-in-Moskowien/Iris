#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "iris/server/http_server.hpp"

namespace iris::server {

// ---------------------------------------------------------------------------
// 任务管理：把 CLI 子命令包装成异步任务。
//
// 每个任务在外层再起一个 `iris <command> ...` 子进程，工作目录是独立的临时目录：
//   - 服务本身不会被长时间的推理阻塞（子进程跑在后台线程里）；
//   - 命令崩溃不会带走服务；
//   - 产物（IR/字幕/配音）都落在任务目录里，便于 /jobs/{id} 汇报。
// ---------------------------------------------------------------------------

struct Job {
    std::string id;
    std::string command;
    std::vector<std::string> args;
    std::string state = "queued"; ///< queued | running | done | failed
    int exit_code = -1;
    std::string output;      ///< 日志（stderr，或合并输出）
    std::string result_json; ///< JSON 模式下子进程 stdout 的结构化结果
    bool json = false;       ///< 该任务是否以 --json 运行
    std::string work_dir;
    std::string created_at;
    double elapsed_seconds = 0.0;
};

class JobManager {
public:
    JobManager(std::string exe_path, std::string work_root, bool json_mode = false);
    ~JobManager();
    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    /// 允许的子命令白名单。
    static const std::vector<std::string>& allowed_commands();

    /// 提交任务；返回任务 id（失败时 error 说明原因）。
    /// json 参数为真时该任务以 --json 运行（默认跟随服务端的 --json）。
    std::optional<std::string> submit(const std::string& command,
                                      const std::vector<std::string>& args, std::string& error,
                                      bool json = true);

    /// 查询任务快照。
    std::optional<Job> get(const std::string& id);

    /// 所有任务（新的在前）。
    std::vector<Job> list() const;

    /// 任务目录里的文件（名 + 字节数）。
    std::vector<std::pair<std::string, std::uintmax_t>> files(const std::string& id) const;

    /// 任务 JSON（含 state/exitCode/output/files）。
    std::string to_json(const Job& job, bool include_files) const;

private:
    void worker_loop();
    void start_worker();

    std::string exe_path_;
    bool json_mode_ = false;
    std::string work_root_;
    mutable std::mutex mutex_;
    std::deque<std::string> queue_;
    std::vector<Job> jobs_;
    std::condition_variable cv_;
    bool shutting_down_ = false;
    std::thread worker_;
    int counter_ = 0;
};

/// 注册 /health /models /commands /jobs 路由。
void register_api_routes(HttpServer& server, JobManager& jobs, const std::string& models_root);

/// 取当前可执行文件路径（Windows: GetModuleFileNameW；Linux: /proc/self/exe）。
std::string executable_path();

} // namespace iris::server
