param(
    [string]$Compiler = "",
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$res = Join-Path $root "resources"
if (-not $Output) { $Output = Join-Path $root "WirelessCenter.exe" }

if (-not $Compiler) {
    $candidates = @()
    if ($env:W64DEVKIT) {
        $candidates += (Join-Path $env:W64DEVKIT "bin\g++.exe")
    }
    $candidates += @(
        "C:\Users\$env:USERNAME\w64devkit\w64devkit\bin\g++.exe",
        "C:\w64devkit\bin\g++.exe",
        "C:\msys64\mingw64\bin\g++.exe",
        "C:\mingw64\bin\g++.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Compiler = $c; break }
    }
}
if (-not $Compiler) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { $Compiler = $cmd.Source }
}
if (-not $Compiler -or -not (Test-Path $Compiler)) {
    throw "g++ not found. Install MinGW-w64/w64devkit, or pass -Compiler <path>."
}

$bin = Split-Path -Parent $Compiler
$windres = Join-Path $bin "windres.exe"
if (-not (Test-Path $windres)) { $windres = "windres.exe" }

Write-Host "G++     : $Compiler"
Write-Host "WINDRES : $windres"
Write-Host "OUTPUT  : $Output"

$resourceObj = Join-Path $src "resource.o"
& $windres -I $res -I $src (Join-Path $src "resource.rc") -o $resourceObj

$sources = @(
    "main.cpp", "common.cpp", "hid.cpp", "protocols.cpp",
    "proto_logitech.cpp", "proto_asus.cpp", "proto_razer.cpp",
    "proto_rapoo.cpp", "proto_generic.cpp", "manager.cpp",
    "tray.cpp", "ui.cpp"
) | ForEach-Object { Join-Path $src $_ }

$flags = @(
    "-std=c++20", "-Os", "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0A00",
    "-municode", "-mwindows", "-static", "-s"
)
$libs = @(
    "-lgdi32", "-lgdiplus", "-luser32", "-lshell32", "-ladvapi32",
    "-lhid", "-lsetupapi", "-lcomctl32", "-lole32"
)

& $Compiler @flags -o $Output @sources $resourceObj @libs
if ($LASTEXITCODE -ne 0) { throw "build failed: $LASTEXITCODE" }

Get-Item $Output | Select-Object FullName, Length, LastWriteTime
