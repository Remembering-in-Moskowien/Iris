#include "iris/ui/console.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace iris::ui {

namespace {

std::once_flag g_init_once;
std::atomic<bool> g_colors{false};
std::atomic<bool> g_unicode{true};
std::atomic<bool> g_tty{false};
std::mutex g_out_mutex;

std::atomic<bool> g_agent{false};
std::atomic<bool> g_json{false};
std::atomic<bool> g_json_emitted{false};
Symbols g_symbols;

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}

/// 中文/全角区间判断（够用的近似 wcwidth）。
bool is_wide(unsigned int cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF);
}

/// 逐码点遍历（非法字节按单字节处理）。
template <typename Fn>
void for_each_codepoint(const std::string& s, Fn&& fn) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        unsigned int cp = c;
        if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            len = 2;
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            len = 3;
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            len = 4;
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        }
        fn(cp, len);
        i += len;
    }
}

std::string paint(const char* code, const std::string& s) {
    if (!g_colors.load()) return s;
    return std::string("\x1b[") + code + "m" + s + "\x1b[0m";
}

Symbols make_symbols(bool unicode) {
    Symbols s;
    if (!unicode) {
        s.check = "OK";
        s.cross = "ERR";
        s.arrow = "->";
        s.play = ">";
        s.dot = "*";
        s.ring = "o";
        s.mid_dot = "-";
        s.unicode = false;
    }
    return s;
}

// ---- Windows 控制台输出（WriteConsoleW，绕开代码页）----
#ifdef _WIN32
bool is_console_handle(HANDLE h) {
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
}

/// 把一段 UTF-16 写到控制台；失败时退回字节写，保证输出不丢。
void write_wide_raw(HANDLE handle, const std::wstring& wide) {
    DWORD written = 0;
    if (WriteConsoleW(handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr) == 0) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            std::string utf8(static_cast<std::size_t>(need), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                                need, nullptr, nullptr);
            WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
}

/// 把 SGR 参数（"1;31" 之类）解析成控制台属性位。
WORD sgr_to_attr(const std::string& params, WORD base) {
    const SgrState base_state;
    const SgrState state = parse_sgr(params, base_state);
    WORD attr = base;
    if (!state.default_fg) {
        static const WORD kFg[8] = {0, FOREGROUND_RED, FOREGROUND_GREEN,
                                    FOREGROUND_RED | FOREGROUND_GREEN, FOREGROUND_BLUE,
                                    FOREGROUND_RED | FOREGROUND_BLUE,
                                    FOREGROUND_GREEN | FOREGROUND_BLUE,
                                    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE};
        attr = static_cast<WORD>((attr & ~static_cast<WORD>(0x0F)) | kFg[state.fg & 7]);
    }
    if (state.bright) {
        attr |= FOREGROUND_INTENSITY;
    } else {
        attr &= static_cast<WORD>(~FOREGROUND_INTENSITY);
    }
    return attr;
}

/// 写一段 UTF-8 到控制台：识别 ANSI SGR（\x1b[...m）并转成控制台属性，
/// 这样不依赖 VT 支持也能上色；其余字节按 UTF-16 写出。
void write_console_wide(const std::string& utf8, HANDLE handle) {
    if (utf8.empty()) return;

    CONSOLE_SCREEN_BUFFER_INFO info{};
    const bool have_info = GetConsoleScreenBufferInfo(handle, &info) != 0;
    const WORD base_attr = have_info ? info.wAttributes : FOREGROUND_RED | FOREGROUND_GREEN |
                                                                 FOREGROUND_BLUE;
    WORD attr = base_attr;

    std::size_t i = 0;
    std::string run;
    const auto flush_run = [&] {
        if (run.empty()) return;
        SetConsoleTextAttribute(handle, attr);
        const int need = MultiByteToWideChar(CP_UTF8, 0, run.data(), static_cast<int>(run.size()),
                                             nullptr, 0);
        if (need > 0) {
            std::wstring wide(static_cast<std::size_t>(need), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, run.data(), static_cast<int>(run.size()), wide.data(),
                                need);
            write_wide_raw(handle, wide);
        }
        run.clear();
    };

    while (i < utf8.size()) {
        if (utf8[i] == '\x1b' && i + 1 < utf8.size() && utf8[i + 1] == '[') {
            const auto m = utf8.find('m', i + 2);
            if (m != std::string::npos) {
                flush_run();
                attr = sgr_to_attr(utf8.substr(i + 2, m - (i + 2)), base_attr);
                i = m + 1;
                continue;
            }
        }
        run.push_back(utf8[i]);
        ++i;
    }
    flush_run();
    SetConsoleTextAttribute(handle, base_attr);
}
#endif

