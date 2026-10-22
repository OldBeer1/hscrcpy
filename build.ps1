# HScrcpy Build Script
# =====================
# One-click build script. Automatically installs missing tools via MSYS2.
# Usage: .\build.ps1 [-Clean]
# =====================

param([switch]$Clean)

$ErrorActionPreference = "Stop"

# --- Colors ---
$Host.UI.RawUI.ForegroundColor = "White"
function Write-Step($msg) { Write-Host ">> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)  { Write-Host "   $msg" -ForegroundColor Green }
function Write-Warn($msg) { Write-Host "   [WARN] $msg" -ForegroundColor Yellow }
function Write-Err($msg) { Write-Host "   [ERROR] $msg" -ForegroundColor Red; exit 1 }

Write-Host "=== HScrcpy Build Script ==="
Write-Host ""

# ============================================================
# Step 1: Find MSYS2
# ============================================================
$msys2Candidates = @(
    "C:\msys64", "D:\msys64", "C:\msys32", "D:\msys32",
    "C:\tools\msys64", "D:\tools\msys64",
    "$env:USERPROFILE\scoop\apps\msys2\current"
)

$toolchainPath = $env:TOOLCHAIN_PATH
if (-not $toolchainPath) {
    foreach ($p in $msys2Candidates) {
        if (Test-Path "$p\usr\bin\bash.exe") {
            $toolchainPath = $p; break
        }
    }
}

if (-not $toolchainPath) {
    Write-Err "MSYS2 not found.`n`nPlease install: https://www.msys2.org/`nThen re-run this script."
}

Write-Step "MSYS2 found at: $toolchainPath"

# ============================================================
# Step 2: Detect MSYS2 environment (ucrt64 or mingw64)
# ============================================================
$mingwSys = "ucrt64"
$mingwDir = "$toolchainPath\ucrt64"
if (-not (Test-Path "$mingwDir\bin")) { $mingwSys = "mingw64"; $mingwDir = "$toolchainPath\mingw64" }

$ucrtExe = "$toolchainPath\ucrt64.exe"
$bash = "$toolchainPath\usr\bin\bash.exe"
$mingwPkgPrefix = if ($mingwSys -eq "ucrt64") { "mingw-w64-ucrt-x86_64" } else { "mingw-w64-x86_64" }

# ============================================================
# Step 3: Check and install required tools via pacman
# ============================================================
function Invoke-MSYS2($cmd) {
    # Set env vars for MSYS2 - these get inherited by child processes
    $env:MSYSTEM = $mingwSys.ToUpper()
    $env:CHERE_INVOKING = "yes"
    
    # Write command to a temp script
    $tmpScript = "$env:TEMP\hscrcpy_$PID.sh"
    [System.IO.File]::WriteAllText($tmpScript, $cmd)
    $scriptPath = $tmpScript.Replace('\','/')
    
    # Run bash --login which loads MSYS2 profile, then the script
    $proc = Start-Process -NoNewWindow -Wait -PassThru -FilePath $bash -ArgumentList @("--login", $scriptPath)
    $exitCode = $proc.ExitCode
    
    if (Test-Path $tmpScript) { Remove-Item -Force $tmpScript }
    return $exitCode
}

$gcc = "$mingwDir\bin\gcc.exe"
$gxx = "$mingwDir\bin\g++.exe"
$cmake = "$mingwDir\bin\cmake.exe"

$needInstall = (-not (Test-Path $gcc)) -or (-not (Test-Path $cmake))

if ($needInstall) {
    Write-Step "Installing MinGW-w64 toolchain via MSYS2..."
    
    Write-Host "   Step 1: Updating package database (pacman -Syu)..."
    # pacman -Syu may restart itself, exit code 1 is normal
    $null = Invoke-MSYS2 "pacman -Syu --noconfirm"
    Write-Host "   (first-time update completed)"
    
    Write-Host "   Step 2: Installing GCC, CMake, Make..."
    $exitCode = Invoke-MSYS2 "pacman -S --noconfirm ${mingwPkgPrefix}-gcc ${mingwPkgPrefix}-cmake make"
    if ($exitCode -ne 0) {
        Write-Err "Package installation failed (exit code: $exitCode).`nPlease open 'MSYS2 $mingwSys' from Start Menu and run: pacman -S ${mingwPkgPrefix}-gcc ${mingwPkgPrefix}-cmake make"
    }
    Write-Ok "Toolchain installed"
}

# Verify
if (-not (Test-Path $gcc))   { Write-Err "GCC not found after install: $gcc" }
if (-not (Test-Path $gxx))   { Write-Err "G++ not found after install: $gxx" }
if (-not (Test-Path $cmake)) { Write-Err "CMake not found after install: $cmake" }

Write-Ok "GCC: $gcc"
Write-Ok "CMake: $cmake"
Write-Host ""

# ============================================================
# Step 4: Clean
# ============================================================
if ($Clean) {
    Write-Step "Cleaning build directory..."
    if (Test-Path "build") { Remove-Item -Recurse -Force "build" }
    Write-Ok "Done."
    exit 0
}

# ============================================================
# Step 5: Check nghttp2 (required for gRPC HTTP/2)
# ============================================================
$nghttp2Lib = "$mingwDir\lib\libnghttp2.a"
if (-not (Test-Path $nghttp2Lib)) {
    Write-Step "Installing nghttp2 via MSYS2..."
    $exitCode = Invoke-MSYS2 "pacman -S --noconfirm ${mingwPkgPrefix}-nghttp2"
    if ($exitCode -ne 0) {
        Write-Err "nghttp2 installation failed (exit code: $exitCode).`nPlease run: pacman -S ${mingwPkgPrefix}-nghttp2"
    }
    Write-Ok "nghttp2 installed"
} else {
    Write-Ok "nghttp2: $nghttp2Lib"
}

# ============================================================
# Step 6: Check FFmpeg (optional, for H264 decoding)
# ============================================================
$ffmpegRoot = $env:FFMPEG_DIR
if (-not $ffmpegRoot) {
    $bundledFfmpeg = "ffmpeg\ffmpeg-master-latest-win64-gpl-shared"
    if (Test-Path "$bundledFfmpeg\include") { $ffmpegRoot = (Resolve-Path $bundledFfmpeg).Path }
}

$ffmpegBinDir = $null
if ($ffmpegRoot) {
    Write-Ok "FFmpeg found: $ffmpegRoot"
    $ffmpegBinDir = "$ffmpegRoot\bin"
} else {
    Write-Warn "FFmpeg not found - H264 decoding disabled"
    Write-Warn "To enable H264 streaming, run: .\download_ffmpeg.ps1"
}

# ============================================================
# Step 7: Configure (CMake)
# ============================================================
Write-Step "CMake configure..."

if (-not (Test-Path "build")) { New-Item -ItemType Directory -Path "build" | Out-Null }

# Build a proper PATH so dlltool and other MSYS2 tools can be found
$env:Path = "$mingwDir\bin;$toolchainPath\usr\bin;$env:Path"
$env:MSYSTEM = if ($mingwSys -eq "ucrt64") { "UCRT64" } else { "MINGW64" }

& $cmake -B build -S . -G "MSYS Makefiles" -DCMAKE_CXX_COMPILER="$gxx"
if ($LASTEXITCODE -ne 0) { Write-Err "CMake configuration failed!" }
Write-Ok "CMake configured"

# ============================================================
# Step 8: Build
# ============================================================
Write-Step "Building..."
& $cmake --build build -j
if ($LASTEXITCODE -ne 0) { Write-Err "Build failed!" }
Write-Ok "Build successful"

# ============================================================
# Step 9: Copy runtime files
# ============================================================
Write-Step "Copying runtime files..."

if (-not (Test-Path "build\server")) {
    if (Test-Path "server") {
        Copy-Item -Recurse -Force "server" "build\server"
        Write-Ok "server\ - copied"
    }
} else {
    # Sync new/updated SO files so newly added server variants appear in build\server
    if (Test-Path "server") {
        Copy-Item -Force "server\*" "build\server\" -ErrorAction SilentlyContinue
        Write-Ok "server\ - updated"
    }
}

if (-not (Test-Path "build\res")) {
    if (Test-Path "res") {
        Copy-Item -Recurse -Force "res" "build\res"
        Write-Ok "res\ - copied"
    }
} else {
    Copy-Item -Recurse -Force "res\*" "build\res\"
    Write-Ok "res\ - updated"
}

if ($ffmpegBinDir -and (Test-Path $ffmpegBinDir)) {
    foreach ($dll in @("avcodec-62.dll","avutil-60.dll","swscale-9.dll","swresample-6.dll")) {
        $src = "$ffmpegBinDir\$dll"
        if (Test-Path $src) {
            Copy-Item -Force $src "build\"
            Write-Ok "$dll - copied"
        }
    }
}

Write-Host ""
Write-Host "=== Build succeeded ===" -ForegroundColor Green
Write-Host "Output: build\hscrcpy.exe"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Connect a HarmonyOS device via USB or TCP"
Write-Host "  2. Run: .\build\hscrcpy.exe"