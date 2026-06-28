#pragma once
// Persistent shell bridge: one long-lived powershell.exe whose stdin we feed
// commands and whose stdout+stderr we read back. A persistent session means the
// working directory and variables survive between commands (real-CLI feel:
// `cd` sticks) and there's no per-command process-startup lag.
//
// Each command's output is delimited by writing a unique marker line after it
// and reading until that marker appears.
//
// exec() is called on the MAIN (engine) thread from the submit hook and blocks
// until the command finishes or times out -- this is the agreed "synchronous"
// model. A timeout bounds how long the game can hitch.

#include <string>
#include <vector>

namespace bridge {

// Spawn the persistent powershell.exe and drain its startup banner. Returns
// false if the process could not be created. Call once on DLL attach (off the
// main thread is fine; it blocks ~1s draining startup).
bool startSession();

// True while the shell process is alive.
bool sessionAlive();

// Send `command` to the shell, read until the end marker, and return the output
// split into lines (CR/LF stripped). Blocks up to timeoutMs. Never throws.
// Runs on the WORKER thread (never the engine thread).
std::vector<std::string> exec(const std::string& command, unsigned timeoutMs = 15000);

// Worker-thread loop: drains queued commands (g_inbound), runs each via exec,
// pushes the resulting lines onto g_outbound for the per-frame pump to render.
void workerLoop();

// Cancel the command currently being processed: its output is discarded instead
// of rendered. Called from the engine thread when the user hits Ctrl-C / 'q'.
// (Does not kill a still-running child process; it stops the flood of output.)
void requestCancel();

// Terminate the shell and close handles. Called on DLL detach.
void stopSession();

}  // namespace bridge
