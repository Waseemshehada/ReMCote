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
# Version enforcement: marker files (.remcote-pinned-tag) are written into
# each cloned dependency directory. If the marker is absent or stale, the
# directory is deleted and the correct version is re-cloned. This detects
# any cached wrong-version dependency automatically, even under GitHub
# Actions cache partial-key restoration.
param([switch]$Debug)
$ErrorActionPreference = "Stop"

$Root   = $PSScriptRoot
$Config = if ($Debug) { "Debug" } else { "Release" }

# ── Pinned revisions ─────────────────────────────────────────────────────────
# Change these ONLY when intentionally upgrading (update DEPENDENCIES.lock.md too).
$VcpkgTag     = "2025.04.09"
$NvHeadersTag = "n12.2.72.0"

Write-Host "== ReMCote Host build ($Config) ==" -ForegroundColor Cyan
Write-Host "   vcpkg tag       : $VcpkgTag"
Write-Host "   nv-codec tag    : $NvHeadersTag"
Write-Host ""

# ── 0. Prerequisites check ───────────────────────────────────────────────────
$missing = @()
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $missing += "git — https://git-scm.com/download/win"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = $null; $vsMajor = $null
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
    $missing += "Visual Studio 2022 with 'Desktop development with C++' workload"
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake -and $vsPath) {
    $vsCmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCmake) { $cmake = $vsCmake }
}
if (-not $cmake) {
    $missing += "CMake 3.24+ — or add 'C++ CMake tools' component in VS installer"
}

if ($missing.Count -gt 0) {
    Write-Host "Missing prerequisites:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}

# Toolchain diagnostics
Write-Host "Toolchain:" -ForegroundColor Cyan
Write-Host "  Visual Studio $vsMajor  at: $vsPath"
Write-Host "  CMake: $(& $cmake --version | Select-Object -First 1)"
$generator = if ($vsMajor -ge 18) { "Visual Studio 18 2026" } else { "Visual Studio 17 2022" }
Write-Host "  Generator: $generator"
Write-Host ""

# ── Helper: ensure a directory is cloned at the correct pinned tag ────────────
# Uses a .remcote-pinned-tag marker file to detect stale cached checkouts.
function Ensure-PinnedClone {
    param([string]$Dir, [string]$Tag, [string]$Url, [string]$Label)
    $marker = Join-Path $Dir ".remcote-pinned-tag"
    $cachedTag = if (Test-Path $marker) { (Get-Content $marker).Trim() } else { "" }
    if ((Test-Path (Join-Path $Dir ".git")) -and $cachedTag -eq $Tag) {
        Write-Host "${Label}: CACHE HIT (pinned $Tag)" -ForegroundColor Green
        return
    }
    if ($cachedTag -ne "" -and $cachedTag -ne $Tag) {
        Write-Host "${Label}: version mismatch (cached=$cachedTag, need=$Tag) — recloning" -ForegroundColor Yellow
    } else {
        Write-Host "${Label}: CACHE MISS — cloning $Tag" -ForegroundColor Yellow
    }
    if (Test-Path $Dir) { Remove-Item $Dir -Recurse -Force -ErrorAction SilentlyContinue }
    $parent = Split-Path $Dir
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    git clone --depth 1 --branch $Tag $Url $Dir
    if ($LASTEXITCODE -ne 0) { throw "$Label clone failed" }
    Set-Content $marker $Tag
}

# ── 1. vcpkg ─────────────────────────────────────────────────────────────────
$VcpkgDir = Join-Path $Root "third_party\vcpkg"
Ensure-PinnedClone -Dir $VcpkgDir -Tag $VcpkgTag `
    -Url "https://github.com/microsoft/vcpkg" -Label "vcpkg"

if (-not (Test-Path (Join-Path $VcpkgDir "vcpkg.exe"))) {
    Write-Host "Bootstrapping vcpkg..."
    & (Join-Path $VcpkgDir "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "vcpkg bootstrap failed" }
} else {
    Write-Host "vcpkg already bootstrapped"
}

# ── 2. NVENC headers ─────────────────────────────────────────────────────────
$NvHeaders = Join-Path $Root "third_party\nv-codec-headers"
Ensure-PinnedClone -Dir $NvHeaders -Tag $NvHeadersTag `
    -Url "https://github.com/FFmpeg/nv-codec-headers" -Label "nv-codec-headers"

# ── 3. CMake configure ───────────────────────────────────────────────────────
$BuildDir = Join-Path $Root "build"
Write-Host ""
Write-Host "Configuring..." -ForegroundColor Cyan
& $cmake -S $Root -B $BuildDir `
    -G $generator -A x64 `
    -DCMAKE_TOOLCHAIN_FILE="$VcpkgDir\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DNVCODEC_SDK_DIR="$NvHeaders\include"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

# ── 4. Compile ───────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "Compiling..." -ForegroundColor Cyan
& $cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Compile failed" }

# ── 5. Package ───────────────────────────────────────────────────────────────
$Dist = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
Copy-Item (Join-Path $BuildDir "bin\$Config\ReMCoteHost.exe") (Join-Path $Dist "ReMCoteHost.exe") -Force

Write-Host ""
Write-Host "Build complete." -ForegroundColor Green
Write-Host "EXE: $Dist\ReMCoteHost.exe"
Write-Host ""
Write-Host "Run: cd $Dist; .\ReMCoteHost.exe"
Write-Host "(Connects to remcote.replit.app by default — no configuration needed.)"
