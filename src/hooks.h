#pragma once
// MinHook setup for the three engine hook points. Addresses are resolved at
// runtime via sigscan (NOT hardcoded) once the signatures are confirmed in
// notes/offsets.md.
//
//   1. submit_handler  - fires on Enter; captures the assembled command string.
//   2. line_append     - the engine's "print one line into terminal scrollback"
//                        routine. Reused to render shell output. Call ONLY from
//                        the per-frame hook (main thread).
//   3. frame_tick      - per-frame update; drains g_outbound and renders.

namespace hooks {

// Installs all hooks that have a confirmed signature. Returns true if at least
// the line-append hook (needed for M0) was installed. Safe to call once on
// DLL attach. Logs progress to the debug log.
bool install();

// Removes hooks and uninitializes MinHook. Called on DLL detach.
void remove();

}  // namespace hooks
