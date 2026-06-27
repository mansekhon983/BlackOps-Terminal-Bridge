# Offsets & Signatures — BlackOps.exe

> Record every confirmed address here, but **hook via signature, not address** —
> offsets break on any game update.

## Binary facts (confirmed)

| Fact | Value |
|---|---|
| Path | `C:\Games\Call of Duty - Black Ops\BlackOps.exe` |
| Size | 8,099,608 bytes |
| Machine | `0x014C` (i386 / **32-bit x86**) |
| Optional header magic | `0x10B` (PE32) |
| Sections | 7 |
| → DLL target | **x86 / Win32** |
| → Debugger | **x32dbg** |

## Image base

- PE ImageBase: **0x00400000**. Module is relocated at load (ASLR), so absolute
  address operands inside instructions get patched at runtime. **Signatures must
  wildcard any 4-byte absolute operand** (marked `?? ?? ?? ??` below), or the
  scan will miss after relocation. RVAs below are base-independent.

## Functions — CONFIRMED (2026-06-27, via Ghidra)

### `line_append` = `FUN_004ef440`  (RVA `0x000EF440`)  ★ the hard 20%, solved
- **Convention:** `__cdecl`. Prototype mirrored from call sites:
  `void line_append(const char* text, int color /*=1*/, int flag /*=1*/, int zero /*=0*/)`
  (4 dwords pushed per call; caller cleans `ADD ESP,0x40` after 4 calls = cdecl).
- **What it does:** renders `text` into the terminal scrollback. Parses `^`
  color codes, wraps at 80 cols (`0x50`), scrolls at 21 lines (`0x15`), draws each
  glyph via `FUN_004165b0(ch, color, flag)`. Line counter global `DAT_00bf8120`,
  cell buffer `DAT_00bf5f10`.
- **We CALL this (don't hook it).** Always call `line_commit` after.
- **Signature (no abs operands in prologue — exact is safe):**
  `83 EC 08 8B 44 24 0C 55 56 33 F6 89 74 24 08 33 ED 8D 50 01 8A 08 40 84`

### `line_commit` = `FUN_00433720`  (RVA `0x00033720`)
- **Convention:** `__cdecl`, no args. Advances to the next scrollback line;
  the game calls it after every `line_append`.
- **Signature (wildcard the `[0xBF8120]` operand):**
  `8B 0D ?? ?? ?? ?? B8 1F 85 EB 51 F7 E9 C1 FA 05 8B C2 C1 E8 1F 03 D0 6B`
  (the `B8 1F85EB51` reciprocal-divide magic is a strong unique anchor.)

### `dispatch_submit` = `FUN_0055f800`  (RVA `0x0015F800`)  ★ submit hook point
- **Convention:** `__cdecl`, one stack arg (a UI/terminal context, passed through
  to each command handler).
- **What it does:** the terminal command dispatcher. Reads the typed line from
  global input buffer **`DAT_00bf8130`**, tokenizes via `get_input_token`
  (`FUN_00566b30`), linear-scans the command table comparing with strcmp
  (`FUN_00552bf0`), calls the matching handler, else prints
  `"^3Error: Unknown Command - try \"help\""`. When a login shell is active it
  routes input to the shell handler stored at `DAT_00bf4d58 + DAT_00bf8234*0x14`.
- **Command table:** `{const char* name, void(*handler)(ctx, inbuf)}` pairs,
  base `PTR_s_alarm_00b71540` (names) / `PTR_FUN_00b71544` (handlers), stride
  `0x8`, count `DAT_00b71608`. First entry is `"alarm"`.
- **Runs on the main/engine thread** → safe place to call `line_append`. This is
  the M0 + M1 hook.
- **Signature (wildcard 3 abs operands):**
  `83 EC 08 A1 ?? ?? ?? ?? 8D 04 80 8B 04 85 ?? ?? ?? ?? C7 04 24 ?? ?? ?? ??`

### Supporting routines (named, not hooked)
| Symbol | Address | Role |
|---|---|---|
| `get_input_token` | `FUN_00566b30` | resolves the active terminal's current input line into a `char*` |
| `cbuf_exec` | `FUN_0049b930` | console-exec (Cbuf_AddText-like): `(ctx, "cmd...\n")` |
| `strcmp_term` | `FUN_00552bf0` | string compare used by the dispatcher |
| `login_handler` | `FUN_005545e0` | login/shell state machine (`USER:`/`PASSWORD:`) |
| `term_fs_load` | `FUN_005aa2a0` | loads `terminal_resources/systems.csv` + `users.csv` |
| `glyph_draw` | `FUN_004165b0` | per-character draw into the cell buffer |
| Input buffer | `DAT_00bf8130` | global: the raw typed command line |
| Line counter | `DAT_00bf8120` | global: current scrollback line index |

### Still TBD
| Symbol | Status | Notes |
|---|---|---|
| `frame_tick` | not yet located | per-frame pump needed for **M3** (async shell output without typing). NOT needed for M0/M1 — those call `line_append` directly from the main-thread submit hook. |

## Terminal strings used as XREF entry points

See `re-log.md`. List the exact strings found in Ghidra's Defined Strings and the
address each XREF lands at.
