#include "iris/server/api.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "iris/catalog/models.hpp"
#include "iris/controller/commands.hpp"
#include "iris/infra/log.hpp"
#include "iris/infra/process.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace iris::server {

namespace fs = std::filesystem;

namespace {

std::string now_string() {
    const auto t = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

/// 简单的参数校验：数量与长度上限，避免把服务变成任意命令执行器。
bool validate_args(const std::vector<std::string>& args, std::string& error) {
    if (args.size() > 32) {
        error = "参数过多（上限 32）";
        return false;
    }
    for (const auto& a : args) {
        if (a.size() > 4096) {
            error = "单个参数过长（上限 4096）";
            return false;
        }
        if (a.find('\0') != std::string::npos) {
            error = "参数含非法字符";
            return false;
        }
    }
    return true;
}

} // namespace

std::string executable_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0) return "iris.exe";
    std::wstring w(buf, n);
    return std::string(w.begin(), w.end());
#else
    char buf[4096] = {};
    const auto n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "iris";
    return std::string(buf, static_cast<std::size_t>(n));
#endif
}

const std::vector<std::string>& JobManager::allowed_commands() {
    static const std::vector<std::string> kCommands = {
        "asr",     "ocr",     "convert", "combine", "correct",
        "translate", "dub",   "tts",     "build",   "quality",
    };
    return kCommands;
}

JobManager::JobManager(std::string exe_path, std::string work_root)
    : exe_path_(std::move(exe_path)), work_root_(std::move(work_root)) {
    std::error_code ec;
    fs::create_directories(work_root_, ec);
    start_worker();
}

JobManager::~JobManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void JobManager::start_worker() {
    worker_ = std::thread([this] { worker_loop(); });
}

std::optional<std::string> JobManager::submit(const std::string& command,
                                              const std::vector<std::string>& args,
                                              std::string& error) {
    const auto& allowed = allowed_commands();
    if (std::find(allowed.begin(), allowed.end(), command) == allowed.end()) {
        error = "不支持的命令：" + command;
        return std::nullopt;
    }
    if (!validate_args(args, error)) return std::nullopt;

    Job job;
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = "job" + std::to_string(++counter_);
        job.id = id;
        job.command = command;
        job.args = args;
        job.created_at = now_string();
        job.work_dir = (fs::path(work_root_) / id).string();
    }
    std::error_code ec;
    fs::create_directories(job.work_dir, ec);
    if (ec) {
        error = "无法创建任务目录：" + job.work_dir;
        return std::nullopt;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(job);
        queue_.push_back(id);
    }
    cv_.notify_one();
    spdlog::info("serve: 任务 {} 已排队（{}，{} 个参数）", id, command, args.size());
    return id;
}

void JobManager::worker_loop() {
    while (true) {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return shutting_down_ || !queue_.empty(); });
            if (shutting_down_ && queue_.empty()) return;
            id = queue_.front();
            queue_.pop_front();
            for (auto& job : jobs_) {
                if (job.id == id) {
                    job.state = "running";
                    break;
                }
            }
        }

        std::string command;
        std::vector<std::string> args;
        std::string work_dir;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& job : jobs_) {
                if (job.id == id) {
                    command = job.command;
                    args = job.args;
                    work_dir = job.work_dir;
                    break;
                }
            }
        }

        std::vector<std::string> argv;
        argv.push_back(exe_path_);
        argv.push_back(command);
        argv.insert(argv.end(), args.begin(), args.end());

        const auto started = std::chrono::steady_clock::now();
        const auto r = infra::run_process(argv, work_dir);
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        std::string output;
        int exit_code = -1;
        if (r.ok()) {
            output = r.value().stdout_text;
            if (output.size() > 32 * 1024) output = output.substr(output.size() - 32 * 1024);
            exit_code = r.value().exit_code;
        } else {
            output = "进程启动失败";
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& job : jobs_) {
                if (job.id != id) continue;
                job.exit_code = exit_code;
                job.state = exit_code == 0 ? "done" : "failed";
                job.output = std::move(output);
                job.elapsed_seconds = elapsed;
                break;
            }
        }
        spdlog::info("serve: 任务 {} {}（{}，{:.1f}s）", id, exit_code == 0 ? "完成" : "失败",
                     command, elapsed);
    }
}

std::optional<Job> JobManager::get(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& job : jobs_) {
        if (job.id == id) return job;
    }
    return std::nullopt;
}

