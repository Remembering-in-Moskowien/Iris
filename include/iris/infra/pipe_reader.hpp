#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iris::infra {

/// 流式子进程：启动命令后从 stdout 管道逐块读取二进制数据，stderr 单独消费。
/// 用于 ffmpeg rawvideo 抽帧等需要边生产边消费的场景（区别于 run_process 的一次性收集）。
class StreamProcess {
public:
    StreamProcess() = default;
    ~StreamProcess();

    StreamProcess(const StreamProcess&) = delete;
    StreamProcess& operator=(const StreamProcess&) = delete;

    /// 启动进程。args[0] 为可执行文件。stderr 重定向到独立管道并由后台线程排空。
    bool start(const std::vector<std::string>& args);

    /// 从 stdout 读满 n 字节（EOF 返回已读字节数 < n）。
    std::size_t read_exact(void* buf, std::size_t n);

    /// 退出码（进程结束后有效）。
    int exit_code() const;

    /// 停止进程并回收句柄。
    void close();

private:
#ifdef _WIN32
    void* h_read_stdout_ = nullptr;   // HANDLE
    void* h_process_ = nullptr;        // HANDLE
    int exit_code_ = -1;
#endif
};

} // namespace iris::infra
