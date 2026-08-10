---
layout: default
title: BlackOps Terminal Bridge
---

Turn the easter-egg terminal hidden in the **Call of Duty: Black Ops (2010)** main menu
into a **real, working shell** — running inside the actual game process, rendered in the
game's own terminal screen.

The stock terminal is a hardcoded fake: a fixed command table, a scripted file system,
canned responses, plus the embedded Zork and Dead Ops Arcade games. This project bolts a
real host shell onto it. You type in the game; a real PowerShell runs on your PC; its
output is painted back into the in-game scrollback — correct font, colours, wrapping,
and all.

[View the source on GitHub](https://github.com/mansekhon983/BlackOps-Terminal-Bridge)

## What it does

- **Real shell inside the game** — opens PowerShell 7 by default, falling back to
  Windows PowerShell 5.1, then `cmd.exe`.
- **Full terminal emulator** — ConPTY backend plus a VT/ANSI parser, so full-screen TUI
  programs render correctly in-game.
- **8000-line scrollback** with mouse-wheel scrolling and a visual scrollbar.
- **24-bit truecolor** text, bypassing the engine's 8-colour palette.
- **Latin-1 codepage** for accented characters and common symbols.
- Authentic **typewriter reveal** like the native terminal, with an instant-mode toggle.
- Non-destructive — the normal terminal, menu, and games are untouched until you opt in.

## How it works

An injected DLL (`bo1bridge.dll`, x86) inline-hooks three points in the engine with
[MinHook](https://github.com/TsudaKageyu/minhook): the command-submit handler, the
terminal's glyph renderer (reused to paint our grid), and the per-frame tick.

A [ConPTY](https://learn.microsoft.com/windows/console/pseudoconsoles) child process runs
the real shell. A reader thread feeds its output through a VT/ANSI parser into an 80×27
screen model with an 8000-line scrollback, which is blitted into the engine's own
character and attribute grids each frame. All engine calls happen on the main thread via
the frame hook — calling them from the worker thread crashes the game.

## Quick start

Requires Windows 10/11, Black Ops installed, and Visual Studio 2022 with the
*Desktop development with C++* workload.

```powershell
git clone https://github.com/mansekhon983/BlackOps-Terminal-Bridge.git
cd BlackOps-Terminal-Bridge

# Build 32-bit, to match BlackOps.exe
.\build.ps1

# Launch Black Ops OFFLINE, open the easter-egg terminal, then
# from an ELEVATED PowerShell:
.\run.ps1

# In the in-game terminal, type:  emu
```

## Safety

**Offline, single-player only.** Black Ops multiplayer ran VAC, and injecting a DLL into
a VAC-secured session is a ban. The terminal is a menu feature — there is no reason to be
online. Turn networking off before injecting.

This runs whatever you type as a host shell command, by design. It is a personal,
single-machine novelty. Do not expose it over a network.

## Reverse-engineering notes

The confirmed offsets, signature patterns, and the full RE log live in the
[`notes/`](https://github.com/mansekhon983/BlackOps-Terminal-Bridge/tree/master/notes)
directory of the repository.

---

Released under the [MIT License](https://github.com/mansekhon983/BlackOps-Terminal-Bridge/blob/master/LICENSE).
