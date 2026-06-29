# Install xmb-web golden reference PNGs for visual regression (local-only).
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path,
    [string]$GoldenDir = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $GoldenDir) {
    $GoldenDir = Join-Path $RepoRoot ".workflow/rsx-26-xmb-web-parity-rebuild/results/goldens"
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $RepoRoot "tests/xmb/reference_frames"
}

$mapping = @{
    "root"         = "xmb-web-root-1920x1080-june-noon.png"
    "settings"     = "xmb-web-settings-1920x1080-june-noon.png"
    "theme"        = "xmb-web-theme-1920x1080-june-noon.png"
    "theme-panel"  = "xmb-web-theme-panel-1920x1080-june-noon.png"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$manifest = @{
    schema = 1
    source = "xmb-web goldens from .workflow/rsx-26-xmb-web-parity-rebuild/results/goldens"
    scenes = @{}
}

foreach ($scene in $mapping.Keys) {
    $sourceName = $mapping[$scene]
    $sourcePath = Join-Path $GoldenDir $sourceName
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Missing golden reference: $sourcePath"
    }
    $destPath = Join-Path $OutputDir "$scene.png"
    Copy-Item -LiteralPath $sourcePath -Destination $destPath -Force
    $hash = (Get-FileHash -LiteralPath $destPath -Algorithm SHA256).Hash.ToLower()
    $manifest.scenes[$scene] = @{
        file = "$scene.png"
        sha256 = $hash
        source_golden = $sourceName
    }
    Write-Host "Installed $scene -> $destPath ($hash)"
}

$manifestPath = Join-Path $OutputDir "manifest.json"
($manifest | ConvertTo-Json -Depth 6) + "`n" | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Wrote $manifestPath"