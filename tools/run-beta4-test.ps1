param([switch]$FirstTimeSetup, [switch]$Windowed, [string]$ProfileDir = "")
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$manifestPath = Join-Path $repoRoot "build/beta4-user-test.json"
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "No separate Beta 4 test snapshot has been prepared." }
$snapshot = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$arguments = @{ BuildDir = $snapshot.build_directory }
if ($FirstTimeSetup) { $arguments.FirstTimeSetup = $true }
if ($Windowed) { $arguments.Windowed = $true }
if ($ProfileDir) { $arguments.ProfileDir = $ProfileDir }
Write-Host "Beta 4 test snapshot: $($snapshot.name)"
& (Join-Path $PSScriptRoot "run-beta4-preview.ps1") @arguments
