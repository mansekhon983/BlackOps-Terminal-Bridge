# RE Log — BO1 Terminal Bridge

Chronological reverse-engineering findings. Newest at the bottom.

## 2026-06-27 — Session 1: recon

- Confirmed `BlackOps.exe` is **32-bit (x86)** via PE header (Machine 0x014C,
  PE32 magic 0x10B). → x86 DLL, x32dbg, thiscall/fastcall conventions likely.
- Located game install: `C:\Games\Call of Duty - Black Ops\BlackOps.exe`.
- Existing Ghidra project found at `C:\Ghidra\ghidra_projects\Black_Ops.gpr`
  (already imported & analyzed; ~9.6 MB program DB).
- Plutonium path: Plutonium T5 exposes a **native plugin API (V1)** with custom
  GSC builtins + a thread scheduler, and **GSC file IO** (`fs_fopen`, `fs_write`,
  `fs_writeline`, `fs_readline`, `fs_read`, `fs_testfile`, `fs_seek`,
  `fs_listfiles`, ...) sandboxed to `scriptdata/`, gated by dvar `scr_allowFileIo`
  (default on). See offsets/decision notes. OPEN QUESTION: the CIA terminal
  easter egg is engine-side hardcoded — unclear whether GSC can intercept its
  input. File IO gives the IPC channel for free regardless.

### Terminal response strings to XREF (candidates)

From the known BO1 terminal command set (cross-ref `whampson/BlackOpsTerminalEmulator`):
commands include `help`, `login`, `dir`, `cat`, `who`, `rlogin`, `mail`, `foobar`,
`3arc`, `doa`, `zork`, `cod`, `arpanet`, `alicia`, `amber`. Good string anchors:
HELP output text, Zork intro ("WEST OF HOUSE"), DOA unlock message, login prompts.

- [x] Export Defined Strings from the Ghidra project (headless) → 116 hits in
      `terminal_strings.txt`.
- [x] Followed help-screen string XREFs → found the help printer.
- [ ] Confirm in x32dbg (live): set BP at dispatch_submit (RVA 0x15F800), type a
      command, verify `DAT_00bf8130` holds the typed line and ctx convention.

## 2026-06-27 — Session 2: functions confirmed (static)

Done entirely from the existing Ghidra DB (headless, `-noanalysis`). See
`decomp.txt` / `sigs.txt` for raw output, `offsets.md` for the canonical table.

**line_append = FUN_004ef440 (RVA 0xEF440) — THE renderer. The hard 20% is solved.**
The HELP command printer (the unnamed function spanning ~0x551bf4..0x551d41) emits
every help line with the identical idiom:
```
PUSH 0 ; PUSH 1 ; PUSH 1 ; PUSH <str> ; CALL 0x4ef440 ; CALL 0x433720
```
So `line_append` is `__cdecl(const char* text, int=1, int=1, int=0)` and
`line_commit`=FUN_00433720 advances the line. line_append parses `^` colour codes,
wraps at 80 cols, scrolls at 21 rows, draws glyphs via FUN_004165b0 — i.e. it IS
the authentic terminal renderer, exactly what M0/M1/M3 need. No `this` pointer.

**dispatch_submit = FUN_0055f800 (RVA 0x15F800).** Terminal command dispatcher,
`__cdecl(ctx)`, main thread. Reads typed line from global `DAT_00bf8130`,
tokenizes (FUN_00566b30), strcmp-scans the `{name,handler}` table at
PTR_s_alarm_00b71540 (count DAT_00b71608, stride 8), else prints
`"^3Error: Unknown Command - try \"help\""`. Login shells route via
DAT_00bf4d58+idx*0x14. → our submit hook + the M0 anchor.

**Decision:** M0 no longer needs a separate frame hook. Hooking dispatch_submit
(main thread) and calling line_append directly from it both proves output
injection (M0) and captures the command (M1) in one hook. frame_tick is only
needed for M3 (async shell output without a keypress) and is still TBD.

Signatures (ASLR-safe, abs operands wildcarded) wired into `src/hooks.cpp`.

## 2026-06-27 — Session 3: first live test froze; root-caused

First injection: typed `help` in-game → **game froze** (hang, not crash). Diagnosed
statically (CheckDispatch.java), no live debugging needed:

- **Ruled out convention:** the engine calls dispatch_submit as
  `PUSH EAX; CALL 0x55f800; ADD ESP,4` — confirmed `__cdecl(ctx)`, as coded.
- **Root cause:** the `line_commit` signature was **not unique** — it matched **10**
  locations; `sigscan` returns the first (`0x00427aeb`), so we were calling into the
  middle of an unrelated function as a `void(void)` → stack/register chaos → freeze.
  The pattern is the compiler's generic `(x % 100) * 0x56` idiom on the global line
  counter `DAT_00bf8120`, which recurs across the terminal code. No byte signature
  can disambiguate it.
