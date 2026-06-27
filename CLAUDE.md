# CLAUDE.md — BO1 Terminal → Real Shell Bridge

> Hand-off context for Claude Code. This file is auto-loaded each session; keep it dense and current. Update the **Status** and **Offsets** sections as work lands.

## What this project is

Bridge the **Call of Duty: Black Ops (2010, "BO1")** in-game easter-egg terminal so that commands typed *in the game's terminal* execute in a **real Windows shell** (PowerShell/cmd) on the host, with the shell's output rendered back into the in-game terminal scrollback.

The goal is authenticity: the work happens *inside the real game process*, not in a look-alike. (A standalone themed terminal was explicitly considered and rejected.)

**Status: greenfield.** No code exists yet; nothing in this repo is built. Start at Milestone 0 below.

## The target (facts + assumptions)

- The in-game terminal is a **hardcoded simulation**: fixed command table, fake filesystem, scripted string responses. Embedded Zork and Dead Ops Arcade are self-contained programs. **It has zero I/O to the host and no config flag unlocks real behavior.** All "realness" must be bolted on from outside.
- Engine: Treyarch **"t5"** (heavily modified IW / idTech-3 lineage).
- `BlackOps.exe` is **almost certainly 32-bit — VERIFY FIRST** (`dumpbin /headers BlackOps.exe`, or any PE viewer). Everything downstream — debugger choice, DLL bitness, calling convention — depends on this. If 32-bit: build an **x86** DLL and use **x32dbg**.

## Architecture (the plan)

Inline-hook the game via an injected DLL. Three hook points:

1. **Command-submit handler** — fires on Enter in the terminal; the assembled command string sits in a register / stack slot. Hook captures the string.
2. **Line-append function** — the routine the game already calls to print one response line into the terminal scrollback. **Reuse it** to render shell output (gets correct font, wrapping, and scroll behavior for free). Finding *and calling* this correctly is the hard 20% of the project.
3. **Per-frame tick** — the main update/render function. Used to marshal output back onto the **main thread** (see threading model).

### Threading model (do not deviate — off-thread engine calls crash)

```
submit hook ──> [inbound queue] ──> worker thread ──> spawn shell, read stdout/stderr
                                                            │
                                                            ▼
per-frame hook <── drain ── [outbound queue] <── push output lines
   │
   └─ calls line-append ON MAIN THREAD only
```

- The shell runs on a **worker thread** so the game never freezes while a command executes.
- Engine functions (especially line-append) are called **only from the per-frame hook**, i.e. on the main thread. Calling line-append directly from the worker thread causes sporadic crashes.

### Shell bridge

- Spawn `powershell.exe -NoProfile -Command "<cmd>"` via `CreateProcessA` with `CREATE_NO_WINDOW`; redirect stdout + stderr to an anonymous pipe (`CreatePipe`, inheritable write handle, read handle marked non-inherit). Read to EOF, split on newlines.
- Decision: start with **one-shot per command**. A persistent session (keep `powershell.exe` alive, write to its stdin) is a later upgrade — same primitives, more bookkeeping.

### Calling convention (critical)

When calling the game's own line-append function, match its convention **exactly**: argument order, register vs. stack passing, and a likely `this` pointer (32-bit engine internals are often `thiscall` or a custom register convention). A mismatch corrupts the stack instantly. Read the convention off how the game itself calls the function in x32dbg, and mirror it precisely.

## Finding the two functions (entry technique)

The terminal's **hardcoded response strings** are the way in:

