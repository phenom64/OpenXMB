param(
    [switch]$FirstTimeSetup,
    [switch]$Windowed,
    [string]$BuildDir = "",
    [string]$ProfileDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot "build/native-c" }
if (-not $ProfileDir) { $ProfileDir = Join-Path $repoRoot "build/beta4-preview-profile" }
$binary = Join-Path $BuildDir "XMS.bin.exe"
if (-not (Test-Path -LiteralPath $binary)) { throw "Build OpenXMB first: $binary" }
New-Item -ItemType Directory -Force -Path $ProfileDir | Out-Null
$configPath = Join-Path $ProfileDir "config.json"
if (-not (Test-Path -LiteralPath $configPath)) {
    $config = Get-Content -LiteralPath (Join-Path $repoRoot "config.json") -Raw | ConvertFrom-Json
    $config | Add-Member -NotePropertyName preferences -NotePropertyValue ([pscustomobject]@{ 'setup.completed' = 'false' }) -Force
    $config | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $configPath -Encoding UTF8
}
$media = Join-Path $ProfileDir "media"
foreach ($category in @("Music", "Pictures", "Videos")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $media $category) | Out-Null
}
$values = @{
    OPENXMB_CONFIG = $configPath
    OPENXMB_DATA_DIR = (Join-Path $ProfileDir "data")
    XMB_ASSET_DIR = (Join-Path $BuildDir "shell")
    XMB_LOCALE_DIR = (Join-Path $BuildDir "locales")
}
$cleared = @("AUROREENGINE_HEADLESS", "OPENXMB_CONFIG_READ_ONLY", "OPENXMB_SETUP_PAGE",
    "OPENXMB_SETUP_EDIT", "OPENXMB_FORCE_SETUP", "OPENXMB_FIXED_SETUP_SECONDS",
    "OPENXMB_FIXED_UNIX_SECONDS", "OPENXMB_FIXED_BOOT_SECONDS", "OPENXMB_FIXED_WAVE_SECONDS",
    "OPENXMB_HEADLESS_STARTUP", "OPENXMB_HARNESS_ACTIONS", "SDL_AUDIODRIVER")
$previous = @{}
foreach ($name in @($values.Keys) + $cleared) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}
try {
    foreach ($name in $cleared) { [Environment]::SetEnvironmentVariable($name, $null, "Process") }
    foreach ($name in $values.Keys) { [Environment]::SetEnvironmentVariable($name, $values[$name], "Process") }
    if ($FirstTimeSetup) { [Environment]::SetEnvironmentVariable("OPENXMB_FORCE_SETUP", "1", "Process") }
    Write-Host "OpenXMB Beta 4 preview: $repoRoot"
    Write-Host "Isolated profile: $ProfileDir"
    $launchArguments = @('--force-dir', $media)
    if ($Windowed) { $launchArguments += @('--no-fullscreen', '--width', '1280', '--height', '720') }
    & $binary @launchArguments
    if ($LASTEXITCODE -ne 0) { throw "OpenXMB exited with code $LASTEXITCODE" }
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], "Process") }
}
