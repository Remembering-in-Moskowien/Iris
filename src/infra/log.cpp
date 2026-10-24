#include "iris/infra/log.hpp"

#include <memory>
#include <string>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>

#include "iris/ui/console.hpp"

namespace iris::infra {

namespace {

/// 把日志行交给 ui 的输出层：Windows 控制台上用 WriteConsoleW，
/// 重定向时写 UTF-8 字节，两条路径都不会出现「按 GBK 解码 UTF-8」的乱码。
class IrisSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit IrisSink(bool to_stderr) : to_stderr_(to_stderr) {}


protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // base_sink 已带 formatter_（由 spdlog::logger 装配），直接用。
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        iris::ui::write_utf8(std::string(buf.data(), buf.size()), to_stderr_);
    }

    void flush_() override {}

private:
    bool to_stderr_ = false;
};

} // namespace

void init_logger(bool verbose) {
    iris::ui::init();

    auto sink = std::make_shared<IrisSink>(false);
    auto logger = std::make_shared<spdlog::logger>("iris", sink);
    logger->set_formatter(std::make_unique<spdlog::pattern_formatter>(
        "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v"));
    logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(logger));
}

} // namespace iris::infra
