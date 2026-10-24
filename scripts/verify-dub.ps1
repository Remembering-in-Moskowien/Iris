# ============================================================================
# dub 链路端到端校验（qwen3-tts）
#
# 用法（仓库根目录）：
#   powershell -ExecutionPolicy Bypass -File scripts/verify-dub.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/verify-dub.ps1 -Media media.mp4 -Language zh
#
# 做的事：媒体 → (asr) → (emotion) → 逐句 TTS → 配音 WAV，然后校验产物
#         （时长 / 采样率 / 音量），并用 whisper 回听配音结果。
# 需要的模型：
#   models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf
#   models/qwen3-tts/qwen3-tts-tokenizer-12hz.gguf
# ============================================================================
param(
    [string]$Media    = "models/jfk.wav",
    [string]$Ref      = "models/jfk.wav",
    [string]$RefText  = "And so my fellow Americans, ask not what your country can do for you, ask what you can do for your country.",
    [string]$Language = "en",
    [string]$AsrEngine = "whisper",
    [string]$AsrModel  = "models/ggml/ggml-base.bin",
    [string]$EmotionModel = "models/emotion/wav2vec2-emotion-fp32.onnx",
    [string]$TtsModel = "models/qwen3-tts/qwen3-tts-12hz-0.6b-base-q8_0.gguf",
    [string]$TtsCodec = "models/qwen3-tts/qwen3-tts-tokenizer-12hz.gguf",
    [string]$Out      = ".cache/verify/dub.wav",
    [string]$IrOut    = ".cache/verify/dub.iris.json",
    [string]$Exe      = "build/bin/Release/iris.exe"
)

$ErrorActionPreference = 'Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)

function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; exit 1 }
function Need($file) { if (-not (Test-Path -Path $file)) { Fail "缺少文件：$file" } }

Need $Exe
Need $Media
Need $TtsModel
Need $TtsCodec
Need $Ref
if ($AsrEngine -ne "none") { Need $AsrModel }

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null

$irisArgs = @(
    "dub", $Media,
    "--engine", "qwen3-tts",
    "--model", $TtsModel,
    "--codec", $TtsCodec,
    "--language", $Language,
    "--ref", $Ref,
    "--ref-text", $RefText,
    "--out", $Out,
    "--ir-out", $IrOut
)
if ($AsrEngine -ne "none") { $irisArgs += @("--asr-engine", $AsrEngine, "--asr-model", $AsrModel) }
if ($EmotionModel -and (Test-Path -Path $EmotionModel)) { $irisArgs += @("--emotion-model", $EmotionModel) }

Write-Host "==> iris $($irisArgs -join ' ')" -ForegroundColor Cyan
& $Exe @irisArgs
if ($LASTEXITCODE -ne 0) { Fail "dub 退出码 $LASTEXITCODE" }
Need $Out

Write-Host "==> 产物校验" -ForegroundColor Cyan
$probe = & ffprobe -v error -show_entries "stream=sample_rate,channels,codec_name" `
    -show_entries "format=duration" -of default=noprint_wrappers=1 $Out 2>&1
$probe | ForEach-Object { "    $_" }
$duration = [double](& ffprobe -v error -show_entries format=duration `
    -of "default=noprint_wrappers=1:nokey=1" $Out)
if ($duration -le 0) { Fail "配音时长为 0" }

$levels = & cmd /c "ffmpeg -v info -i `"$Out`" -af volumedetect -f null NUL 2>&1" |
    Select-String 'mean_volume|max_volume'
$levels | ForEach-Object { "    $_" }
if (-not $levels) { Fail "读不到音量（音频可能是静音）" }

# 句间停顿：多句时至少要有一次 >= gap/2 的静音（否则说明句子首尾相接）
$gaps = & cmd /c "ffmpeg -v info -i `"$Out`" -af silencedetect=noise=-45dB:d=0.08 -f null NUL 2>&1" |
    Select-String 'silence_duration'
$cueCount = 1
if (Test-Path -Path $IrOut) {
    $track = (Get-Content $IrOut -Raw | ConvertFrom-Json).state.tracks |
        Where-Object { $_.type -eq 'transcribe' } | Select-Object -First 1
    if ($track) { $cueCount = @($track.sentences).Count }
}
if ($cueCount -gt 1) {
    if (-not $gaps) { Fail "检测不到句间停顿（句子首尾相接）" }
    $gaps | ForEach-Object { "    $_" }
}

Write-Host "==> IR 校验" -ForegroundColor Cyan
& $Exe validate $IrOut
if ($LASTEXITCODE -ne 0) { Fail "IR 校验失败" }

if ($AsrEngine -ne "none") {
    Write-Host "==> 回听：whisper 转写配音结果" -ForegroundColor Cyan
    $backIr = [IO.Path]::ChangeExtension($Out, ".asr.json")
    & $Exe asr $Out --engine whisper --model $AsrModel --language $Language --output $backIr | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "回听转写失败" }
    (Get-Content $backIr -Raw | ConvertFrom-Json).state.tracks |
        ForEach-Object { $_.sentences } |
        ForEach-Object { "    " + $_.text }
}

Write-Host ""
Write-Host "[OK] dub 链路通畅：$Out（$([math]::Round($duration,2))s）" -ForegroundColor Green
