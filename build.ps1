<#
.SYNOPSIS
  Build the BO1 terminal bridge (bo1bridge.dll + inject.exe), 32-bit.

.DESCRIPTION
  Configures and builds the CMake project for Win32 (x86) -- required to match
  BlackOps.exe, which is a 32-bit process. Outputs land in build\Release\.
  Requires Visual Studio 2022 (any edition, incl. the free Community) with the
  "Desktop development with C++" workload. CMake is found automatically from a
  VS install if it is not already on PATH.

.EXAMPLE
  .\build.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Find-CMake {
    $c = Get-Command cmake -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $candidates = @(
        "$env:ProgramFiles\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    foreach ($pat in $candidates) {
        $hit = Get-ChildItem $pat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "cmake.exe not found. Install Visual Studio 2022 with the C++ workload, or add cmake to PATH."
}

$cmake = Find-CMake
Write-Host "[build] using cmake: $cmake" -ForegroundColor Cyan

& $cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $cmake --build build --config $Config
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$dll = Join-Path $PSScriptRoot "build\$Config\bo1bridge.dll"
$inj = Join-Path $PSScriptRoot "build\$Config\inject.exe"
Write-Host ""
Write-Host "[build] done:" -ForegroundColor Green
Write-Host "  $dll"
Write-Host "  $inj"
Write-Host ""
Write-Host "Next: launch Black Ops OFFLINE, open the in-game terminal, then run" -ForegroundColor Yellow
Write-Host "  .\run.ps1        (from an ELEVATED PowerShell)" -ForegroundColor Yellow
