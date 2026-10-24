#include "iris/controller/commands.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

#include "iris/infra/i18n.hpp"
#include "iris/infra/log.hpp"
#include "iris/ui/console.hpp"
#include "iris/server/api.hpp"
#include "iris/server/http_server.hpp"

namespace iris::controller {

namespace {

server::HttpServer* g_server = nullptr;

void on_signal(int) {
    if (g_server != nullptr) g_server->stop();
}

} // namespace

int run_serve(const std::string& host, int port, const std::string& models_root,
              const std::string& work_root) {
    namespace fs = std::filesystem;

    const std::string exe = server::executable_path();
    if (exe.empty() || !fs::exists(exe)) {
        std::cerr << _("serve: cannot locate the iris executable\n",
                       "serve: 无法定位 iris 可执行文件\n");
        return 1;
    }

    server::JobManager jobs(exe, work_root);
    server::HttpServer http;
    server::register_api_routes(http, jobs, models_root);

    g_server = &http;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string error;
    ui::banner("iris serve", std::string("http://") + (host.empty() ? "127.0.0.1" : host) + ":" +
                                 std::to_string(port) + "/");
    ui::section(_("Endpoints", "端点"));
    ui::kv_table({{"GET  /health", _("status + version", "状态与版本")},
                  {"GET  /commands", _("allowed subcommands", "允许的子命令")},
                  {"GET  /models", _("model registry + installed flags", "模型注册表与安装状态")},
                  {"POST /jobs", _("submit {command, args}", "提交任务 {command, args}")},
                  {"GET  /jobs/{id}", _("job state, output, files", "任务状态、输出、产物")}});
    ui::note(_("press Ctrl-C to stop", "按 Ctrl-C 停止"));
    if (!http.listen(host, port, error)) {
        std::cerr << "serve: " << error << '\n';
        g_server = nullptr;
        return 1;
    }
    g_server = nullptr;
    spdlog::info("serve: 已停止");
    return 0;
}

} // namespace iris::controller
