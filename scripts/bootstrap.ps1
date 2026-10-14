# Windows 一键构建脚本：daisy (C++ migration of Centurion)
# 用法：powershell -ExecutionPolicy Bypass -File scripts/bootstrap.ps1 [-ConfigureOnly] [-SkipTests]
param(
    [switch]$ConfigureOnly,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Write-Host "=== daisy bootstrap ==="

# 1. 检查工具链
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Write-Host "[ERROR] 未找到 cmake。请先安装 CMake >= 3.20 并加入 PATH。" -ForegroundColor Red
    exit 1
}
Write-Host "cmake: $((cmake --version | Select-Object -First 1))"

# 2. 配置
Write-Host "`n[1/3] 配置 (cmake -S . -B build)"
& cmake -S . -B build -DDAISY_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($ConfigureOnly) {
    Write-Host "配置完成（跳过构建）。" -ForegroundColor Green
    exit 0
}

# 3. 构建
Write-Host "`n[2/3] 构建"
& cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# 4. 测试
if (-not $SkipTests) {
    Write-Host "`n[3/3] 测试 (ctest)"
    & ctest --test-dir build -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "`n完成。可执行文件: $root\build\bin\daisy.exe" -ForegroundColor Green
