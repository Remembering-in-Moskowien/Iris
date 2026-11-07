param(
    [Parameter(Mandatory = $true)]
    [string]$Media,
    [string]$OutputDirectory = ".cache\iris-media",
    [string]$Language = "en",
    [ValidateSet("whisper", "qwen3-asr", "whisper-ct2")]
    [string]$AsrEngine = "qwen3-asr",
    [string]$AsrModel = "",
    [switch]$Align,
    [string]$AlignModel = "models\aligner\qwen3-forced-aligner-0.6b-q4_k.gguf",
    [string]$Exe = "build\bin\Release\iris.exe",
    [switch]$ReuseIr
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

function Get-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $repoRoot $Path
}

function Invoke-Iris([string[]]$Arguments, [string]$Action) {
    & $exePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "iris $Action failed with exit code $LASTEXITCODE"
    }
}

$mediaPath = (Resolve-Path -LiteralPath $Media -ErrorAction Stop).Path
$exePath = Get-RepoPath $Exe
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "Iris executable not found: $exePath"
}

if (-not $ReuseIr) {
    if (-not $AsrModel) {
        switch ($AsrEngine) {
            "whisper" { $AsrModel = "models\ggml\ggml-base.bin" }
            "qwen3-asr" { $AsrModel = "models\qwen3\qwen3-asr-0.6b-q4_k.gguf" }
            "whisper-ct2" { $AsrModel = "models\ct2\whisper-base" }
        }
    }
    $modelPath = Get-RepoPath $AsrModel
    if (-not (Test-Path -LiteralPath $modelPath)) {
        throw "ASR model not found: $modelPath"
    }
}

$outputPath = Get-RepoPath $OutputDirectory
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$baseName = [IO.Path]::GetFileNameWithoutExtension($mediaPath)
$irPath = Join-Path $outputPath "$baseName.iris.json"
$assPath = Join-Path $outputPath "$baseName.ass"
$srtPath = Join-Path $outputPath "$baseName.srt"

if ($ReuseIr) {
    if (-not (Test-Path -LiteralPath $irPath -PathType Leaf)) {
        throw "IR file not found for reuse: $irPath"
    }
    Write-Host "==> Reusing IR: $irPath" -ForegroundColor Cyan
} else {
    Write-Host "==> ASR ($AsrEngine, $Language)" -ForegroundColor Cyan
    Invoke-Iris @("asr", $mediaPath, "--engine", $AsrEngine, "--model", $modelPath,
        "--language", $Language, "--output", $irPath) "asr"
}
Invoke-Iris @("validate", $irPath) "validate"

if ($Align) {
    $alignModelPath = Get-RepoPath $AlignModel
    if (-not (Test-Path -LiteralPath $alignModelPath -PathType Leaf)) {
        throw "Aligner model not found: $alignModelPath (install with iris models install qwen3-forced-aligner)"
    }
    $alignedIrPath = Join-Path $outputPath "$baseName.aligned.iris.json"
    Write-Host "==> Forced alignment" -ForegroundColor Cyan
    & $exePath align $mediaPath --ir $irPath --model $alignModelPath --language $Language `
        --pad 2 --apply-sentence-timings --out $alignedIrPath
    if ($LASTEXITCODE -eq 2) {
        Write-Warning "Some cues could not be reliably aligned; their original subtitle timings were kept."
    } elseif ($LASTEXITCODE -ne 0) {
        throw "iris align failed with exit code $LASTEXITCODE"
    }
    $irPath = $alignedIrPath
    Invoke-Iris @("validate", $irPath) "validate aligned IR"
}

$ir = Get-Content -LiteralPath $irPath -Raw -Encoding UTF8 | ConvertFrom-Json
$track = $ir.state.tracks | Where-Object { $_.type -eq "transcribe" } | Select-Object -First 1
if (-not $track -or @($track.sentences).Count -eq 0) {
    throw "ASR produced no transcribe sentences: $irPath"
}
$invalidTimes = @($track.sentences | Where-Object { $_.startMs -lt 0 -or $_.endMs -le $_.startMs }).Count
if ($invalidTimes -gt 0) {
    throw "ASR produced $invalidTimes sentence(s) with invalid timestamps: $irPath"
}

Write-Host "==> ASS" -ForegroundColor Cyan
Invoke-Iris @("build", $irPath, "--format", "ass", "--output", $assPath) "build ASS"
Write-Host "==> SRT" -ForegroundColor Cyan
Invoke-Iris @("build", $irPath, "--format", "srt", "--output", $srtPath) "build SRT"

foreach ($file in @($irPath, $assPath, $srtPath)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
        (Get-Item -LiteralPath $file).Length -eq 0) {
        throw "Expected output missing or empty: $file"
    }
}

Write-Host ""
Write-Host "[OK] Subtitles generated: $(@($track.sentences).Count) cues" -ForegroundColor Green
Write-Host "     IR : $irPath"
Write-Host "     ASS: $assPath"
Write-Host "     SRT: $srtPath"