/// 接管 std::cout / std::cerr：控制台上逐行用 WriteConsoleW 输出，
/// 重定向时按 UTF-8 字节写文件（保持可管道、可解析）。
class ConsoleBuf : public std::streambuf {
public:
    ConsoleBuf(bool to_stderr, bool console) : to_stderr_(to_stderr), console_(console) {}

protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        buf_.push_back(static_cast<char>(c));
        if (c == '\n') flush_buffer();
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        buf_.append(s, static_cast<std::size_t>(n));
        if (buf_.find('\n') != std::string::npos) flush_buffer();
        if (buf_.size() > 8192) flush_buffer();
        return n;
    }

    int sync() override {
        flush_buffer();
        return 0;
    }

private:
    void flush_buffer() {
        if (buf_.empty()) return;
        if (console_) {
#ifdef _WIN32
            write_console_wide(buf_, GetStdHandle(to_stderr_ ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE));
#else
            std::fwrite(buf_.data(), 1, buf_.size(), to_stderr_ ? stderr : stdout);
            std::fflush(to_stderr_ ? stderr : stdout);
#endif
        } else {
            std::fwrite(buf_.data(), 1, buf_.size(), to_stderr_ ? stderr : stdout);
            std::fflush(to_stderr_ ? stderr : stdout);
        }
        buf_.clear();
    }

    std::string buf_;
    bool to_stderr_ = false;
    bool console_ = false;
};

/// JSON 模式：吞掉所有经 std::cout 的人类可读输出（结果文档走 json_out 的独立通道）。
class NullBuf : public std::streambuf {
protected:
    int_type overflow(int_type c) override { return traits_type::not_eof(c); }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void init() {
    std::call_once(g_init_once, [] {
        const bool plain_output = env_set("IRIS_PLAIN_OUTPUT");
#ifdef _WIN32
        const bool out_console = is_console_handle(GetStdHandle(STD_OUTPUT_HANDLE));
        const bool err_console = is_console_handle(GetStdHandle(STD_ERROR_HANDLE));
        g_tty.store(out_console);
        // VT 序列（颜色）只在控制台上需要开启；代码页不再改动，
        // 输出一律走 WriteConsoleW，所以用户 shell 的代码页不受影响。
        if (out_console) {
            if (HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); h != INVALID_HANDLE_VALUE) {
                DWORD mode = 0;
                if (GetConsoleMode(h, &mode)) {
                    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
                }
            }
        }
        if (!plain_output) {
            static ConsoleBuf out_buf(false, out_console);
            static ConsoleBuf err_buf(true, err_console);
            std::cout.rdbuf(&out_buf);
            std::cerr.rdbuf(&err_buf);
        }
#else
        g_tty.store(::isatty(STDOUT_FILENO) != 0);
        if (!plain_output) {
            static ConsoleBuf out_buf(false, false);
            static ConsoleBuf err_buf(true, false);
            std::cout.rdbuf(&out_buf);
            std::cerr.rdbuf(&err_buf);
        }
#endif
        // IRIS_FORCE_COLOR=1：即使不是 TTY 也输出颜色（配合 `less -R` 或日志染色）
        g_colors.store((g_tty.load() || env_set("IRIS_FORCE_COLOR")) && !env_set("NO_COLOR") &&
                       !env_set("IRIS_PLAIN"));
        g_unicode.store(!env_set("IRIS_ASCII"));
        g_symbols = make_symbols(g_unicode.load());
    });
}

