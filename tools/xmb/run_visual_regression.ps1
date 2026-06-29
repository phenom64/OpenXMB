# Run pixel-diff visual regression for all registered parity scenes.
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path,
    [string]$BuildDir = "",
    [string]$ReferenceDir = "",
    [string]$CaptureDir = "",
    [string]$ReportDir = "",
    [switch]$SkipCapture,
    [switch]$Heatmap
)

$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    $BuildDir = Join-Path $RepoRoot "build/native"
}
if (-not $ReferenceDir) {
    $ReferenceDir = Join-Path $RepoRoot "tests/xmb/reference_frames"
}
if (-not $CaptureDir) {
    $CaptureDir = Join-Path $RepoRoot "build/visual-regression/captures"
}
if (-not $ReportDir) {
    $ReportDir = Join-Path $RepoRoot "build/visual-regression/reports"
}

$thresholds = Join-Path $RepoRoot "assets/xmb/manifests/visual-thresholds.json"
$comparePy = Join-Path $RepoRoot "tools/xmb/compare_frames.py"
$installRefs = Join-Path $RepoRoot "tools/xmb/install_reference_frames.ps1"
$capturePs1 = Join-Path $RepoRoot "tools/xmb/capture_frame.ps1"

if (-not (Test-Path -LiteralPath (Join-Path $ReferenceDir "root.png"))) {
    Write-Host "Installing reference frames from workflow goldens..."
    & $installRefs -RepoRoot $RepoRoot -OutputDir $ReferenceDir
}

if (-not $SkipCapture) {
    Write-Host "Capturing native frames for all scenes..."
    & $capturePs1 -Scene all -RepoRoot $RepoRoot -BuildDir $BuildDir -OutputDir $CaptureDir
}

New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null

$presets = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot "tools/xmb/scene_presets.json") | ConvertFrom-Json
$sceneNames = @($presets.scenes.PSObject.Properties.Name)

$summary = @{
    schema = 1
    passed = $true
    scenes = @{}
}

$failures = 0
foreach ($name in $sceneNames) {
    $reference = Join-Path $ReferenceDir "$name.png"
    $actual = Join-Path $CaptureDir "$name/$name.png"
    $report = Join-Path $ReportDir "$name-comparison.json"
    $heatmapPath = Join-Path $ReportDir "$name-heatmap.png"

    if (-not (Test-Path -LiteralPath $reference)) {
        throw "Missing reference frame: $reference"
    }
    if (-not (Test-Path -LiteralPath $actual)) {
        throw "Missing capture frame: $actual (run capture first)"
    }

    $compareArgs = @(
        $comparePy,
        "--reference", $reference,
        "--actual", $actual,
        "--thresholds", $thresholds,
        "--scene", $name,
        "--output", $report
    )
    if ($Heatmap) {
        $compareArgs += @("--heatmap", $heatmapPath)
    }

    Write-Host "Comparing scene '$name'..."
    & python @compareArgs
    $exit = $LASTEXITCODE
    if ($exit -ne 0) {
        $summary.passed = $false
        $failures++
    }

    $reportJson = Get-Content -Raw -LiteralPath $report | ConvertFrom-Json
    $summary.scenes[$name] = @{
        passed = $reportJson.passed
        mae = $reportJson.metrics.mae
        rmse = $reportJson.metrics.rmse
        report = $report
    }
    if ($Heatmap) {
        $summary.scenes[$name].heatmap = $heatmapPath
    }
}

$summaryPath = Join-Path $ReportDir "summary.json"
($summary | ConvertTo-Json -Depth 8) + "`n" | Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-Host "Wrote $summaryPath"

if ($failures -gt 0) {
    Write-Error "Visual regression failed for $failures scene(s)"
    exit 1
}

Write-Host "All $($sceneNames.Count) scenes passed visual regression."
exit 0