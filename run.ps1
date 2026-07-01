<#
.SYNOPSIS
  Inject the BO1 terminal bridge into a running BlackOps.exe.

.DESCRIPTION
  Runs inject.exe (CreateRemoteThread + LoadLibrary) to load bo1bridge.dll into
  the game. The game must ALREADY be running and OFFLINE (BO1 multiplayer used
  VAC -- injecting into a VAC session is a ban; use single-player / the campaign
  menu only). This script must be run from an ELEVATED PowerShell.

  After injecting, open the in-game easter-egg terminal and type  emu  to enter
  the real shell. Type  emu  again to leave.

.EXAMPLE
  .\run.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Elevation check -- CreateRemoteThread into the game needs admin.
$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Not elevated. Injection will likely fail with OpenProcess error 5."
    Write-Warning "Re-run this from an ADMINISTRATOR PowerShell."
}

$inj = Join-Path $PSScriptRoot "build\$Config\inject.exe"
$dll = Join-Path $PSScriptRoot "build\$Config\bo1bridge.dll"
if (-not (Test-Path $inj) -or -not (Test-Path $dll)) {
    throw "Missing build output. Run .\build.ps1 first (looked in build\$Config\)."
}

if (-not (Get-Process -Name BlackOps -ErrorAction SilentlyContinue)) {
    throw "BlackOps.exe is not running. Launch the game (OFFLINE) first, then re-run."
}

Write-Host "[run] injecting $dll" -ForegroundColor Cyan
& $inj "BlackOps.exe" $dll
if ($LASTEXITCODE -ne 0) { throw "injection failed (see message above)" }

Write-Host ""
Write-Host "Injected. In the in-game terminal, type 'emu' to open the shell." -ForegroundColor Green
Write-Host "  Ctrl+]        leave emu mode        Ctrl+\  toggle instant text" -ForegroundColor DarkGray
Write-Host "  wheel / PgUp / PgDn   scroll the 8000-line history" -ForegroundColor DarkGray
