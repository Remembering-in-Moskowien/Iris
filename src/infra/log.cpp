#include "iris/infra/log.hpp"

#include <cctype>
#include <filesystem>
#include <memory>
#include <string>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "iris/ui/console.hpp"
#include "iris/infra/paths.hpp"

namespace iris::infra {

namespace {

/// 把日志行交给 ui 的输出层：Windows 控制台上用 WriteConsoleW，
/// 重定向时写 UTF-8 字节，两条路径都不会出现「按 GBK 解码 UTF-8」的乱码。
/// agent 模式：把 "info message" 变成 "INFO  message"（固定宽度级别前缀）。
std::string uppercase_level(std::string line) {
    const auto space = line.find(' ');
    if (space == std::string::npos || space == 0) return line;
    std::string level = line.substr(0, space);
    for (auto& c : level) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (level.size() < 5) level.push_back(' ');
    return level + " " + line.substr(space + 1);
}

class IrisSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit IrisSink(bool to_stderr) : to_stderr_(to_stderr) {}

    void set_agent(bool on) { agent_ = on; }


protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // base_sink 已带 formatter_（由 spdlog::logger 装配），直接用。
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        std::string line(buf.data(), buf.size());
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (agent_) line = uppercase_level(std::move(line));
        else if (iris::ui::colors_enabled()) {
            if (msg.level >= spdlog::level::err) line = iris::ui::red(line);
            else if (msg.level == spdlog::level::warn) line = iris::ui::yellow(line);
            else if (msg.level == spdlog::level::debug) line = iris::ui::dim(line);
        }
        line.push_back('\n');
        iris::ui::write_utf8(line, to_stderr_);
    }

    void flush_() override {}

private:
    bool to_stderr_ = false;
    bool agent_ = false;
};

} // namespace

void init_logger(bool verbose, const std::string& log_file) {
    iris::ui::init();

    // JSON 模式下 stdout 只放结果文档，日志改走 stderr
    auto sink = std::make_shared<IrisSink>(iris::ui::json_mode());
    sink->set_agent(iris::ui::agent_mode());
    auto logger = std::make_shared<spdlog::logger>("iris", sink);
    const std::string pattern = iris::ui::agent_mode() ? "%l %v"
                                                       : "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";
    logger->set_formatter(std::make_unique<spdlog::pattern_formatter>(pattern));
    std::string file_path = log_file;
    if (file_path.empty()) file_path = ".cache/iris/iris.log";
    try {
        const auto path = infra::utf8_path(file_path);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            infra::path_to_utf8(path), true);
        file_sink->set_pattern(pattern);
        logger->sinks().push_back(file_sink);
    } catch (const std::exception& e) {
        iris::ui::warning(std::string("无法打开日志文件：") + e.what());
    }
    logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}

} // namespace iris::infra
