#pragma once

namespace iris::cli {

/// CLI 命令分发入口（对应 C# Program/CommandApp + Spectre.Console）。
/// 返回进程退出码：0 成功，非 0 失败。
int run(int argc, char** argv);

} // namespace iris::cli
