#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace iris::ui {

// ---------------------------------------------------------------------------
// 终端呈现层（自写，不引第三方库）。
//
// 参考 Centurion（Spectre.Console）的语义色与符号表，以及 Ollama 的进度展示：
//   - 语义色：error 红粗、warning 金、success 绿、accent 青、dim 灰；
//   - 符号：✓ ✗ → ▶ ● ○ ·，非 Unicode 终端自动退回 ASCII；
//   - 进度：spinner（转圈）+ 字节/速度/ETA 进度条；
//   - 所有装饰在非 TTY、NO_COLOR、IRIS_PLAIN=1 时自动关闭，保持可管道处理。
// ---------------------------------------------------------------------------

struct Symbols {
    std::string check = "✓";
    std::string cross = "✗";
    std::string arrow = "→";
    std::string play = "▶";
    std::string dot = "●";
    std::string ring = "○";
    std::string mid_dot = "·";
    bool unicode = true;
};

/// 初始化（Windows 上开启 ANSI，并把 stdout/stderr 接管为宽字符控制台输出）。
/// 重复调用无副作用。
void init();

/// 直接写一段 UTF-8 文本到控制台（Windows 走 WriteConsoleW，重定向时写字节）。
void write_utf8(const std::string& text, bool to_stderr = false);

/// Agent 模式（对应 C# 的 --agent）：面向 LLM/脚本的纯文本输出。
/// 关闭颜色与装饰，行首加固定级别前缀（INFO/OK/WARN/ERROR/STEP），
/// 关闭 spinner 与进度条动画，符号退化为 ASCII。必须在 init() 之前调用。
void set_agent_mode(bool on);
[[nodiscard]] bool agent_mode();

/// 行首前缀（agent 模式使用；非 agent 模式返回空串）。
std::string agent_prefix(const std::string& level);

/// 是否启用颜色（TTY 且未设置 NO_COLOR / IRIS_PLAIN）。
bool colors_enabled();

/// 是否使用 Unicode 符号（未设置 IRIS_ASCII 且输出编码支持）。
bool unicode_enabled();

/// 当前符号表。
const Symbols& symbols();

// ---- 着色（关闭时原样返回）------------------------------------------------
std::string bold(const std::string& s);
std::string dim(const std::string& s);
std::string green(const std::string& s);
std::string red(const std::string& s);
std::string yellow(const std::string& s);
std::string cyan(const std::string& s);
std::string magenta(const std::string& s);

// ---- 结构元素 ------------------------------------------------------------
/// 顶部 banner：标题 + 副标题（版本/命令）。
void banner(const std::string& title, const std::string& subtitle);

/// 小节标题："▌ 转写"
void section(const std::string& title);

/// 带符号的状态行：✓ 完成 / ✗ 失败 / → 步骤 / �​· 提示
void success(const std::string& message);
void failure(const std::string& message);
void warning(const std::string& message);
void step(const std::string& message);
void note(const std::string& message);

/// 结束语：一行汇总（成功绿色 / 失败红色）。
void finish(bool ok, const std::string& message);

/// 两列表格（键值），逐行打印。
void kv_table(const std::vector<std::pair<std::string, std::string>>& rows);

/// 多列表格：自动按显示宽度对齐（CJK 按 2 列宽）。
void table(const std::vector<std::string>& headers,
           const std::vector<std::vector<std::string>>& rows);

/// SGR 解析结果（与平台无关，便于单测）。
struct SgrState {
    int fg = -1;    ///< 前景色 0..7（-1 = 用终端默认）
    bool bright = false;
    bool default_fg = true;
};

/// 解析 SGR 参数串（如 "1;31"、"0"、"90"），在 base 基础上应用。
SgrState parse_sgr(const std::string& params, SgrState base);

/// 字符串的终端显示宽度（ASCII 1、CJK/全角 2）。
std::size_t display_width(const std::string& s);

/// 右补空格到指定显示宽度。
std::string pad_right(const std::string& s, std::size_t width);

/// 人类可读体积（1.2 GB / 940 MB / 512 KB）。
std::string human_size(std::int64_t bytes);

/// 人类可读时长（1m 20s / 42s）。
std::string human_duration(double seconds);

// ---- 进度 ----------------------------------------------------------------
/// 转圈提示（非 TTY 时只打印一行开始/结束）。
class Spinner {
public:
    explicit Spinner(std::string label);
    ~Spinner();
    Spinner(const Spinner&) = delete;
    Spinner& operator=(const Spinner&) = delete;

    /// 更新提示文字（可选）。
    void update(const std::string& label);
    /// 结束并打印最终状态。
    void stop(bool ok, const std::string& final_text);

private:
    std::string label_;
    bool active_ = false;
};

/// 流水线阶段打印器：每进入新阶段先把上一阶段收尾成 "✓ 阶段名 (耗时)"。
class StagePrinter {
public:
    void begin(const std::string& stage);
    void finish();

private:
    std::string current_;
    std::chrono::steady_clock::time_point started_{};
    bool active_ = false;
};

/// 进度条：已知总量时显示百分比、字节与速度；未知总量时显示已下载字节。
class ProgressBar {
public:
    explicit ProgressBar(std::string label);
    ~ProgressBar();
    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;

    /// 设定总量（可为 0 表示未知）。
    void begin(std::int64_t total_bytes);
    /// 刷新当前进度。
    void update(std::int64_t done_bytes);
    /// 结束（打印换行）。
    void finish();

private:
    std::string label_;
    std::int64_t total_ = 0;
    std::int64_t done_ = 0;
    bool active_ = false;
    std::int64_t last_render_ms_ = 0;
    std::int64_t last_done_ = -1;
    std::chrono::steady_clock::time_point last_time_{};
    double speed_bps_ = 0.0;
};

} // namespace iris::ui
