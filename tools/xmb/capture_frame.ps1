# Capture deterministic OpenXMB frames for xmb-web parity visual regression.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("root", "settings", "theme", "theme-panel", "all")]
    [string]$Scene,

    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path,
    [string]$BuildDir = "",
    [string]$OutputDir = "",
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$Frames = 0,
    [switch]$BackgroundOnly
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot "build/native-c"
}

$presetsPath = Join-Path $RepoRoot "tools/xmb/scene_presets.json"
$presets = Get-Content -Raw -LiteralPath $presetsPath | ConvertFrom-Json
$defaults = $presets.defaults

if ($Width -le 0) { $Width = [int]$defaults.width }
if ($Height -le 0) { $Height = [int]$defaults.height }
if ($Frames -le 0) { $Frames = [int]$defaults.frames }

$binary = Join-Path $BuildDir "XMS.bin.exe"
if (-not (Test-Path -LiteralPath $binary)) {
    throw "Binary not found: $binary"
}

$shellDir = Join-Path $BuildDir "shell"
$localeDir = Join-Path $BuildDir "locales"

function Invoke-SceneCapture {
    param(
        [string]$SceneName,
        [hashtable]$SceneEnv,
        [string]$Pattern
    )

    if (-not $OutputDir) {
        $captureRoot = Join-Path $RepoRoot "build/visual-regression/captures"
    } else {
        $captureRoot = $OutputDir
    }
    $sceneDir = Join-Path $captureRoot $SceneName
    New-Item -ItemType Directory -Force -Path $sceneDir | Out-Null
    Get-ChildItem -LiteralPath $sceneDir -Filter "*.png" -File |
        Remove-Item -Force

    $configPath = Join-Path $sceneDir "config.json"
    Copy-Item -LiteralPath (Join-Path $RepoRoot "config.json") -Destination $configPath -Force
    $cfg = Get-Content -Raw -LiteralPath $configPath | ConvertFrom-Json
    $cfg.shell.'background-type' = $defaults.background_type
    $cfg.shell.'theme-colour-mode' = $defaults.theme_colour_mode
    $cfg.render.'sample-count' = [int]$defaults.sample_count
    $cfg.render.vsync = $false
    $cfg.render.'max-fps' = [int]$defaults.max_fps
    $cfg.render.'show-fps' = $false
    $cfg.render.'show-mem' = $false
    $cfg.render.'icon-glass-refraction' = [bool]$defaults.icon_glass_refraction
    $cfg | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $configPath -Encoding UTF8

    Remove-Item Env:OPENXMB_INITIAL_CATEGORY -ErrorAction SilentlyContinue
    Remove-Item Env:OPENXMB_INITIAL_SETTINGS_MENU -ErrorAction SilentlyContinue
    Remove-Item Env:OPENXMB_INITIAL_SETTINGS_SELECTION -ErrorAction SilentlyContinue
    Remove-Item Env:OPENXMB_OPEN_INITIAL_SETTINGS_CHOICE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENXMB_HEADLESS_STARTUP -ErrorAction SilentlyContinue
    Remove-Item Env:OPENXMB_FIXED_BOOT_SECONDS -ErrorAction SilentlyContinue

    foreach ($key in $SceneEnv.Keys) {
        Set-Item -Path "Env:$key" -Value $SceneEnv[$key]
    }

    $env:SPDLOG_LEVEL = "warn"
    $env:DREAMRENDER_HEADLESS = "1"
    $env:DREAMRENDER_HEADLESS_WIDTH = "$Width"
    $env:DREAMRENDER_HEADLESS_HEIGHT = "$Height"
    $env:DREAMRENDER_HEADLESS_FRAMES = "$Frames"
    $env:DREAMRENDER_HEADLESS_OUTPUT_DIR = $sceneDir
    $env:DREAMRENDER_HEADLESS_OUTPUT_PATTERN = $Pattern
    $env:XMB_ASSET_DIR = $shellDir
    $env:XMB_LOCALE_DIR = $localeDir
    $env:SDL_AUDIODRIVER = "dummy"
    $env:OPENXMB_CONFIG = $configPath
    $env:OPENXMB_CONFIG_READ_ONLY = "1"
    $env:OPENXMB_FIXED_UNIX_SECONDS = "$([long]$defaults.fixed_unix_seconds)"
    $env:OPENXMB_FIXED_WAVE_SECONDS = "$([double]$defaults.fixed_wave_seconds)"

    $args = @("--width", "$Width", "--height", "$Height", "--no-fullscreen")
    if ($BackgroundOnly) {
        $args += "--background-only"
    }

    Write-Host "Capturing scene '$SceneName' -> $sceneDir"
    & $binary @args
    if ($LASTEXITCODE -ne 0) {
        throw "Capture failed for scene '$SceneName' with exit code $LASTEXITCODE"
    }

    $pngs = Get-ChildItem -LiteralPath $sceneDir -Filter "*.png" | Sort-Object Name
    if ($pngs.Count -eq 0) {
        throw "No PNG output for scene '$SceneName'"
    }
    $last = $pngs[-1]
    $canonical = Join-Path $sceneDir "$SceneName.png"
    if ($last.FullName -ne $canonical) {
        Copy-Item -LiteralPath $last.FullName -Destination $canonical -Force
    }
    Write-Host "  canonical: $canonical ($($last.Length) bytes)"
    return $canonical
}

$sceneNames = if ($Scene -eq "all") {
    @($presets.scenes.PSObject.Properties.Name)
} else {
    @($Scene)
}

$results = @{}
foreach ($name in $sceneNames) {
    $scenePreset = $presets.scenes.$name
    if (-not $scenePreset) {
        throw "Unknown scene preset: $name"
    }
    $envMap = @{}
    foreach ($prop in $scenePreset.env.PSObject.Properties) {
        $envMap[$prop.Name] = [string]$prop.Value
    }
    $results[$name] = Invoke-SceneCapture -SceneName $name -SceneEnv $envMap -Pattern $scenePreset.pattern
}

return $results