- **Fix:** `BlackOps.exe` has **no ASLR** (`DllCharacteristics=0x8000`, DYNAMIC_BASE
  clear) → always loads at `0x400000`. So `line_commit` is resolved by **RVA
  (0x33720) + prologue verification** (`sigscan::matches`, which VirtualQuery-guards
  the read so a wrong RVA can never crash). line_append + dispatch keep their unique
  signatures. Rebuilt OK.

Next live test: force-close the frozen game, relaunch offline, re-inject the rebuilt
DLL, type a command → expect `hello from the void` + a verified line_commit log line.

## 2026-06-27 — Session 4: M0 confirmed live; built M3+M4 (real CLI)

- **M0 CONFIRMED LIVE:** "hello from the void" rendered in the real scrollback
  after injecting the rebuilt DLL. Find+call line_append works end to end.
- Reworked for the real-CLI goal (synchronous, persistent session):
  - **bridge.cpp** now runs ONE persistent `powershell.exe -NoProfile -NoLogo`
    (REPL over redirected pipes). `cd`/vars persist. Per-command output is
    delimited by a unique marker line; reads poll with PeekNamedPipe and a 15s
    timeout (bounds the main-thread hitch). Init sets empty prompt + ASCII out.
  - **line_append is now HOOKED** (not just called): while `g_inDispatch` is set
    we swallow ALL of the game's terminal output, so the fake built-in
    `dir`/`cd`/`cat`/`help` don't shadow the real shell. Our own output is
    rendered via the trampoline (`orig_line_append`), bypassing suppression.
  - **hook_dispatch:** capture typed line -> run orig_dispatch suppressed (keeps
    engine input bookkeeping) -> `bridge::exec(line)` -> print each output line.
    Every non-empty command goes to the real shell.
  - dllmain starts the session on attach, then installs hooks (no startup race).
- **Known v1 caveats:** BO1 easter-egg commands (zork/doa/login) are shadowed;
  interactive programs (python REPL, claude) will hang until the 15s timeout
  (synchronous model). Async + native-passthrough prefix are later upgrades.
- **To verify next live test:** does the terminal keep accepting input after a
  command (i.e., is suppressed orig_dispatch enough bookkeeping)? Does `cd` then
  `dir` show the new directory (persistence)? Watch DebugView `[hooks] shell:`.

## 2026-06-27 — Session 5: glitches root-caused; async + pump + toggle

User tested synchronous M3: worked but glitchy — output one command behind,
typewriter letter-by-letter, screen blanked at a limit then resumed on Enter.

Root-caused via Ghidra (FindTermRender.java -> render.txt):
- **FUN_00575be0** is the per-frame terminal renderer. Reveals rows over time
  (DAT_00bf8124 reveal head chasing DAT_00bf8120 write head) = the typewriter.
  **Case 5 = a real `--MORE--` pager** that waits for a keypress = the
  "blank then resume on Enter". Rows live in a **100-row ring** (`%100 * 0x56`).
- The synchronous dump froze the engine thread (renderer couldn't run) and
  flooded the ring -> all the glitches.

Fix (the architecture CLAUDE.md originally prescribed):
- **Async:** shell runs on a worker thread (`bridge::workerLoop`), draining
  g_inbound -> exec -> g_outbound. Engine thread never blocks; a hung/interactive
  command just times out after 15s with no freeze.
- **Per-frame pump:** hooked FUN_00575be0 (RVA 0x175BE0, __cdecl/6 args, fwd
  verbatim). Each frame drips <=3 lines from g_outbound, gated by
  `write-reveal < 24`, so the ring never overruns and output streams in at the
  reveal rate (looks like a live terminal).
- **Prompt leak fix:** REPL prompt set to sentinel `###BO1P###` and stripped from
  output (empty prompt falls back to "PS> " in PowerShell, hence the leak).
- **Toggle mode (user request):** start in the normal BO1 terminal; typing
  `connect` enters host-shell mode; typing `connect` again returns. Only in shell
  mode are commands routed; game behaves 100% normally otherwise.

Next live test: `connect` -> `whoami`/`dir`/`cd ..`/`type file` -> `connect` to
exit. Watch: smooth streaming (no freeze/pager), cd persistence, prompt clean.

### Assumptions to verify live in x32dbg before trusting the build
- [ ] dispatch_submit is truly `__cdecl(ctx)` (Ghidra showed no __fastcall tag).
- [ ] line_append's 4th pushed dword (0) is ignored (decompile typed only 3 args).
- [ ] Calling line_append is safe only while the terminal is active (it writes
      DAT_00bf5f10 / DAT_00bf8120) — guaranteed inside dispatch_submit.
- [ ] frame_tick: locate for M3 (candidate approach: find the terminal's own
      per-frame draw that consumes DAT_00bf8120).
