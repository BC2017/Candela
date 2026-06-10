# Builds Candela. Usage: scripts\build.ps1 [-Preset debug|release] [-ConfigureOnly]
param(
    [string]$Preset = "debug",
    [switch]$ConfigureOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$steps = "cmake --preset $Preset"
if (-not $ConfigureOnly) { $steps += " && cmake --build --preset $Preset" }

cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && $steps"
exit $LASTEXITCODE