void write_utf8(const std::string& text, bool to_stderr) {
    init();
#ifdef _WIN32
    HANDLE h = GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (is_console_handle(h)) {
        write_console_wide(text, h);
        return;
    }
#endif
    std::FILE* f = to_stderr ? stderr : stdout;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fflush(f);
}

bool colors_enabled() {
    init();
    return g_colors.load();
}

bool unicode_enabled() {
    init();
    return g_unicode.load();
}

const Symbols& symbols() {
    init();
    return g_symbols;
}

void set_agent_mode(bool on) {
    g_agent.store(on);
    // 先确保初始化跑过（否则随后的 init() 会覆盖这里的设置）
    init();
    if (on) {
        g_tty.store(false);
        g_colors.store(false);
        g_unicode.store(false);
        g_symbols = make_symbols(false);
    } else {
        // 退出 agent 模式：恢复颜色/符号判定
#ifdef _WIN32
        g_tty.store(is_console_handle(GetStdHandle(STD_OUTPUT_HANDLE)));
#else
        g_tty.store(::isatty(STDOUT_FILENO) != 0);
#endif
        g_colors.store((g_tty.load() || env_set("IRIS_FORCE_COLOR")) && !env_set("NO_COLOR") &&
                       !env_set("IRIS_PLAIN"));
        g_unicode.store(!env_set("IRIS_ASCII"));
        g_symbols = make_symbols(g_unicode.load());
    }
}

bool agent_mode() {
    init();
    return g_agent.load();
}

std::string agent_prefix(const std::string& level) {
    if (!g_agent.load()) return {};
    std::string prefix = level;
    while (prefix.size() < 5) prefix.push_back(' ');
    return prefix + ' ';
}

void set_json_mode(bool on) {
    g_json.store(on);
    init();
    static NullBuf null_buf;
    if (on) {
        std::cout.rdbuf(&null_buf); // stdout 只留给结果文档
        // JSON 模式：无颜色、无装饰、无动画
        g_tty.store(false);
        g_colors.store(false);
        g_unicode.store(false);
        g_symbols = make_symbols(false);
    } else if (g_tty.load()) {
        // 退出 JSON 模式：恢复常规输出（仅当原本是控制台时）
        std::cout.rdbuf(nullptr);
    }
}

bool json_mode() {
    init();
    return g_json.load();
}

bool json_was_emitted() { return g_json_emitted.load(); }

