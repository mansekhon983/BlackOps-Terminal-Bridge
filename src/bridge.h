#pragma once
// Shell bridge: run a captured command line in PowerShell and return its
// stdout+stderr split into lines. One-shot per command (M2/M3); a persistent
// session is a later upgrade (M4).

#include <string>
#include <vector>

namespace bridge {

// Spawns: powershell.exe -NoProfile -Command "<command>"
// with CREATE_NO_WINDOW, stdout+stderr redirected to a pipe, read to EOF.
// Returns output split on newlines (CR/LF stripped). Never throws.
std::vector<std::string> runCommand(const std::string& command);

// Worker-thread loop: drains g_inbound, runs each command, pushes result
// lines onto g_outbound. Started once from dllmain on attach.
void workerLoop();

}  // namespace bridge
