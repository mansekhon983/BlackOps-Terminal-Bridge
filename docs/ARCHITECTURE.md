# Architecture & Reverse Engineering

How a 2010 game's hardcoded easter-egg terminal was turned into a real Windows
shell — with no source, no symbols, and no debug information.

This is the technical companion to the [README](../README.md). The chronological
record lives in [`notes/re-log.md`](../notes/re-log.md); the curated address table
in [`notes/offsets.md`](../notes/offsets.md); a longer typeset write-up, including
the full error-and-fix log, in [`docs/report.tex`](report.tex).

---

## 1. The problem

*Call of Duty: Black Ops* (2010, Treyarch engine codename **t5**, an idTech 3 / IW
derivative) hides a "CIA terminal" in its main menu. It is a **pure simulation**: a
fixed command table, a fake filesystem backed by two CSVs, scripted string
responses, plus a complete embedded Zork and Dead Ops Arcade. It has **zero I/O to
the host**, and no config flag unlocks real behaviour.

The goal was authenticity — the shell had to run **inside the real game process**
and render through **the game's own text renderer**, not in a themed look-alike
window. That constraint is what makes the project interesting: every character of
output has to be produced by engine code that was never designed to be driven from
outside.

## 2. The target binary

| Fact | Value |
|---|---|
| Binary | `BlackOps.exe`, 8,099,608 bytes |
| Machine | `0x014C` — **i386 / 32-bit x86** (PE32, magic `0x10B`) |
| Sections | 7 |
| PE ImageBase | `0x00400000`, relocated at load (ASLR) |
| ⇒ DLL target | **x86 / Win32** (a 64-bit DLL cannot load into it) |
| ⇒ Debugger | **x32dbg** |

Verifying bitness was step zero: it determines the debugger, the DLL target, and
the calling convention of every function below. Because the module is relocated,
**signatures must wildcard any 4-byte absolute operand** or the scan misses after
ASLR — the RVAs recorded in `notes/offsets.md` are base-independent, and hooks
resolve by signature wherever a unique one exists.

## 3. Reverse-engineering methodology

The way into a closed binary like this is its own **hardcoded response strings** —
they are the only part of the terminal whose meaning is known in advance.

1. **Ghidra → Defined Strings.** Find a known terminal response (`HELP` output, a
   Zork line, the `USER:`/`PASSWORD:` prompts). 116 candidates were exported.
2. **Follow the cross-references.** The code referencing a response string *is* the
   command-dispatch logic, or one hop from it.
3. **Breakpoint it live** in x32dbg, type the matching command in-game, read the
   registers and stack at the break.
4. **Walk the call stack.** *Up* to where the raw input line is assembled → the
   submit handler. *Down* to where a response string is handed to rendering → the
   line-append routine.

All static recon was scripted against Ghidra **headless** (`analyzeHeadless`, with
`-noanalysis` against the already-analysed project) so every pass was cheap to
re-run as understanding improved. Those scripts are in
[`ghidra_scripts/`](../ghidra_scripts/) — `DumpTerminalStrings`, `FindTermRender`,
`ReconTermEmu`, `ReconInput`, `DumpDraw`, `DumpEmit`, `DumpTimer` — and their raw
decompiler output is committed under [`notes/dumps/`](../notes/dumps/), so the
findings below are reproducible rather than asserted.

Calling conventions were read off **how the game itself calls each function**
(argument order, register vs. stack, who cleans the stack) and mirrored exactly. A
mismatch corrupts the stack instantly, and two of the early crashes were precisely
that.

## 4. The engine surface that was recovered

### Functions

