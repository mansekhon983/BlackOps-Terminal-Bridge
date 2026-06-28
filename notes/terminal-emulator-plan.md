# Full Terminal Emulator inside BO1 — design & milestone plan

> Goal: make the in-game BO1 terminal behave like a *real* terminal — run
> interactive, full-screen, cursor-addressed programs (vim, less, htop, nano,
> python REPL, interactive ssh) rendered inside the game's own terminal screen,
> using the same on-screen interface. This supersedes the line-shell bridge
> (M0–M4); that bridge stays as a simpler "line mode" the user can toggle to.
>
> Feasibility recon completed 2026-06-28 (see re-log session 8, notes/emu-recon.txt).
> **Verdict: green — no architectural blockers.**

## Why it's feasible (the three load-bearing facts)

1. **The display layer is already a 2D grid.** Under `line_append` there is an
   addressable char grid + attribute grid the renderer paints every frame:
   - `DAT_00bf56a0` = char grid, `DAT_00bf44d8` = attribute grid, width `0x50`
     (80 cols), ~27 rows (linear offset caps at `0x86f`); bottom row is the
     input line → ~**80×26 usable**.
   - `DAT_00bf812c` = linear cursor (`col + row*0x50`) — gotoxy already exists.
   - cell writer = `FUN_00422d30(ch, color, flag)`:
     `grid_chars[cursor]=ch; grid_attrs[cursor]=(color<<4)|flag; cursor++`.
   - `\n` path scrolls via `FUN_005cc680(1,0x1b,1,0x50,1)` — reuse for scroll-up.
   - Per-cell color: attribute high nibble → text color (~16-entry palette).

2. **Per-cell color exists.** `(color<<4)|flag`; renderer decodes high nibble to
   the cell's text color. Enough for ANSI-16 (map to nearest engine palette
   entry). No bold/underline (reverse can be faked); no truecolor.

3. **One terminal-scoped hook captures every key AND suppresses it.**
   `FUN_0047eb90(ctx, keycode, down)` is the terminal key handler. It already
   receives printable chars, `0x7f` backspace, `0xd` Enter (where it calls
   `dispatch_submit`), `0x9a`/`0x9b` up/down arrows (history), and explicitly
   checks `0x1b` ESC / `0x03` Ctrl-C. Because it only fires when the terminal has
   focus, we need no focus detection. In emu mode: translate keycode → VT input,
   write to the PTY, `return` (swallow) so the game does no line-editing and ESC
   doesn't open the pause menu.

## Architecture

```
ConPTY child (bash / vim / python / ssh)
   │ bytes out (text + VT/ANSI escapes)            [worker thread: read pipe]
   ▼
VT parser ──► our 80×26 grid model (cells: glyph + colour; cursor; modes)
   ▲                                   │
   │ VT input bytes                    ▼  per-frame, MAIN thread (renderer hook)
keycode→VT  ◄── hook FUN_0047eb90    blit grid → DAT_00bf56a0 / DAT_00bf44d8
   (swallow the key)                   (keep reveal==write so the typewriter/ring
                                        half of FUN_00575be0 stays dormant)
```

- **Backend:** replace the cmd-pipe bridge with **ConPTY** (`CreatePseudoConsole`
  + pipes). Spawn the child attached to the pseudoconsole, fixed size 80×26.
- **Grid model + VT parser:** own module. Parse the xterm subset that matters:
  SGR colours, cursor moves (CUP/CUU/CUD/CUF/CUB), erase (ED/EL), scroll region
  (DECSTBM), alt-screen (`?1049h/l`), `\r`/`\n`/`\t`/`\b`, line wrap, autowrap.
  Adapt a compact open-source parser rather than writing from scratch.
- **Render frontend:** each frame, on the main thread (existing `frame_render`
  hook on `FUN_00575be0`), copy the 80×26 grid into the engine char/attr grids
  and draw the cursor cell. Pin `DAT_00bf8124 == DAT_00bf8120` so the reveal/ring
  machinery (first half of the renderer) does nothing.
- **Input frontend:** hook `FUN_0047eb90`; in emu mode map keycode→VT bytes
  (printable as-is; arrows→`ESC[A/B/C/D`; Enter→`\r`; Backspace→`0x7f`;
  ESC→`0x1b`; Ctrl-C→`0x03`; Tab→`\t`) into the PTY input queue and return.
- **Threading:** PTY read on a worker (as today) → grid mutated under a lock;
  blit + cursor on main thread in the frame hook; key capture on whatever thread
  `FUN_0047eb90` runs (main). Mirrors the existing model.
- **Mode toggle:** keep `connect` for the existing **line mode**; add a separate
  command (e.g. `raw` / `pty`) to enter **emulator mode**; exiting the child (or
  a hotkey) returns to the game terminal.

## Milestones

- **E0 — ConPTY echo.** Swap backend to ConPTY; pipe child stdout straight to the
  existing line renderer (no grid yet). Proves the pseudoconsole + child spawn.
- **E1 — static grid render.** Stand up the 80×26 grid model; blit it into
  `DAT_00bf56a0`/`44d8` each frame with reveal pinned; render a hard-coded test
  pattern + colours. Proves direct-grid rendering and the palette mapping.
- **E2 — VT parser drawing.** Feed child output through the VT parser into the
  grid. Target: `ls --color`, a `printf` cursor-move test, then **vim opens and
  draws** (alt-screen, cursor, status line). Read-only (no input yet).
- **E3 — input flows.** Hook `FUN_0047eb90`; map keycodes→VT; type into vim,
  move with arrows, `:wq`. Confirm ESC/Ctrl-C reach the child, not the game.
  Pin down the rest of the keycode table (left/right, Home/End, Tab, Ctrl-combos).
- **E4 — real use.** python REPL, htop, less paging, interactive `ssh user@host`
  login. Polish: scroll region, tab stops, cursor visibility, exit handling.
- **E5 (optional) — fidelity.** reverse-video, better ANSI→palette mapping,
  bracketed paste; decide if any of mouse/truecolor is worth a D3D-overlay path.

## Open items to confirm live (not blockers)

- Full engine keycode table beyond `0x9a/0x9b/0x7f/0xd/0x1b/0x03`
  (left/right arrow, Home/End/Tab/Del, Ctrl-letter combos, F-keys).
- That pinning `reveal==write` fully disables the typewriter/`--MORE--` half of
  `FUN_00575be0` without breaking the input-line/footer draw.
- Whether the second input writer `FUN_00508230` (a separate, restricted input
  path — uppercasing, per-char) ever fires for the terminal, or only for
  Zork/DOA; if it does, it needs the same emu-mode short-circuit.
- Engine palette: exact colours of the ~16 attribute entries (for ANSI mapping).

## Confirmed addresses (see offsets.md for the canonical table)

| Symbol | Addr | Role |
|---|---|---|
| `cell_write` | `FUN_00422d30` | write ch+attr at cursor, advance |
| `grid_chars` | `DAT_00bf56a0` | 80×~27 char grid |
| `grid_attrs` | `DAT_00bf44d8` | 80×~27 attribute grid, `(color<<4)|flag` |
| `grid_cursor` | `DAT_00bf812c` | linear cursor `col+row*0x50` |
| `scroll_up` | `FUN_005cc680` | scroll the grid up one line |
| `key_handler` | `FUN_0047eb90` | terminal key handler `(ctx, keycode, down)` — input hook |
| `key_handler2` | `FUN_00508230` | secondary/restricted input path (verify scope) |
