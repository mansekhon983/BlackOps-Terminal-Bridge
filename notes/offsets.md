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

### `frame_render` = `FUN_00575be0`  (RVA `0x00175BE0`)  ★ per-frame pump (FOUND)
- **Convention:** `__cdecl`, 6 dword-sized args (forwarded verbatim through the hook).
- **What it does:** the terminal's per-frame renderer. Reveals queued rows over
  time (typewriter) by walking `DAT_00bf8124` (reveal head) toward `DAT_00bf8120`
  (write head); **case 5 = the `--MORE--` pager**. Resolved by RVA + prologue
  verify (ASLR off).
- **Verify bytes:** `A1 24 81 BF 00 83 EC 28 53 55 56 57 3B 05 20 81 BF 00`
- Hooked to drip shell output a few lines/frame, throttled by `write-reveal < 24`
  so the 100-row ring never overruns.

### Terminal render model (confirmed)
| Global | RVA | Meaning |
|---|---|---|
| `DAT_00bf8120` | `0x7F8120` | lines **written** (write head; `%100` ring of `0x56`-byte rows) |
| `DAT_00bf8124` | `0x7F8124` | lines **revealed** (reveal head; animated toward write head) |
| `DAT_00bf5f10` | `0x7F5F10` | row state buffer; `[row*0x56]`: 1=text, 5=`--MORE--` pager |
| `FUN_004165b0` | — | per-character cell writer (one glyph onto the current row) |

### Grid draw loop + glyph emitter (confirmed 2026-06-29, via `DumpDraw*.java` / `DumpEmit.java`)
> `frame_render`'s **second half** is the per-cell grid draw loop: it walks all 27
> rows of `grid_chars`/`grid_attrs` and draws them at screen lines 9..35. Two
> behaviours here dictate exactly how the emulator must lay out the grid (see the
> long comment in `emuBlit`, `src/hooks.cpp`). Decompiles: `notes/draw*.txt`, `emit.txt`.

| Symbol | Address | RVA | Role |
|---|---|---|---|
| `glyph_run_draw` | `FUN_005f4a90` | `0x1F4A90` | draws one run of glyphs: measure (`FUN_005d84f0`) + transform (`FUN_005cd7b0`) + emit (`FUN_0061c4d0`) |
| `glyph_emit` | `FUN_006d65c0` | `0x2D65C0` | **the glyph emitter.** Reads the run as a **NUL-terminated C-string** (`strlen`+`memcpy`, ignores the count) into ONE shared per-frame text-vertex buffer; **hard size guard**: when the buffer fills to within `0x2000` of the end it sets `buf[3]=0` and emits **nothing for the rest of the frame**. |
| `run_width` | `FUN_005d84f0` | `0x1D84F0` | run pixel width; parses `^N` colour codes (0 width) via `FUN_006d3110` |
| `xform_justify` | `FUN_005cd7b0` | `0x1CD7B0` | position/justify transform (no clip/cull — culling is the budget guard above) |

**Two emulator-load-bearing quirks of the draw loop (`FUN_00575be0` second half):**
1. **NUL-terminated rows + shared vertex budget.** Because `glyph_emit` reads to the
   first NUL and shares a capped buffer, a row with no NUL is read ~2KB past its end;
   a screenful of such runs exhausts the buffer and every later run that frame is
   dropped → bottom rows vanish. **Fix:** NUL-terminate each grid row after its last
   glyph (blank rows → all-NUL, which the loop skips).
2. **Run-colour off-by-one.** The loop splits a row into runs of equal `grid_attrs`
   and colours each run by the attr of the cell that **ends** the run (the boundary),
   not the run's own cells. **Fix (emuBlit "filler" scheme):** put the real colour
   only on a segment's last cell; interior cells = the previous segment's colour
   (always differs → boundary lands on the segment end); spaces inherit the preceding
   glyph's colour so a lone 1-cell border becomes a colourable 2-cell segment.