void json_out(const std::string& body) {
    if (!g_json.load() || body.empty()) return;
    g_json_emitted.store(true);
    std::lock_guard<std::mutex> lock(g_out_mutex);
    // 直接用字节写 stdout，避免被 ConsoleBuf 的宽字符路径影响
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

std::string bold(const std::string& s) { return paint("1", s); }
std::string dim(const std::string& s) { return paint("90", s); }
std::string green(const std::string& s) { return paint("32", s); }
std::string red(const std::string& s) { return paint("1;31", s); }
std::string yellow(const std::string& s) { return paint("33", s); }
std::string cyan(const std::string& s) { return paint("36", s); }
std::string magenta(const std::string& s) { return paint("1;35", s); }

SgrState parse_sgr(const std::string& params, SgrState base) {
    SgrState state = base;
    std::size_t start = 0;
    while (start <= params.size()) {
        const auto semi = params.find(';', start);
        const std::string item = params.substr(
            start, semi == std::string::npos ? std::string::npos : semi - start);
        const int code = item.empty() ? 0 : std::atoi(item.c_str());
        if (code == 0) {
            state = SgrState{};
        } else if (code == 1) {
            state.bright = true;
        } else if (code == 22) {
            state.bright = false;
        } else if (code >= 30 && code <= 37) {
            state.fg = code - 30;
            state.default_fg = false;
        } else if (code >= 90 && code <= 97) {
            state.fg = code - 90;
            state.default_fg = false;
            state.bright = true;
        } else if (code == 39) {
            state.default_fg = true;
            state.fg = -1;
        }
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return state;
}

std::size_t display_width(const std::string& s) {
    std::size_t w = 0;
    for_each_codepoint(s, [&](unsigned int cp, std::size_t) {
        if (cp < 0x20) return; // 控制字符不占位
        w += is_wide(cp) ? 2 : 1;
    });
    return w;
}

std::string pad_right(const std::string& s, std::size_t width) {
    const auto w = display_width(s);
    if (w >= width) return s;
    return s + std::string(width - w, ' ');
}

std::string human_size(std::int64_t bytes) {
    if (bytes < 0) return "-";
    const double b = static_cast<double>(bytes);
    char buf[32];
    if (b >= 1024.0 * 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f GB", b / (1024.0 * 1024 * 1024));
    } else if (b >= 1024.0 * 1024) {
        std::snprintf(buf, sizeof buf, "%.0f MB", b / (1024.0 * 1024));
    } else if (b >= 1024.0) {
        std::snprintf(buf, sizeof buf, "%.0f KB", b / 1024.0);
    } else {
        std::snprintf(buf, sizeof buf, "%.0f B", b);
    }
    return buf;
}

std::string human_duration(double seconds) {
    if (seconds < 0) return "-";
    char buf[32];
    if (seconds >= 60.0) {
        const int m = static_cast<int>(seconds) / 60;
        const int s = static_cast<int>(seconds) % 60;
        std::snprintf(buf, sizeof buf, "%dm %02ds", m, s);
    } else if (seconds >= 10.0) {
        std::snprintf(buf, sizeof buf, "%.0fs", seconds);
    } else {
        std::snprintf(buf, sizeof buf, "%.1fs", seconds);
    }
    return buf;
}

void banner(const std::string& title, const std::string& subtitle) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << "INFO  " << title;
        if (!subtitle.empty()) std::cout << " - " << subtitle;
        std::cout << '\n';
        return;
    }
    // 边框按「显示宽度」拼：std::string(n, '─') 会重复单字节，产出非法 UTF-8。
    const auto width = std::max<std::size_t>(display_width(title) + 4, 44);
    std::string line;
    if (unicode_enabled()) {
        for (std::size_t i = 0; i < width; ++i) line += "─";
    } else {
        line.assign(width, '-');
    }
    const std::string top_left = unicode_enabled() ? "╭" : "+";
    const std::string top_right = unicode_enabled() ? "╮" : "+";
    const std::string bottom_left = unicode_enabled() ? "╰" : "+";
    const std::string bottom_right = unicode_enabled() ? "╯" : "+";
    const std::string bar = unicode_enabled() ? "│" : "|";
    std::cout << '\n';
    std::cout << dim(top_left + line + top_right) << '\n';
    std::cout << dim(bar + " ") << bold(title) << '\n';
    if (!subtitle.empty()) std::cout << dim(bar + " ") << dim(subtitle) << '\n';
    std::cout << dim(bottom_left + line + bottom_right) << '\n';
}

void section(const std::string& title) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << "INFO  == " << title << " ==\n";
        return;
    }
    std::cout << '\n' << cyan("▌ ") << bold(title) << '\n';
}

void success(const std::string& message) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << "OK    " << message << '\n';
        return;
    }
    std::cout << "  " << green(symbols().check) << ' ' << message << '\n';
}

void failure(const std::string& message) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cerr << "ERROR " << message << '\n';
        return;
    }
    std::cerr << "  " << red(symbols().cross) << ' ' << message << '\n';
}

void warning(const std::string& message) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << "WARN  " << message << '\n';
        return;
    }
    std::cout << "  " << yellow(symbols().dot) << ' ' << message << '\n';
}

void step(const std::string& message) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << "STEP  " << message << '\n';
        return;
    }
    std::cout << "  " << cyan(symbols().arrow) << ' ' << message << '\n';
}

void note(const std::string& message) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << "INFO  " << message << '\n';
        return;
    }
    std::cout << "  " << dim(symbols().mid_dot) << ' ' << dim(message) << '\n';
}