| Symbol | RVA | Convention | Role |
|---|---|---|---|
| `line_append` (`FUN_004ef440`) | `0xEF440` | `__cdecl(const char* text, int color, int flag, int zero)` | The authentic scrollback renderer. Parses `^N` colour codes, wraps at 80 cols, scrolls at 21 rows, emits glyphs via `FUN_004165b0`. **Called, not hooked** — this was "the hard 20%". |
| `line_commit` (`FUN_00433720`) | `0x33720` | `__cdecl(void)` | Advances the scrollback to the next line; the game calls it after every `line_append`. |
| `dispatch_submit` (`FUN_0055f800`) | `0x15F800` | `__cdecl(ctx)` | The command dispatcher. Fires on Enter, **on the main thread**. Reads the typed line from `DAT_00bf8130`, tokenises, linear-scans a `{name, handler}` table, else prints *Unknown Command*. **Hooked** — capture and route. |
| `frame_render` (`FUN_00575be0`) | `0x175BE0` | `__cdecl`, 6 dwords | The per-frame terminal renderer. First half = typewriter reveal + the `--MORE--` pager; second half = the per-cell grid draw loop. **Hooked** as the per-frame pump. |
| `glyph_run_draw` (`FUN_005f4a90`) | `0x1F4A90` | — | Draws one run of glyphs: measure + justify + emit. **Hooked** to override per-run colour and to suppress the native caret. |
| `glyph_emit` (`FUN_006d65c0`) | `0x2D65C0` | — | The vertex emitter. `strlen` + `memcpy` per run into one shared, size-capped per-frame buffer. Source of the culling quirk in §7. |
| `cell_write` (`FUN_00422d30`) | `0x22D30` | `(ch, color, flag)` | Writes one glyph + attribute at the grid cursor and advances it; `\n` scrolls. |
| `reveal_timer` (`FUN_0062ef60`) | `0x22EF60` | — | `Sys_Milliseconds()` = `timeGetTime() - start`. Pins the native reveal rate at **≈1 glyph/ms**. |
| `key_handler` (`FUN_0047eb90`) | `0x7EB90` | `(ctx, keycode, down)` | The terminal key handler. Recovered, understood, and deliberately **not used** — see §8. |

### Display globals

The decisive discovery: the terminal's display is **a real 2D character grid in
engine memory**, not an opaque text buffer. That is what makes a full emulator
possible at all.

| Global | RVA | Meaning |
|---|---|---|
| `DAT_00bf56a0` | `0x7F56A0` | **Char grid.** Row-major, stride `0x50` (80 cols), 27 rows. |
| `DAT_00bf44d8` | `0x7F44D8` | **Attribute grid.** Byte = `(color << 4) \| flag`; high nibble selects cell colour. |
| `DAT_00bf812c` | `0x7F812C` | Linear grid cursor = `col + row * 0x50`. |
| `DAT_00bf8120` | `0x7F8120` | Lines *written* — write head of a 100-row ring. |
| `DAT_00bf8124` | `0x7F8124` | Lines *revealed* — chases the write head; this is the typewriter. |
| `DAT_00bf8130` | `0x7F8130` | The raw typed command line. |
| `DAT_00a321c4` | `0x6321C4` | The game's own input-line text. In **`.rdata`** — writing it crashes (§9). |
| `DAT_02910160` | `0x2510160` | Key-catcher bitmask (idTech `cl_keyCatchers`); bit `0x100` = terminal text input. |

## 5. System architecture

