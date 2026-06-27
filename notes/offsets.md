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

## Functions to find

| Symbol | Address (build-specific) | Signature / pattern | Convention | Notes |
|---|---|---|---|---|
| `submit_handler` | TBD | TBD | TBD (likely thiscall/fastcall) | where the typed command string is assembled on Enter |
| `line_append` | TBD | TBD | TBD (likely thiscall) | prints one line into terminal scrollback; reuse to render shell output |
| `frame_tick` | TBD | TBD | TBD (likely cdecl) | per-frame update/render; confirm it runs every frame |

## Image base

- Default PE ImageBase: TBD (read from optional header; module is ASLR-relocated
  at runtime — sigscan handles this, addresses below are for the static Ghidra view).

## Terminal strings used as XREF entry points

See `re-log.md`. List the exact strings found in Ghidra's Defined Strings and the
address each XREF lands at.
