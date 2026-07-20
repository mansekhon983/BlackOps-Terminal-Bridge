<#
.SYNOPSIS
  Inject the BO1 terminal bridge into a running Black Ops process.

.DESCRIPTION
  Runs inject.exe (CreateRemoteThread + LoadLibrary) to load bo1bridge.dll into
  the game. The game must ALREADY be running and OFFLINE (BO1 multiplayer used
  VAC -- injecting into a VAC session is a ban; use single-player / the campaign
  menu only). This script must be run from an ELEVATED PowerShell.

  Auto-detects the game process name. Retail Steam is BlackOps.exe; the Plutonium
  client runs single-player as t5sp.exe (the easter-egg terminal is in the SP
  menu). Override with -Process if needed.

  After injecting, open the in-game easter-egg terminal and type  emu  to enter
  the real shell. Type  emu  again to leave.

.EXAMPLE
  .\run.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    # Explicit game process name (with or without .exe). Default: auto-detect.
    [string]$Process
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Known Black Ops (t5) process names, most-likely first. The easter-egg terminal
# lives in the single-player menu, so the SP executable is the target.
#   BlackOps.exe  retail / Steam
#   t5sp.exe      Plutonium single-player
#   t5mp.exe      Plutonium multiplayer (VAC-free under Plutonium, but SP has the terminal)
$knownNames = @('BlackOps', 't5sp', 't5mp')

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

# Resolve the target process. Prefer an explicit -Process, else probe the known
# names in order. Get-Process wants the name without ".exe".
$candidates = if ($Process) { @($Process -replace '\.exe$', '') } else { $knownNames }
$proc = $null
foreach ($name in $candidates) {
    if (Get-Process -Name $name -ErrorAction SilentlyContinue) { $proc = $name; break }
}
if (-not $proc) {
    throw ("No Black Ops process found (looked for: {0}). Launch the game (OFFLINE) " +
           "first, then re-run. For the Plutonium client this is t5sp.exe. If your " +
           "build uses a different name, pass it: .\run.ps1 -Process <name>.") -f ($candidates -join ', ')
}

$target = "$proc.exe"
Write-Host "[run] target=$target  injecting $dll" -ForegroundColor Cyan
& $inj $target $dll
if ($LASTEXITCODE -ne 0) { throw "injection failed (see message above)" }

Write-Host ""
Write-Host "Injected. In the in-game terminal, type 'emu' to open the shell." -ForegroundColor Green
Write-Host "  Ctrl+]        leave emu mode        Ctrl+\  toggle instant text" -ForegroundColor DarkGray
Write-Host "  wheel / PgUp / PgDn   scroll the 8000-line history" -ForegroundColor DarkGray
