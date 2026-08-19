# ReMCote Host — reproducible Windows x64 build
#
# Usage:
#   .\build-windows.ps1              # release build -> dist\ReMCoteHost.exe
#   .\build-windows.ps1 -Debug       # debug build
#
# Pinned dependency versions (matches DEPENDENCIES.lock.md):
#   vcpkg           tag 2025.04.09   commit ce613c41
#   nv-codec-headers tag n12.2.72.0  commit c69278340
#   libdatachannel  0.22.6           (resolved by pinned vcpkg baseline)
#
# Prerequisites:
#   - Visual Studio 2022 (any edition) with "Desktop development with C++"
#   - CMake 3.24+   (included in the VS C++ workload)
#   - git
param([switch]$Debug)
$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$Config = if ($Debug) { "Debug" } else { "Release" }

# Pinned dependency revisions — change these ONLY when intentionally upgrading.
$VcpkgTag      = "2025.04.09"
$NvHeadersTag  = "n12.2.72.0"

Write-Host "== ReMCote Host build ($Config) ==" -ForegroundColor Cyan
Write-Host "   vcpkg tag      : $VcpkgTag"
Write-Host "   nv-codec tag   : $NvHeadersTag"

# --- 0. Preflight: check prerequisites -------------------------------------
$missing = @()

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $missing += "git — install from https://git-scm.com/download/win"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = $null
$vsMajor = $null
if (Test-Path $vswhere) {
    $vsJson = & $vswhere -latest -products * -prerelease `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -version "[17.0,19.0)" -format json | ConvertFrom-Json
    if ($vsJson) {
        $vsPath  = $vsJson[0].installationPath
        $vsMajor = [int]($vsJson[0].installationVersion.Split('.')[0])
    }
}
if (-not $vsPath) {
    $missing += "Visual Studio 2022 with the 'Desktop development with C++' workload"
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake -and $vsPath) {
    $vsCmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCmake) { $cmake = $vsCmake }
}
if (-not $cmake) {
    $missing += "CMake 3.24+ — or add 'C++ CMake tools' in VS installer"
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Missing prerequisites:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}

# Toolchain diagnostics
Write-Host ""
Write-Host "Toolchain:" -ForegroundColor Cyan
Write-Host "  Visual Studio $vsMajor  at: $vsPath"
Write-Host "  CMake: $(& $cmake --version | Select-Object -First 1)"
$generator = if ($vsMajor -ge 18) { "Visual Studio 18 2026" } else { "Visual Studio 17 2022" }
Write-Host "  CMake generator: $generator"
Write-Host ""

# --- 1. Bootstrap vcpkg at pinned tag --------------------------------------
$VcpkgDir = Join-Path $Root "third_party\vcpkg"
if (-not (Test-Path (Join-Path $VcpkgDir "vcpkg.exe"))) {
    if (-not (Test-Path $VcpkgDir)) {
        Write-Host "Cloning vcpkg $VcpkgTag (first run)..." -ForegroundColor Yellow
        git clone --depth 1 --branch $VcpkgTag https://github.com/microsoft/vcpkg $VcpkgDir
        if ($LASTEXITCODE -ne 0) { throw "vcpkg clone failed" }
    }
    Write-Host "Bootstrapping vcpkg..."
    & (Join-Path $VcpkgDir "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
} else {
    Write-Host "vcpkg: already bootstrapped (CACHE HIT)" -ForegroundColor Green
}

# --- 2. Fetch NVENC headers at pinned tag ----------------------------------
$NvHeaders = Join-Path $Root "third_party\nv-codec-headers"
if (-not (Test-Path $NvHeaders)) {
    Write-Host "Cloning nv-codec-headers $NvHeadersTag..." -ForegroundColor Yellow
    git clone --depth 1 --branch $NvHeadersTag https://github.com/FFmpeg/nv-codec-headers $NvHeaders
    if ($LASTEXITCODE -ne 0) { throw "nv-codec-headers clone failed" }
} else {
    Write-Host "nv-codec-headers: already present (CACHE HIT)" -ForegroundColor Green
}

# --- 3. Configure ----------------------------------------------------------
$BuildDir = Join-Path $Root "build"
Write-Host ""
Write-Host "Configuring..." -ForegroundColor Cyan
& $cmake -S $Root -B $BuildDir `
    -G $generator -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$VcpkgDir\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DNVCODEC_SDK_DIR="$NvHeaders\include"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# --- 4. Build --------------------------------------------------------------
Write-Host ""
Write-Host "Compiling..." -ForegroundColor Cyan
& $cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Compile failed" }

# --- 5. Package ------------------------------------------------------------
$Dist = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
Copy-Item (Join-Path $BuildDir "bin\$Config\ReMCoteHost.exe") (Join-Path $Dist "ReMCoteHost.exe") -Force

Write-Host ""
Write-Host "Build complete." -ForegroundColor Green
Write-Host "EXE: $Dist\ReMCoteHost.exe"
Write-Host ""
Write-Host "Run: cd $Dist; .\ReMCoteHost.exe"
Write-Host "(No server configuration needed — connects to remcote.replit.app by default.)"
Write-Host "(Override: set REMCOTE_SIGNALING_URL or create remcote-server.txt next to the exe.)"
