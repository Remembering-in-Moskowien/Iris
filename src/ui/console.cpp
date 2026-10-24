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

const Symbols& symbols_impl() {
    static Symbols sym = [] {
        Symbols s;
        if (!g_unicode.load()) {
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
    }();
    return sym;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

void init() {
    std::call_once(g_init_once, [] {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        if (HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE); h != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(h, &mode)) {
                SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }
        g_tty.store(_isatty(_fileno(stdout)) != 0);
#else
        g_tty.store(::isatty(STDOUT_FILENO) != 0);
#endif
        g_colors.store(g_tty.load() && !env_set("NO_COLOR") && !env_set("IRIS_PLAIN"));
        g_unicode.store(!env_set("IRIS_ASCII"));
    });
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
    return symbols_impl();
}

std::string bold(const std::string& s) { return paint("1", s); }
std::string dim(const std::string& s) { return paint("90", s); }
std::string green(const std::string& s) { return paint("32", s); }
std::string red(const std::string& s) { return paint("1;31", s); }
std::string yellow(const std::string& s) { return paint("33", s); }
std::string cyan(const std::string& s) { return paint("36", s); }
std::string magenta(const std::string& s) { return paint("1;35", s); }

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
    std::lock_guard<std::mutex> lock(g_out_mutex);
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
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << '\n' << cyan("▌ ") << bold(title) << '\n';
}

void success(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << green(symbols().check) << ' ' << message << '\n';
}

void failure(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cerr << "  " << red(symbols().cross) << ' ' << message << '\n';
}

void warning(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << yellow(symbols().dot) << ' ' << message << '\n';
}

void step(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << cyan(symbols().arrow) << ' ' << message << '\n';
}

void note(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << "  " << dim(symbols().mid_dot) << ' ' << dim(message) << '\n';
}

void finish(bool ok, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::cout << '\n' << (ok ? green(symbols().check + " " + message)
                             : red(symbols().cross + " " + message))
              << '\n';
}

void kv_table(const std::vector<std::pair<std::string, std::string>>& rows) {
    std::size_t key_width = 0;
    for (const auto& [k, _] : rows) key_width = std::max(key_width, display_width(k));
    std::lock_guard<std::mutex> lock(g_out_mutex);
    for (const auto& [k, v] : rows) {
        std::cout << "  " << dim(pad_right(k, key_width)) << "  " << v << '\n';
    }
}

void table(const std::vector<std::string>& headers,
           const std::vector<std::vector<std::string>>& rows) {
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
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_tty.load()) std::cout << "\r\x1b[2K";
    std::cout << "  " << (ok ? green(symbols().check) : red(symbols().cross)) << ' '
              << (final_text.empty() ? label_ : final_text) << '\n'
              << std::flush;
}

// ---------------------------------------------------------------------------
// StagePrinter
// ---------------------------------------------------------------------------
void StagePrinter::begin(const std::string& stage) {
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
