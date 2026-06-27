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

- [ ] Export Defined Strings from the Ghidra project (headless).
- [ ] Pick 2–3 unique terminal strings, follow XREFs.
- [ ] Breakpoint in x32dbg; type known command; inspect regs/stack.
- [ ] Walk stack UP → submit_handler; DOWN → line_append.