void finish(bool ok, const std::string& message) {
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << (ok ? "OK    " : "ERROR ") << message << '\n';
        return;
    }
    std::cout << '\n' << (ok ? green(symbols().check + " " + message)
                             : red(symbols().cross + " " + message))
              << '\n';
}

void kv_table(const std::vector<std::pair<std::string, std::string>>& rows) {
    if (g_json.load()) return;
    std::size_t key_width = 0;
    for (const auto& [k, _] : rows) key_width = std::max(key_width, display_width(k));
    std::lock_guard<std::mutex> lock(g_out_mutex);
    for (const auto& [k, v] : rows) {
        std::cout << "  " << dim(pad_right(k, key_width)) << "  " << v << '\n';
    }
}

void table(const std::vector<std::string>& headers,
           const std::vector<std::vector<std::string>>& rows) {
    if (g_json.load()) return;
    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t i = 0; i < headers.size(); ++i) widths[i] = display_width(headers[i]);
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], display_width(row[i]));
        }
    }

    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::string head;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (i > 0) head += "  ";
        head += pad_right(headers[i], widths[i]);
    }
    std::cout << "  " << dim(head) << '\n';
    for (const auto& row : rows) {
        std::string line;
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i > 0) line += "  ";
            line += pad_right(row[i], i < widths.size() ? widths[i] : 0);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        std::cout << "  " << line << '\n';
    }
}

// ---------------------------------------------------------------------------
// Spinner
// ---------------------------------------------------------------------------
Spinner::Spinner(std::string label) : label_(std::move(label)) {
    init();
    if (g_json.load()) return;
    if (g_agent.load()) {
        std::lock_guard<std::mutex> lock(g_out_mutex);
        std::cout << "STEP  " << label_ << '\n' << std::flush;
        active_ = true;
        return;
    }
    if (!g_tty.load() || env_set("IRIS_PLAIN")) {
        std::lock_guard<std::mutex> lock(g_out_mutex);
        std::cout << "  " << label_ << "..." << std::flush;
        active_ = true;
        return;
    }
    active_ = true;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << label_ << ' ' << dim("...") << std::flush;
}

Spinner::~Spinner() {
    if (active_) stop(false, label_);
}

void Spinner::update(const std::string& label) {
    if (!active_) return;
    label_ = label;
    if (!g_tty.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "\r\x1b[2K" << "  " << label_ << ' ' << dim("...") << std::flush;
}

void Spinner::stop(bool ok, const std::string& final_text) {
    if (!active_) return;
    active_ = false;
    if (g_json.load()) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_agent.load()) {
        std::cout << (ok ? "OK    " : "ERROR ") << (final_text.empty() ? label_ : final_text)
                  << '\n'
                  << std::flush;
        return;
    }
    if (g_tty.load()) std::cout << "\r\x1b[2K";
    std::cout << "  " << (ok ? green(symbols().check) : red(symbols().cross)) << ' '
              << (final_text.empty() ? label_ : final_text) << '\n'
              << std::flush;
}

// ---------------------------------------------------------------------------
// StagePrinter
// ---------------------------------------------------------------------------
void StagePrinter::begin(const std::string& stage) {
    if (g_json.load()) {
        // JSON 模式不输出阶段行，但仍要维护状态，便于 finish() 收尾
        current_ = stage;
        started_ = std::chrono::steady_clock::now();
        active_ = true;
        return;
    }
    if (g_agent.load()) {
        if (active_) {
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
            std::lock_guard<std::mutex> lock(g_out_mutex);
            std::cout << "OK    " << current_ << " (" << human_duration(secs) << ")\n";
        }
        if (!stage.empty() && stage == current_ && active_) {
            active_ = false;
            return;
        }
        current_ = stage;
        started_ = std::chrono::steady_clock::now();
        active_ = true;
        std::lock_guard<std::mutex> lock(g_out_mutex);
        std::cout << "STEP  " << stage << '\n' << std::flush;
        return;
    }
    // 流水线对同一阶段会回调两次（开始/结束），同名即视为收尾。
    if (active_ && stage == current_) {
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        active_ = false;
        std::lock_guard<std::mutex> lock(g_out_mutex);
        std::cout << "  " << green(symbols().check) << ' ' << current_ << ' '
                  << dim(human_duration(secs)) << '\n';
        return;
    }
    if (active_) {
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        std::lock_guard<std::mutex> lock(g_out_mutex);
        std::cout << "  " << green(symbols().check) << ' ' << current_ << ' '
                  << dim(human_duration(secs)) << '\n';
    }
    current_ = stage;
    started_ = std::chrono::steady_clock::now();
    active_ = true;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << cyan(symbols().play) << ' ' << stage << '\n' << std::flush;
}