1. Ghidra → **Defined Strings** → find a known response string (Zork text, `HELP` output, a fake file's contents).
2. Check its **XREFs**. The referencing code is the dispatch logic, or one hop from it.
3. In x32dbg, breakpoint there, type a known command in-game, inspect registers/stack at the break.
4. Walk the call stack **up** to where the raw input is assembled → **submit handler**. Walk **down** to where the response string is handed to rendering → **line-append**.

Record every confirmed address in the **Offsets** table, but **hook via signature/pattern scanning, not hardcoded addresses** — offsets break on any game update.

## Toolchain

- **Ghidra** or IDA — static analysis
- **x32dbg** (32-bit x64dbg) — live debugging / breakpoints *(use x64dbg if the binary turns out 64-bit)*
- **Cheat Engine** — quick memory poking
- **MinHook** — inline hooks (Microsoft Detours is an acceptable alternative)
- **Injector** — `CreateRemoteThread` + `LoadLibrary` is fine for personal use; or Xenos / nefarius-Injector

## Build (proposed — greenfield; set up on first commit)

Target **must** match game bitness (x86 if 32-bit):

```
cmake -A Win32 -B build
cmake --build build --config Release
```

Proposed layout:

```
/src
  dllmain.cpp        # injection entry; sets up hooks on attach
  hooks.{h,cpp}      # MinHook setup: submit / line-append / per-frame
  bridge.{h,cpp}     # CreateProcess + pipe shell bridge
  queues.{h,cpp}     # thread-safe inbound/outbound queues
  sigscan.{h,cpp}    # pattern scanning for resilient hooking
/notes
  offsets.md         # confirmed addresses + signatures
  re-log.md          # RE findings as you go
CMakeLists.txt
```

## Milestones

- **M0 — prove output injection (do this first; de-risks the whole project).** Inject a DLL that does nothing but print `hello from the void` into the terminal scrollback on a keypress. If arbitrary text appears in the scrollback, the hard part (find + call line-append) is solved and everything else is plumbing.
- **M1** — capture the submitted command string (submit-handler hook); log it to a file.
- **M2** — wire the shell bridge: run a captured command in PowerShell, log output to a file.
- **M3** — full loop with the threading model: in-game command → shell → output rendered in scrollback.
- **M4 (optional)** — persistent shell session; ANSI / exit-code handling; signature-scan hardening.

## Constraints / gotchas

- **OFFLINE / SINGLE-PLAYER ONLY.** BO1 multiplayer ran **VAC**; injecting into a VAC-secured session is a ban. The terminal is a campaign/menu feature — there is no reason to be online. Develop and run with networking off.
- **Offsets are fragile** across game updates → prefer signature scanning over hardcoded addresses.
- **Arbitrary command execution by design.** This runs whatever is typed in-game as a host shell command. Personal, single-machine, novelty use only. If an IPC/bridge channel is ever added, **bind to localhost only** — never expose it.
- **Command injection** is inherent in concatenating input into `-Command "..."`. Acceptable for a personal novelty; do not reuse this pattern anywhere networked.

## Reference implementations (crib from these)

- **Shell Bridge** (Minecraft mod, Modrinth) — working reference for the **shell-bridge half**: in-game text → real PowerShell/bash → output back in-game. The easy 80%, already done on an easier-to-mod game.
- **CLI-Anything `STS2_Bridge`** (GitHub: `HKUDS/CLI-Anything`) — pattern for an **in-game bridge mod + localhost HTTP/IPC** talking to an external helper. Relevant if taking the bridge-server route.
- **BlackOpsTerminalEmulator** (GitHub: `whampson/BlackOpsTerminalEmulator`) — standalone recreation of *this exact terminal*; its source reveals the **command table and response behavior**. Use as a map for which strings to XREF and what each command should do.
- ComputerCraft / OpenComputers — *contrast only*: in-game terminals embedding a **sandboxed** Lua VM, deliberately walled off from the host. Not a model (we want the opposite); useful only for understanding why a real-host-shell bridge is the rarer thing.

## Alternative path — Plutonium (investigate before committing to pure RE)

Plutonium hosts BO1 as "t5" and loads **GSC** scripts. GSC is **sandboxed** — no process spawn, no host file I/O — so GSC alone cannot reach a shell. **But** if Plutonium exposes a **native plugin API** or **GSC file/IPC** primitives, the whole project gets easier: capture the command in GSC → write to a named pipe / dvar → a small native helper runs the shell → writes output back → display via GSC's **own** print functions (so you never have to hand-find line-append).

**UNKNOWN — verify in Plutonium docs / Discord:** whether T5 exposes native plugins or GSC file/IPC. If yes, this likely beats the manual-hook path. If no, fall back to the inline-hook plan above.

## Offsets (fill in as confirmed — keep signatures, not just addresses)

| Symbol | Address (build-specific) | Signature / pattern | Convention | Notes |
|---|---|---|---|---|
| `submit_handler` | TBD | TBD | TBD | where is the input string assembled? |
| `line_append` | TBD | TBD | TBD (likely thiscall) | argument list: ? |
| `frame_tick` | TBD | TBD | TBD | confirm it runs every frame |