**Palette (calibrated live, `grid_attrs` high nibble):** `0`=white (default/native),
`2`=black, `3`=red, `4`=green, `5`=yellow, `6`=blue, `7`=magenta, `8`=cyan, `9..F`=white.
(`1` also renders ~default.) `ansiToEngine` maps ANSI white→`0`; `vtparse rgbToAnsi8`
sends low-saturation→white and saturated colours through a per-channel `>127`
threshold (keeps box orange red while the spinner's brightening orange flips red↔yellow).

### Terminal emulator layer (confirmed 2026-06-28, via `ReconTermEmu.java`)
> The display under `line_append` is a real 2D grid — the basis for the full
> terminal-emulator project. See `terminal-emulator-plan.md` + re-log session 8.

| Symbol | Address | RVA | Role |
|---|---|---|---|
| `cell_write` | `FUN_00422d30` | `0x22D30` | `(ch,color,flag)` → `grid_chars[cursor]=ch; grid_attrs[cursor]=(color<<4)|flag; cursor++`. case `\n`(10) resets cursor + scrolls |
| `grid_chars` | `DAT_00bf56a0` | `0x7F56A0` | char grid, width `0x50` (80), ~27 rows (offset ≤ `0x86f`); bottom row = input line → ~80×26 usable |
| `grid_attrs` | `DAT_00bf44d8` | `0x7F44D8` | attribute grid; byte = `(color<<4)|flag`; renderer decodes **high nibble → per-cell text colour** (~16 palette) |
| `grid_cursor` | `DAT_00bf812c` | `0x7F812C` | linear cursor = `col + row*0x50`; case 3 of renderer = gotoxy |
| `scroll_up` | `FUN_005cc680` | `0x1CC680` | scroll grid up one line (`(1,0x1b,1,0x50,1)`) |
| `key_handler` | `FUN_0047eb90` | `0x7EB90` | **terminal key handler `(ctx, keycode, down)`**. Sees `0x7f` backspace, `0xd`/`0xbf` Enter (calls `dispatch_submit`), `0x9a`/`0x9b` up/down arrows (history), checks `0x1b` ESC & `0x03` Ctrl-C. Printable chars arrive as `param_2==1` "char event" with the char from the `ui_keyboard_focus_key` dvar (idTech Key_Event/Char_Event split). Terminal-scoped. **NOT used for E3** — see below. |
| `key_dispatch` | `FUN_00569f70` | `0x169F70` | engine key-event funnel; when keycatcher `DAT_02910160 & 0x100` (terminal text input) is set it routes to `FUN_0047eb90`, else to the bind handler `FUN_00587500`. |
| `keycatcher` | `DAT_02910160` | `0x2510160` | key-catcher bitmask (idTech `cl_keyCatchers`); bit `0x100` = terminal/UI text input active. |
| `key_handler2` | `FUN_00508230` | `0x108230` | secondary/restricted input path (uppercases, per-char); Zork/DOA. |

### E3 input decision (recon 2026-06-28, `ReconInput.java` → input-recon.txt)
Game keyboard input is **Windows-message based** — imports `RegisterClassA/Ex`,
`DefWindowProcA`, `CallWindowProcA`, `GetMessageA`/`PeekMessageA`/`TranslateMessage`/
`DispatchMessageA`, `SetWindowLongA`; message pump = `FUN_006eb1f0`. **No DirectInput**
(only GDI `GetDeviceCaps`). → E3 hooks input via a **WndProc subclass** (clean
`WM_CHAR` chars + `WM_KEYDOWN` special keys, standard Win32 VK codes), NOT the
engine key-handler. Found the game HWND at runtime via `EnumWindows` (own PID,
visible, titled). Swallowing messages while in emu mode stops game actions / the
ESC pause menu (TranslateMessage already produced WM_CHAR in the pump, so swallowing
WM_KEYDOWN doesn't lose the char). Exit emu via **Ctrl+]** hotkey.

### Native input caret + reveal timer + cls pattern (confirmed 2026-06-30, `DumpTimer.java`, pty capture)
> Findings while adding the typewriter reveal, instant-TUI rendering, and `cls`
> scroll-back clearing. See re-log session 15.

| Symbol | Address | RVA | Role |
|---|---|---|---|
| `native_input_str` | `DAT_00a321c4` | `0x6321C4` | the game's own terminal input-line text, drawn by `frame_render` at screen line `0x23` via `glyph_run_draw(...,&DAT_00a321c4,0x7fffffff,...)` with blink alpha `param_5[3]`. Its x is the **previous row's pixel width** (`param_2` is not reset) — so the caret drifts right with our bottom grid row. **In `.rdata` (READ-ONLY): writing it crashes.** Suppressed in emu mode by hooking `glyph_run_draw` and skipping the call whose text ptr == this address. |
| `cursor_rect_count` | `DAT_00bf8230` | `0x7F8230` | count of cursor/selection rects `frame_render` draws (`if(0<DAT_00bf8230){...&DAT_00bf80c8...}`); in writable `.data`. (Not the drifting caret — that's the text above.) |
| `reveal_timer` | `FUN_0062ef60` | `0x22EF60` | `Sys_Milliseconds()` = `timeGetTime() - start`. `frame_render`'s typewriter half reveals `now_ms - last_ms` work-units/frame (1 char or 1 control entry each) → **native reveal rate ≈ 1 glyph/ms (~1000 cps)**. Emu `emuPump` matches this via `timeGetTime` (link `winmm`). |

**`cls` clear pattern (cmd via ConPTY):** `cls` emits **no `ESC[2J`/`ESC[3J`** — it does
`ESC[25l ESC[H` then **~26× `ESC[K<CR><LF>`** (erase each line + LF down), then repositions.
We detect a near-full-screen run of blank erase-then-linefeed in `grid::lineFeed` and drop
the scroll-back. NOTE: Claude Code's full-frame repaint emits the **identical** pattern
(`ESC[H` → 26× `ESC[K<CR><LF>` → `ESC[H` → redraw), so the two are indistinguishable at the
byte level (harmless: Claude has nothing in history to clear because it never scrolls).
Claude uses **no alt-screen, no `IND`/`RI`, only absolute `CUP`** to reposition → instant-TUI
detection keys off upward `CUP`/`CUU` (`Screen::repaint()`), and Claude produces **zero**
scroll-back (`feed_log` replay: history peak = 0) — the motivation for the tall-viewport work.

## Terminal strings used as XREF entry points

See `re-log.md`. List the exact strings found in Ghidra's Defined Strings and the
address each XREF lands at.