void StagePrinter::finish() {
    if (!active_) return;
    if (g_json.load()) {
        active_ = false;
        return;
    }
    if (g_agent.load()) {
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        active_ = false;
        std::lock_guard<std::mutex> lock(g_out_mutex);
        std::cout << "OK    " << current_ << " (" << human_duration(secs) << ")\n";
        return;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    active_ = false;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << green(symbols().check) << ' ' << current_ << ' '
              << dim(human_duration(secs)) << '\n';
}

// ---------------------------------------------------------------------------
// ProgressBar
// ---------------------------------------------------------------------------
ProgressBar::ProgressBar(std::string label) : label_(std::move(label)) {
    init();
    active_ = true;
}

ProgressBar::~ProgressBar() { finish(); }

void ProgressBar::begin(std::int64_t total_bytes) {
    if (g_json.load()) return;
    total_ = total_bytes;
    done_ = 0;
    last_done_ = -1;
    last_time_ = std::chrono::steady_clock::now();
    speed_bps_ = 0.0;
    if (!g_tty.load() || env_set("IRIS_PLAIN")) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << label_ << '\n' << std::flush;
}

void ProgressBar::update(std::int64_t done_bytes) {
    done_ = done_bytes;
    if (g_agent.load()) return; // agent 模式只保留开始/结束两行，不打进度动画
    if (!g_tty.load() || env_set("IRIS_PLAIN")) return;
    const auto t = now_ms();
    if (t - last_render_ms_ < 120) return; // 限流，避免刷屏
    last_render_ms_ = t;

    const int width = 28;
    double ratio = 0.0;
    if (total_ > 0) ratio = std::min(1.0, static_cast<double>(done_) / static_cast<double>(total_));
    const int filled = static_cast<int>(std::lround(ratio * width));

    std::string bar;
    for (int i = 0; i < width; ++i) bar += (i < filled) ? "█" : "░";
    char buf[64];
    if (total_ > 0) {
        std::snprintf(buf, sizeof buf, "%3d%%", static_cast<int>(std::lround(ratio * 100)));
    } else {
        buf[0] = '\0';
    }

    // 速度（指数平滑）与 ETA
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_time_).count();
    if (last_done_ >= 0 && dt > 0.05) {
        const double inst = static_cast<double>(done_ - last_done_) / dt;
        speed_bps_ = speed_bps_ <= 0.0 ? inst : speed_bps_ * 0.7 + inst * 0.3;
        last_done_ = done_;
        last_time_ = now;
    }

    std::string extra;
    if (speed_bps_ > 1.0) {
        char sbuf[64];
        std::snprintf(sbuf, sizeof sbuf, "%s/s", human_size(static_cast<std::int64_t>(speed_bps_)).c_str());
        extra += "  ";
        extra += sbuf;
        if (total_ > 0 && speed_bps_ > 0.0) {
            const double eta = static_cast<double>(total_ - done_) / speed_bps_;
            extra += "  ETA ";
            extra += human_duration(eta);
        }
    }

    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "\r\x1b[2K  " << cyan(bar) << ' ' << buf << ' ' << human_size(done_);
    if (total_ > 0) std::cout << " / " << human_size(total_);
    std::cout << extra << "   " << std::flush;
}

void ProgressBar::finish() {
    if (!active_) return;
    active_ = false;
    if (!g_tty.load() || env_set("IRIS_PLAIN")) return;
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "\r\x1b[2K" << std::flush;
}

} // namespace iris::ui
