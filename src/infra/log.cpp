#include "iris/infra/log.hpp"

#include <cctype>
#include <memory>
#include <string>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>

#include "iris/ui/console.hpp"

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
        line.push_back('\n');
        iris::ui::write_utf8(line, to_stderr_);
    }

    void flush_() override {}

private:
    bool to_stderr_ = false;
    bool agent_ = false;
};

} // namespace

void init_logger(bool verbose) {
    iris::ui::init();

    auto sink = std::make_shared<IrisSink>(false);
    sink->set_agent(iris::ui::agent_mode());
    auto logger = std::make_shared<spdlog::logger>("iris", sink);
    // agent 模式：无时间戳、无颜色，固定 "LEVEL message"（便于 LLM/脚本解析）
    const std::string pattern = iris::ui::agent_mode() ? "%^%l%$ %v"
                                                       : "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v";
    logger->set_formatter(std::make_unique<spdlog::pattern_formatter>(pattern));
    logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}

} // namespace iris::infra