`bo1bridge.dll` (x86, static CRT) is loaded by a small custom injector
([`tools/inject.cpp`](../tools/inject.cpp): `CreateRemoteThread` + `LoadLibrary`).
On attach it installs inline hooks with [MinHook](https://github.com/TsudaKageyu/minhook).

Two modes, each toggled by typing a word in the in-game terminal:

- **`emu` — emulator mode** (the real project). A ConPTY shell drives a VT parser
  and a character grid that is blitted into engine display memory every frame.
  Full-screen interactive programs run here.
- **`connect` — line-shell mode** (the original milestone). Each typed command runs
  in a persistent shell and its output is rendered with the engine's own
  `line_append`, inheriting the authentic font, wrapping, typewriter reveal and
  `--MORE--` pager for free. Kept because it is the simplest proof the bridge
  works, but it cannot host interactive programs — which is why the emulator
  exists.

### Threading model

Engine functions may only be touched on the **main thread**; calling them from a
worker crashes sporadically and non-deterministically. The whole design exists to
keep every engine call on the per-frame hook:

```
submit hook ──▶ [inbound queue] ──▶ worker / reader thread ──▶ shell or ConPTY
                                            │
                                            ▼
per-frame hook ◀── drain ◀── [outbound queue] / grid model ◀── output bytes
   │
   └── renders ON THE MAIN THREAD ONLY (line_append, or the grid blit)
```

In emulator mode the reader thread only *buffers* raw pty bytes; the frame hook
(`emuPump`) meters them into the parser and then blits. The grid model is
mutex-guarded for the batch, so the blit always reads a consistent snapshot.

## 6. The emulator pipeline

```
ConPTY child (PowerShell) ─▶ reader thread ─▶ vt::Parser ─▶ grid::Screen
                                                               │
                              engine renderer ◀── emuBlit() ◀──┘   (per frame)

input: WndProc subclass ─▶ VT byte translation ─▶ ConPTY stdin
```

**ConPTY backend** ([`src/conpty.cpp`](../src/conpty.cpp)) — a Windows
pseudoconsole makes the child believe it is on a real terminal, so it emits VT/ANSI
and accepts VT input: exactly the byte stream an emulator parses.
`CreatePseudoConsole` / `ResizePseudoConsole` / `ClosePseudoConsole` are resolved
dynamically from `kernel32` so the build does not depend on SDK header versions.
The pseudoconsole is attached to the child through a `STARTUPINFOEX`
`PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` attribute. The shell is **PowerShell 7 if
present, else Windows PowerShell 5.1, else `cmd.exe`**, launched in the user's home
directory with UTF-8 forced on the console.

**VT/ANSI parser** ([`src/vtparse.cpp`](../src/vtparse.cpp)) — a ground / escape /
CSI state machine covering:

- *Cursor and edits:* CUP, CUU/CUD/CUF/CUB, ED, EL, ECH, ICH, DCH, IL, DL, VPA,
  HPA, RI, the DECSTBM scroll region, and DECSC/DECRC save-restore.
- *Colour (SGR):* the 16 ANSI colours, 256-colour (`38;5;n`) and truecolour
  (`38;2;r;g;b`) — all resolved to a **24-bit RGB** foreground per cell (§7).
- *Codepage:* the engine font atlas is **ISO-8859-1**, so `U+00A0..U+00FF` pass
  straight through (accents, symbols). Box-drawing and block glyphs have no font
  entry and are approximated in ASCII (box-drawing → `| - + =`, blocks → `# : .`,
  braille spinners → `*`).
- *Query replies:* DSR (`ESC[6n`) and DA (`ESC[c`) are answered through a responder
  callback that writes back to the pty. Without this, size detection stalls (§9).
- Alternate screen (`?1049h` / `?47h`).

**Grid model** ([`src/grid.cpp`](../src/grid.cpp)) — `grid::Screen` is a grid of
`{char, rgb}` cells with a cursor, current SGR colour, scroll region, saved-cursor
slot, an **8000-row** scroll-back, a view anchor for paging, and an alt-screen
flag. One subtle but load-bearing member is `wrapPending_`, the deferred-wrap
(DECAWM) flag — see §9.

The display is **one unified windowed view**: the grid and the PTY height reported
to applications are both 8000 rows, and the engine's 27 visible rows window over
it, bottom-anchored, with large blank gaps collapsed. That design came out of a
measurement, not a guess — offline replay through
[`tools/feed_log.cpp`](../tools/feed_log.cpp) showed that Claude Code clips every
render to the reported height and repaints in place, producing **zero** scroll-back
of its own (history peak = 0). So the tall viewport *is* its scrollback.

## 7. Renderer internals — the two load-bearing quirks

Understanding `frame_render`'s second half (the per-cell draw loop) was what made
the emulator work. Two of its behaviours dictate the entire layout of the blit, and
both were found by reading the decompiler output rather than by guessing at
symptoms.

**(a) NUL-terminated rows and a shared vertex budget.** `glyph_emit` reads each run
as a **NUL-terminated C string** (`strlen` + `memcpy`, ignoring the count it is
passed) into *one* shared per-frame text-vertex buffer with a hard size cap. When
that buffer fills to within `0x2000` of its end, it emits **nothing for the rest of
the frame**. A row without a NUL is therefore read ~2 KB past its end, and a
screenful of such runs exhausts the budget — after which every later run that frame
is silently dropped and *the bottom rows simply vanish*. This was the hardest bug
in the project: it presented as "the last few lines are missing", with no error
anywhere.

*Fix:* every grid row is NUL-terminated after its last glyph; blank rows are
all-NUL, which the loop skips. Content is 79 columns so column 79 is always the
terminator.

**(b) Run colour comes from the boundary cell.** The draw loop splits a row into
runs of equal attribute and colours each run by the attribute of the cell that
**ends** the run, not by the run's own cells. Painting every cell its true colour
therefore bleeds each segment's colour onto its neighbour.

*Fix:* an inverted "filler" scheme — within a colour segment only the **last** cell
carries the real colour, and the interior cells get the *previous* segment's colour
(guaranteed to differ, since adjacent segments differ by definition), so the
engine's run boundary lands exactly on the segment end. Spaces inherit the
preceding glyph's colour, so a lone one-cell border becomes a colourable two-cell
segment. The terminating NUL cell's attribute carries the edge colour — the engine
reads that attribute for the run colour, but `strlen` never draws the NUL — so the
off-by-one is neutralised **without spending a visible column**.

**24-bit colour, bypassing the engine palette.** The engine exposes only 8 colour
slots through the attribute nibble. Rather than live with that, colour is routed
around it entirely: each cell carries a true 24-bit foreground, `emuBlit` fills a
parallel `g_trueRGB` grid and assigns attribute nibbles **purely as run-break IDs**
(cycling `1..7`, so the renderer still splits runs correctly), and the
`glyph_run_draw` hook overrides the run's colour argument with an RGBA built from
`g_trueRGB`. This is safe because the engine copies the colour synchronously per
run. A sentinel distinguishes "terminal default" from real white; pure black folds
to white (black-on-black would vanish) and bright-black comes through as real grey,
which is what makes dark PSReadLine parameter text readable in-game.

**Typewriter reveal.** `reveal_timer` confirmed the native terminal reveals output
at ≈1 glyph/ms, paced by `timeGetTime`. `emuPump` matches that rate exactly,
counting *visible glyphs* (`Screen::produced()`), so shell output appears with the
same cadence as the game's own text. Full-screen TUIs would be unusable at that
rate, so they are detected (alt-screen, or the stream moving the cursor *up* —
`Screen::repaint()`) and rendered instantly; `Ctrl+\` forces instant mode always.

## 8. Input

Static recon ([`ReconInput.java`](../ghidra_scripts/ReconInput.java)) showed the
game's keyboard input is **Windows-message driven** — it imports `RegisterClass`,
`DefWindowProc`, `CallWindowProc` and the
`Get`/`Peek`/`Translate`/`DispatchMessage` family, with **no DirectInput**
anywhere. That made the engine's own `key_handler`/`key_dispatch` path, which had
already been reversed, the *worse* option: it splits printable characters off into
a dvar-mediated char event and is awkward to drive.

So the game window is **subclassed** instead (found at runtime via `EnumWindows`
filtered to this PID, visible, titled). In emu mode `emuWndProc` takes clean
`WM_CHAR` characters and `WM_KEYDOWN` special keys, translates them to VT input
bytes (arrows → `ESC[A..D`, Enter → `\r`, Backspace → `0x7f`, PgUp/PgDn, Ctrl
combinations), writes them to the pty, and **swallows** the message so the game
takes no action — no player movement, no ESC pause menu. Because `TranslateMessage`
has already produced the `WM_CHAR` by that point, swallowing `WM_KEYDOWN` does not
lose the character. `WM_MOUSEWHEEL` scrolls the history. `Ctrl+]` leaves emu mode.

## 9. Notable findings

- **The display is a real character grid.** Everything else followed from this. The
  stock terminal writes glyph + attribute pairs into two plain arrays at
  `DAT_00bf56a0` / `DAT_00bf44d8` — so a terminal emulator can simply *be* the
  thing that fills them.
- **The game's own caret lives in read-only memory.** The native input-line text is
  in `.rdata`, and its x-position is the *previous* row's pixel width (the x
  accumulator is never reset), so it drifts right as our bottom row grows. Writing
  the string to blank it crashes. It is suppressed instead by hooking
  `glyph_run_draw` and skipping the single call whose text pointer equals that
  address.
- **`cls` emits no `ED`.** `cls` under ConPTY produces no `ESC[2J`/`ESC[3J` — it
  does `ESC[?25l ESC[H` then ~26× `ESC[K CR LF`. Clearing the scroll-back therefore
  requires detecting a near-full-screen run of blank erase-then-linefeed. Claude
  Code's full-frame repaint emits a **byte-identical** pattern, so the two are
  indistinguishable in principle — and harmless in practice, since it never
  scrolls.
- **Deferred wrap (DECAWM) is not optional.** Wrapping eagerly at column 80 — the
  obvious implementation — causes cumulative drift and duplicated lines in any app
  that writes to the last column. The cursor must stay on the last column with a
  *pending* wrap flag that resolves only on the next printable character.
- **Unanswered terminal queries stall startup.** A child that sends DSR or DA and
  gets no reply blocks for ~15 seconds before falling back. Size detection has to
  be answered, not ignored.
- **A non-unique signature froze the game.** The first live test hung because the
  `line_commit` pattern matched ten call sites. Signature uniqueness has to be
  *verified*, not assumed — which is why every signature in `notes/offsets.md`
  records the anchor that makes it unique (for `line_commit`, a reciprocal-divide
  magic constant).

The full error-and-fix log — roughly twenty distinct failures, each with its root
cause — is in [`docs/report.tex`](report.tex) under *Errors encountered and how
they were fixed*; the chronological version is in
[`notes/re-log.md`](../notes/re-log.md).

## 10. Current state

Working end to end. A real PowerShell runs inside the Black Ops terminal with
24-bit colour, an 8000-line scroll-back with mouse-wheel scrolling and a visual
scrollbar, Latin-1 text, and live keyboard input. `ssh`, the Python REPL, `vim` and
`htop` all render. Claude Code itself runs in-game — welcome box, colours, live
spinner, keyboard input and all — which was the original stretch goal.

Known limits: box-drawing and block glyphs are ASCII approximations (the font atlas
has no entries for them); background colour is parsed but not rendered (an engine
cell stores a foreground only); and offsets are resolved against this specific build
of the game, so an update can break them.

## 11. Source map

| Path | What's in it |
|---|---|
| [`src/hooks.cpp`](../src/hooks.cpp) | MinHook setup, the hooks, `emuBlit`, `emuPump`, the WndProc subclass. The heart of the project. |
| [`src/conpty.cpp`](../src/conpty.cpp) | Pseudoconsole session: create, attach, launch, resize, read. |
| [`src/vtparse.cpp`](../src/vtparse.cpp) | VT/ANSI state machine. |
| [`src/grid.cpp`](../src/grid.cpp) | Screen model, scroll region, scroll-back, view anchoring. |
| [`src/bridge.cpp`](../src/bridge.cpp) | `connect` line-shell mode: persistent shell over pipes. |
| [`src/sigscan.cpp`](../src/sigscan.cpp) | Pattern scanning with operand wildcards. |
| [`tools/inject.cpp`](../tools/inject.cpp) | `CreateRemoteThread` + `LoadLibrary` injector. |
| [`tools/feed_log.cpp`](../tools/feed_log.cpp) | Offline replay harness — feeds a captured pty log through the parser and grid with diagnostics. How most render bugs were actually diagnosed. |
| [`tools/vtparse_test.cpp`](../tools/vtparse_test.cpp), [`tools/conpty_test.cpp`](../tools/conpty_test.cpp) | Standalone validation of the parser and the ConPTY backend outside the game. |
| [`ghidra_scripts/`](../ghidra_scripts/) | Re-runnable headless recon scripts. |
| [`notes/`](../notes/) | `offsets.md` (address table), `re-log.md` (chronological log), `dumps/` (raw decompiles). |
