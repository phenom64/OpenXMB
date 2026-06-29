# Headless background capture for xmb-web parity visual audits.
# Requires a built OpenXMB binary and compat pack (windows-native preset).
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path,
    [string]$BuildDir = "",
    [string]$CaptureDir = "",
    [int]$Width = 1280,
    [int]$Height = 720,
    [int]$Frames = 4,
    [string]$Pattern = "original-{:05d}.png",
    [double]$FixedWaveSeconds = 4.0,
    [long]$FixedUnixSeconds = 1781524800  # 2026-06-15 12:00:00 UTC — stable month gradient
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot "build/native-c"
}
if (-not $CaptureDir) {
    $CaptureDir = Join-Path $RepoRoot "build/audit-captures/native-original-explicit"
}

$binary = Join-Path $BuildDir "XMS.bin.exe"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Binary not found: $binary (run cmake --build --preset windows-native first)"
}

$shellDir = Join-Path $BuildDir "shell"
$localeDir = Join-Path $BuildDir "locales"
if (-not (Test-Path -LiteralPath (Join-Path $shellDir "compat/xmb-ui-compat/manifest.json"))) {
    Write-Warning "Compat pack missing under $shellDir — captures will use Classic wave fallback"
}

New-Item -ItemType Directory -Force -Path $CaptureDir | Out-Null

$configPath = Join-Path $CaptureDir "config.json"
Copy-Item -LiteralPath (Join-Path $RepoRoot "config.json") -Destination $configPath -Force

$cfg = Get-Content -Raw -LiteralPath $configPath | ConvertFrom-Json
$cfg.shell.'background-type' = 'original'
$cfg.shell.'theme-colour-mode' = 'original'
$cfg.render.'sample-count' = 1
$cfg.render.vsync = $false
$cfg.render.'max-fps' = 30
$cfg.render.'show-fps' = $false
$cfg.render.'show-mem' = $false
$cfg.render.'icon-glass-refraction' = $true
$cfg | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $configPath -Encoding UTF8

$env:SPDLOG_LEVEL = "debug"
$env:DREAMRENDER_HEADLESS = "1"
$env:DREAMRENDER_HEADLESS_WIDTH = "$Width"
$env:DREAMRENDER_HEADLESS_HEIGHT = "$Height"
$env:DREAMRENDER_HEADLESS_FRAMES = "$Frames"
$env:DREAMRENDER_HEADLESS_OUTPUT_DIR = $CaptureDir
$env:DREAMRENDER_HEADLESS_OUTPUT_PATTERN = $Pattern
$env:XMB_ASSET_DIR = $shellDir
$env:XMB_LOCALE_DIR = $localeDir
$env:SDL_AUDIODRIVER = "dummy"
$env:OPENXMB_CONFIG = $configPath
$env:OPENXMB_CONFIG_READ_ONLY = "1"
$env:OPENXMB_FIXED_UNIX_SECONDS = "$FixedUnixSeconds"
$env:OPENXMB_FIXED_WAVE_SECONDS = "$FixedWaveSeconds"

Write-Host "Capturing $Frames frame(s) to $CaptureDir"
& $binary --width $Width --height $Height --no-fullscreen --background-only
if ($LASTEXITCODE -ne 0) {
    throw "Capture failed with exit code $LASTEXITCODE"
}

Get-ChildItem -LiteralPath $CaptureDir -Filter "*.png" | ForEach-Object {
    Write-Host "  $($_.Name) ($($_.Length) bytes)"
}