std::vector<Job> JobManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Job> out(jobs_.rbegin(), jobs_.rend());
    return out;
}

std::vector<std::pair<std::string, std::uintmax_t>> JobManager::files(const std::string& id) const {
    std::vector<std::pair<std::string, std::uintmax_t>> out;
    std::string work_dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& job : jobs_) {
            if (job.id == id) {
                work_dir = job.work_dir;
                break;
            }
        }
    }
    if (work_dir.empty()) return out;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(work_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        out.emplace_back(fs::relative(entry.path(), work_dir, ec).string(),
                         entry.file_size(ec));
    }
    return out;
}

std::string JobManager::to_json(const Job& job, bool include_files) const {
    nlohmann::json j;
    j["id"] = job.id;
    j["command"] = job.command;
    j["args"] = job.args;
    j["state"] = job.state;
    j["exitCode"] = job.exit_code;
    j["createdAt"] = job.created_at;
    j["elapsedSeconds"] = job.elapsed_seconds;
    j["workDir"] = job.work_dir;
    j["output"] = job.output;
    if (include_files) {
        auto files_json = nlohmann::json::array();
        for (const auto& [name, size] : files(job.id)) {
            files_json.push_back({{"name", name}, {"bytes", size}});
        }
        j["files"] = files_json;
    }
    return j.dump(2);
}

void register_api_routes(HttpServer& server, JobManager& jobs, const std::string& models_root) {
    server.route("GET", "/health", [&jobs](const HttpRequest&) {
        nlohmann::json j;
        j["status"] = "ok";
        j["jobs"] = jobs.list().size();
        j["version"] = iris::controller::kVersion;
        return HttpResponse::json(200, j.dump());
    });

    server.route("GET", "/commands", [](const HttpRequest&) {
        nlohmann::json j;
        j["commands"] = JobManager::allowed_commands();
        j["request"] = {{"method", "POST"}, {"path", "/jobs"}, {"body", "{\"command\":\"asr\",\"args\":[\"in.mp4\",\"--engine\",\"whisper\",\"--model\",\"models/ggml/ggml-base.bin\",\"-o\",\"out.iris.json\"]}"}};
        return HttpResponse::json(200, j.dump(2));
    });

    server.route("GET", "/models", [models_root](const HttpRequest&) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& entry : catalog::all()) {
            arr.push_back({{"key", entry.key},
                           {"category", entry.category},
                           {"name", entry.name},
                           {"bytes", entry.bytes},
                           {"installed", catalog::installed(entry, models_root)}});
        }
        nlohmann::json j;
        j["models"] = arr;
        return HttpResponse::json(200, j.dump(2));
    });

    server.route("GET", "/jobs", [&jobs](const HttpRequest&) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& job : jobs.list()) {
            arr.push_back(nlohmann::json::parse(jobs.to_json(job, false)));
        }
        nlohmann::json j;
        j["jobs"] = arr;
        return HttpResponse::json(200, j.dump(2));
    });

    server.route("POST", "/jobs", [&jobs](const HttpRequest& req) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body.empty() ? "{}" : req.body);
        } catch (const std::exception& e) {
            return HttpResponse::json(400, std::string(R"({"error":"invalid json"})"));
        }
        const std::string command = body.value("command", "");
        std::vector<std::string> args;
        if (body.contains("args") && body["args"].is_array()) {
            for (const auto& a : body["args"]) {
                if (a.is_string()) args.push_back(a.get<std::string>());
            }
        }
        std::string error;
        const auto id = jobs.submit(command, args, error);
        if (!id) {
            nlohmann::json j;
            j["error"] = error;
            return HttpResponse::json(400, j.dump());
        }
        nlohmann::json j;
        j["id"] = *id;
        j["state"] = "queued";
        j["status"] = "/jobs/" + *id;
        return HttpResponse::json(201, j.dump());
    });

    server.route("GET", "/jobs/{id}", [&jobs](const HttpRequest& req) {
        const std::string id = path_param("/jobs/{id}", req.path);
        if (id.empty() || id.find('/') != std::string::npos) {
            return HttpResponse::json(404, R"({"error":"not found"})");
        }
        const auto job = jobs.get(id);
        if (!job) return HttpResponse::json(404, R"({"error":"unknown job"})");
        return HttpResponse::json(200, jobs.to_json(*job, true));
    });
}

} // namespace iris::server
