# BlackOps Terminal Bridge

Turn the easter-egg terminal hidden in the **Call of Duty: Black Ops (2010)** main
menu into a **real, working shell** — running inside the actual game process, rendered
in the game's own terminal screen.

The stock terminal is a hardcoded fake: a fixed command table, a scripted file system,
canned responses, plus the embedded Zork and Dead Ops Arcade games. This project bolts a
genuine host shell onto it. You type in the game; a real **PowerShell** (or cmd) runs on
your PC; its output is painted back into the in-game scrollback — correct font, colours,
wrapping, and all. It is a full terminal emulator, so interactive programs work too:
`ssh`, the Python REPL, `vim`, and even **Claude Code** run *inside* the BlackOps terminal.

## Features

- **Real shell inside the game** — PowerShell 7 → Windows PowerShell 5.1 → cmd, auto-detected, UTF-8.
- **Full terminal emulator** — ConPTY backend + a VT/ANSI parser, so TUIs (vim, htop, Claude Code) render in-game.
- **8000-line scrollback** with mouse-wheel scrolling and a visual scrollbar (PgUp/PgDn too).
- **24-bit truecolor** text, bypassing the engine's 8-colour palette.
- **Latin-1 codepage** for accented characters and common symbols.
- Authentic **typewriter reveal** (like the native terminal), with an instant-mode toggle.
- Non-destructive: the normal BlackOps terminal, menu, and games are untouched until you opt in.

## Requirements

- Windows 10/11 (x64).
- **Call of Duty: Black Ops (2010)** installed.
- **Visual Studio 2022** (the free Community edition is fine) with the *Desktop development with C++* workload. That's the only build dependency; CMake and MinHook are handled for you (CMake ships with VS; MinHook is fetched automatically).

## Quick start

```powershell
# 1. Get the code
git clone https://github.com/mansekhon983/BlackOps-Terminal-.git
cd BlackOps-Terminal-

# 2. Build (32-bit, to match BlackOps.exe)
.\build.ps1

# 3. Launch Black Ops -- OFFLINE (see safety note) -- and open the
#    easter-egg terminal in the main menu.

# 4. From an ELEVATED PowerShell, inject:
.\run.ps1

# 5. In the in-game terminal, type:  emu
```

You're now in a real shell. Type `emu` again (or `Ctrl+]`) to leave.

## In-game controls

| Key / word | Action |
|---|---|
| `emu` | Toggle the real-shell emulator on/off |
| `Ctrl+]` | Leave emu mode |
| `Ctrl+\` | Toggle the typewriter reveal (instant vs. animated) |
| Mouse wheel / `PgUp` / `PgDn` | Scroll the 8000-line history |
| any keypress while scrolled | snaps back to the live bottom |

There is also a lighter `connect` line-mode (one command at a time, no full emulator) if
you just want simple output-only commands.

## ⚠️ Safety & scope

- **OFFLINE / SINGLE-PLAYER ONLY.** BlackOps multiplayer ran **VAC**; injecting a DLL into a
  VAC-secured session is a ban. The terminal is a menu/campaign feature — there is no reason to
  be online. **Turn networking off** before injecting.
- **This runs whatever you type as a host shell command, by design.** It is a personal,
  single-machine novelty. Do not expose it over a network. If you ever add an IPC/bridge
  channel, bind it to `localhost` only.
- `inject.exe` needs to run from an **elevated** shell (it uses `CreateRemoteThread`).
- Offsets are resolved at load time by signature/RVA against this specific build of the game;
  a game update can break them. See `notes/offsets.md`.

## How it works

An injected DLL (`bo1bridge.dll`, x86) inline-hooks three points in the engine with
[MinHook](https://github.com/TsudaKageyu/minhook): the command-submit handler, the terminal's
glyph renderer (reused to paint our grid), and the per-frame tick. A [ConPTY](https://learn.microsoft.com/windows/console/pseudoconsoles)
child process runs the real shell; a reader thread feeds its output through a VT/ANSI parser into
an 80×27 screen model with an 8000-line scrollback, which is blitted into the engine's own
character/attribute grids each frame. All engine calls happen on the main thread via the frame hook.

Full architecture, the reverse-engineering log, and confirmed offsets are in `notes/` and
`docs/report.tex`. The project instructions live in `CLAUDE.md`.

## Repository layout

```
src/        DLL: dllmain, hooks, ConPTY backend, VT parser, screen grid, sig-scan
tools/      inject.exe + offline test harnesses (feed_log, conpty_test, vtparse_test)
notes/      offsets, RE log, plan; notes/dumps/ = raw Ghidra decompiler output
ghidra_scripts/  headless recon scripts used to find the offsets
docs/       report.tex (write-up)
build.ps1   configure + build (Win32)
run.ps1     inject into a running BlackOps.exe
```

## Building manually

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Outputs: `build/Release/bo1bridge.dll` and `build/Release/inject.exe`. The `-A Win32` is
mandatory — a 64-bit DLL cannot load into the 32-bit game.

## License

Personal/novelty project provided as-is, for offline single-player use. No warranty.
