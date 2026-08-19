# ReMCote Desktop — reproducible Windows x64 build.
#
# Usage:
#   .\build-windows.ps1
#   .\build-windows.ps1 -Debug
#
# The dependency source of truth is conanfile.txt / DEPENDENCIES.lock.md.
# WebView2 is intentionally not installed: the viewer is native Win32,
# Media Foundation, D3D11, and libdatachannel.

param([switch]$Debug)
$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$Config = if ($Debug) { "Debug" } else { "Release" }
$NvHeadersTag = "n12.2.72.0"

Write-Host "== ReMCote Desktop build ($Config) ==" -ForegroundColor Cyan
Write-Host "   Conan packages : conanfile.txt"
Write-Host "   NVENC headers  : $NvHeadersTag"

$missing = @()
foreach ($tool in @("git", "python")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        $missing += $tool
    }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsJson = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -version "[17.0,18.0)" -format json | ConvertFrom-Json
    if ($vsJson) { $vsPath = $vsJson[0].installationPath }
}
if (-not $vsPath) {
    $missing += "Visual Studio 2022 with Desktop development with C++"
}

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmake -and $vsPath) {
    $bundledCmake = Join-Path $vsPath `
        "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $bundledCmake) { $cmake = $bundledCmake }
}
if (-not $cmake) { $missing += "CMake 3.24+" }

if ($missing.Count -gt 0) {
    Write-Host "Missing prerequisites:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}

Write-Host "Installing Conan 2..." -ForegroundColor Cyan
python -m pip install "conan>=2.4,<3"
if ($LASTEXITCODE -ne 0) { throw "Conan installation failed" }
conan profile detect --force
if ($LASTEXITCODE -ne 0) { throw "Conan profile detection failed" }

$ConanDir = Join-Path $Root "conan"
conan install $Root `
    --output-folder=$ConanDir `
    --build=missing `
    -s build_type=$Config `
    -s arch=x86_64 `
    -s compiler.cppstd=17
if ($LASTEXITCODE -ne 0) { throw "Conan dependency resolution failed" }

$toolchain = Get-ChildItem -Path $ConanDir -Recurse `
    -Filter conan_toolchain.cmake | Select-Object -First 1
if (-not $toolchain) { throw "conan_toolchain.cmake not found" }

$thirdParty = Join-Path $Root "third_party"
$nvHeaders = Join-Path $thirdParty "nv-codec-headers"
$nvTagFile = Join-Path $nvHeaders ".remcote-pinned-tag"
$cachedTag = if (Test-Path $nvTagFile) {
    (Get-Content $nvTagFile).Trim()
} else {
    ""
}
if (-not (Test-Path (Join-Path $nvHeaders ".git")) -or
    $cachedTag -ne $NvHeadersTag) {
    if (Test-Path $nvHeaders) {
        cmd /c "rmdir /s /q `"$nvHeaders`""
    }
    New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null
    git clone --depth 1 --branch $NvHeadersTag `
        https://github.com/FFmpeg/nv-codec-headers $nvHeaders
    if ($LASTEXITCODE -ne 0) { throw "nv-codec-headers clone failed" }
    Set-Content $nvTagFile $NvHeadersTag
}

$BuildDir = Join-Path $Root "build"
& $cmake -S $Root -B $BuildDir `
    -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$($toolchain.FullName)" `
    -DCMAKE_BUILD_TYPE=$Config `
    -DNVCODEC_SDK_DIR="$nvHeaders\include"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& $cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Compile failed" }

$Dist = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
Copy-Item `
    (Join-Path $BuildDir "bin\$Config\ReMCoteHost.exe") `
    (Join-Path $Dist "ReMCoteHost.exe") -Force

Write-Host "Build complete: $Dist\ReMCoteHost.exe" -ForegroundColor Green
Write-Host "Install the same program on both PCs; no browser is required."