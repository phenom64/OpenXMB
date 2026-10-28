$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$llvm = 'C:\Program Files\LLVM\bin'
if (!(Test-Path "$llvm/clang-scan-deps.exe") -or !((& "$llvm/clang++.exe" --version) -match '20.1.8')) {
    $installer = Join-Path $env:RUNNER_TEMP 'LLVM-20.1.8-win64.exe'
    Invoke-WebRequest 'https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/LLVM-20.1.8-win64.exe' -OutFile $installer
    if ((Get-FileHash $installer -Algorithm SHA256).Hash.ToLower() -ne '3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b') { throw 'LLVM checksum mismatch' }
    $process = Start-Process $installer -ArgumentList '/S' -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw 'LLVM installation failed' }
}
"$llvm" | Out-File $env:GITHUB_PATH -Append -Encoding utf8
"CC=$llvm\clang.exe" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
"CXX=$llvm\clang++.exe" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
$vcpkg = Join-Path $env:RUNNER_TEMP 'openxmb-vcpkg'
if (!(Test-Path "$vcpkg/.git")) { git clone --filter=blob:none https://github.com/microsoft/vcpkg.git $vcpkg; if ($LASTEXITCODE) { throw 'vcpkg clone failed' } }
git -C $vcpkg checkout ef7dbf94b9198bc58f45951adcf1f041fcbc5ea0
if ($LASTEXITCODE) { throw 'vcpkg checkout failed' }
& "$vcpkg/bootstrap-vcpkg.bat" -disableMetrics
if ($LASTEXITCODE) { throw 'vcpkg bootstrap failed' }
$installed = Join-Path $root 'build/vcpkg_installed'
& "$vcpkg/vcpkg.exe" install --triplet x64-windows "--overlay-triplets=$root/scripts/ci/triplets" "--x-manifest-root=$root" "--x-install-root=$installed" --clean-after-build
if ($LASTEXITCODE) { throw 'vcpkg dependencies failed' }
$prefix = Join-Path $installed 'x64-windows'
foreach ($directory in @("$prefix/bin", "$prefix/tools/glslang", "$prefix/tools/ffmpeg", "$prefix/tools/gettext/bin", "$prefix/tools/pkgconf")) {
    if (Test-Path $directory) { $directory | Out-File $env:GITHUB_PATH -Append -Encoding utf8 }
}
$popplerZip = Join-Path $env:RUNNER_TEMP 'poppler.zip'
Invoke-WebRequest 'https://github.com/oschwartz10612/poppler-windows/releases/download/v26.09.0-0/Release-26.09.0-0.zip' -OutFile $popplerZip
if ((Get-FileHash $popplerZip -Algorithm SHA256).Hash.ToLower() -ne '7a6f256a0ddf7536182246a5733331bf4677cbcc34f4663774947ad34556c8d0') { throw 'Poppler checksum mismatch' }
$poppler = Join-Path $env:RUNNER_TEMP 'openxmb-poppler'
Expand-Archive $popplerZip -DestinationPath $poppler -Force
$popplerBin = (Get-ChildItem $poppler -Filter pdftoppm.exe -Recurse | Select-Object -First 1).Directory.FullName
if (!$popplerBin) { throw 'Poppler tools missing' }
$popplerBin | Out-File $env:GITHUB_PATH -Append -Encoding utf8
$gettextZip = Join-Path $env:RUNNER_TEMP 'gettext-tools.zip'
Invoke-WebRequest 'https://github.com/mlocati/gettext-iconv-windows/releases/download/v1.0-v1.19/gettext1.0-iconv1.19-static-64.zip' -OutFile $gettextZip
if ((Get-FileHash $gettextZip -Algorithm SHA256).Hash.ToLower() -ne '2b999bc37b56fa052ff98578e9cb188152009f634e3a66aa8ff263a016e9c466') { throw 'Gettext checksum mismatch' }
$gettext = Join-Path $env:RUNNER_TEMP 'openxmb-gettext'
Expand-Archive $gettextZip -DestinationPath $gettext -Force
$gettextBin = (Get-ChildItem $gettext -Filter msgfmt.exe -Recurse | Select-Object -First 1).Directory.FullName
if (!$gettextBin -or !(Test-Path "$gettextBin/msgmerge.exe")) { throw 'Gettext catalog tools missing' }
$gettextBin | Out-File $env:GITHUB_PATH -Append -Encoding utf8
@("VCPKG_ROOT=$vcpkg", "VCPKG_INSTALLED_DIR=$installed", "OPENXMB_POPPLER_BIN=$popplerBin", 'SDL_AUDIODRIVER=dummy') | Out-File $env:GITHUB_ENV -Append -Encoding utf8
