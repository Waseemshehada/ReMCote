# ReMCote Host — reproducible Windows x64 build
#
# Usage:
#   .\build-windows.ps1              # release build -> dist\ReMCoteHost.exe
#   .\build-windows.ps1 -Debug       # debug build
#
# Prerequisites (checked below, with install pointers if missing):
#   - Visual Studio 2022 (any edition) with "Desktop development with C++"
#   - CMake 3.24+   (also included in the VS C++ workload)
#   - git
param([switch]$Debug)
$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$Config = if ($Debug) { "Debug" } else { "Release" }

Write-Host "== ReMCote Host build ($Config) ==" -ForegroundColor Cyan

# --- 0. Preflight: report ALL missing prerequisites before doing anything ----
$missing = @()

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $missing += "git — install from https://git-scm.com/download/win"
}

# Locate Visual Studio 2022 C++ tools via vswhere (ships with VS installer).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -version "[17.0,18.0)" -property installationPath
}
if (-not $vsPath) {
    $missing += "Visual Studio 2022 with the 'Desktop development with C++' workload — install VS 2022 Community from https://visualstudio.microsoft.com/downloads/ and select that workload"
}

# CMake: PATH, or the copy bundled with the VS C++ workload.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake -and $vsPath) {
    $vsCmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCmake) { $cmake = $vsCmake }
}
if (-not $cmake) {
    $missing += "CMake 3.24+ — install from https://cmake.org/download/ (check 'Add to PATH'), or add the 'C++ CMake tools' component in the VS installer"
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Missing prerequisites:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Install the items above, open a NEW PowerShell window, and re-run .\build-windows.ps1"
    exit 1
}
Write-Host "Prerequisites OK (VS 2022: $vsPath)" -ForegroundColor Green

# --- 1. Bootstrap vcpkg (dependency manager, cloned locally) -----------------
$VcpkgDir = Join-Path $Root "third_party\vcpkg"
if (-not (Test-Path (Join-Path $VcpkgDir "vcpkg.exe"))) {
    Write-Host "Bootstrapping vcpkg (first run only, downloads dependencies)..."
    if (-not (Test-Path $VcpkgDir)) {
        git clone --depth 1 https://github.com/microsoft/vcpkg $VcpkgDir
    }
    & (Join-Path $VcpkgDir "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
}

# --- 2. Fetch NVENC headers (NVIDIA Video Codec SDK interface) ---------------
# nvEncodeAPI is loaded from the NVIDIA driver at runtime; only headers needed.
$NvHeaders = Join-Path $Root "third_party\nv-codec-headers"
if (-not (Test-Path $NvHeaders)) {
    Write-Host "Fetching nv-codec-headers..."
    git clone --depth 1 https://github.com/FFmpeg/nv-codec-headers $NvHeaders
}

# --- 3. Configure -------------------------------------------------------------
$BuildDir = Join-Path $Root "build"
& $cmake -S $Root -B $BuildDir `
    -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$VcpkgDir\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DNVCODEC_SDK_DIR="$NvHeaders\include"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (see output above)" }

# --- 4. Build -----------------------------------------------------------------
& $cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed (see compiler output above)" }

# --- 5. Package -----------------------------------------------------------------
$Dist = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
Copy-Item (Join-Path $BuildDir "bin\$Config\ReMCoteHost.exe") (Join-Path $Dist "ReMCoteHost.exe") -Force

Write-Host ""
Write-Host "Build complete." -ForegroundColor Green
Write-Host "EXE: $Dist\ReMCoteHost.exe" -ForegroundColor Green
Write-Host ""
Write-Host "Before running, point the Host at your ReMCote server:"
Write-Host '  $env:REMCOTE_SIGNALING_URL = "wss://<your-remcote-server>/api/ws"'
Write-Host "  cd $Dist"
Write-Host "  .\ReMCoteHost.exe"
Write-Host ""
Write-Host "(Alternatively create remcote-server.txt next to the exe containing that URL.)"
