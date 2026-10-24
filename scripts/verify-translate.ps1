# ============================================================================
# 翻译链路校验（非 LLM 机器翻译：M2M100 / OPUS-MT）
#
# 用法（仓库根目录）：
#   powershell -ExecutionPolicy Bypass -File scripts/verify-translate.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/verify-translate.ps1 -Ir .cache/verify/dub.iris.json -To zh
#
# 做的事：IR（或媒体 → ASR）→ 逐句翻译 → 保存 IR → 校验译文（中文必须含汉字）。
# 需要的模型（二选一）：
#   models/translation/m2m100-418m-q8_0.gguf     （100 语言，任意方向）
#   models/translation/opus-mt-en-zh-q8_0.gguf   （单方向，需自行转换）
# ============================================================================
param(
    [string]$Media   = "models/jfk.wav",
    [string]$Ir      = "",
    [string]$Model   = "models/translation/m2m100-418m-q8_0.gguf",
    [string]$Engine  = "auto",
    [string]$From    = "en",
    [string]$To      = "zh",
    [int]$Beam       = 5,
    [string]$Out     = ".cache/verify/translate.$To.iris.json",
    [string]$AsrEngine = "whisper",
    [string]$AsrModel  = "models/ggml/ggml-base.bin",
    [string]$Exe     = "build/bin/Release/iris.exe"
)

$ErrorActionPreference = 'Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)

function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; exit 1 }
function Need($file) { if (-not (Test-Path -Path $file)) { Fail "缺少文件：$file" } }

Need $Exe
Need $Model

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null

$irisArgs = @("translate")
if ($Ir) {
    Need $Ir
    $irisArgs += @($Ir)
} else {
    Need $Media
    if ($AsrEngine -ne "none") { Need $AsrModel }
    $irisArgs += @($Media, "--asr-engine", $AsrEngine, "--asr-model", $AsrModel)
}
$irisArgs += @("-m", $Model, "--engine", $Engine, "--from", $From, "--to", $To,
               "--beam", $Beam, "--out", $Out)

Write-Host "==> iris $($irisArgs -join ' ')" -ForegroundColor Cyan
$lines = & $Exe @irisArgs 2>$null
if ($LASTEXITCODE -ne 0) { Fail "translate 退出码 $LASTEXITCODE" }
Need $Out

Write-Host "==> 译文（stdout）" -ForegroundColor Cyan
$lines | Where-Object { $_ -and $_ -notmatch '^\[' } | ForEach-Object { "    $_" }

Write-Host "==> IR 校验" -ForegroundColor Cyan
& $Exe validate $Out
if ($LASTEXITCODE -ne 0) { Fail "IR 校验失败" }

# 译文质量的最低保证：目标语言是中文时，译文里必须真的有汉字
if ($To -like 'zh*') {
    $json = [IO.File]::ReadAllText((Resolve-Path $Out), [Text.UTF8Encoding]::new($false))
    $hits = [regex]::Matches($json, '[\u4e00-\u9fff]').Count
    Write-Host "==> 汉字数量（IR 全文）：$hits"
    if ($hits -lt 2) { Fail "译文里没有汉字，翻译链路没生效" }
}

Write-Host ""
Write-Host "[OK] 翻译链路通畅：$Out（$From → $To，引擎 $Engine）" -ForegroundColor Green
