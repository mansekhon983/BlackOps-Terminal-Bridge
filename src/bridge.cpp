#include "bridge.h"
#include "queues.h"

#include <windows.h>

namespace {

HANDLE g_inW  = nullptr;   // parent -> child stdin
HANDLE g_outR = nullptr;   // child stdout+stderr -> parent
HANDLE g_proc = nullptr;
HANDLE g_thr  = nullptr;

// Marker that delimits one command's output. Must be something normal output
// won't contain. We print it as a bare string from PowerShell.
const char* const kMarker = "###BO1_BRIDGE_END_7F3A###";

// The interactive host writes a prompt before each read. We can't reliably make
// it empty (PowerShell falls back to "PS> " for an empty prompt), so we make the
// prompt a known sentinel and strip it from output instead.
const char* const kPrompt = "###BO1P###";

void writeLine(const char* s) {
    if (!g_inW) return;
    DWORD wrote = 0;
    WriteFile(g_inW, s, (DWORD)lstrlenA(s), &wrote, nullptr);
    WriteFile(g_inW, "\r\n", 2, &wrote, nullptr);
}

// Read from the shell until kMarker is seen (or timeout). Returns the raw text
// before the marker.
std::string readUntilMarker(unsigned timeoutMs) {
    std::string acc;
    DWORD start = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(g_outR, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
            DWORD got = 0;
            if (ReadFile(g_outR, buf, want, &got, nullptr) && got > 0) {
                acc.append(buf, got);
                size_t pos = acc.find(kMarker);
                if (pos != std::string::npos) { acc.resize(pos); break; }
            }
        } else {
            if (GetTickCount() - start > timeoutMs) {
                acc.append("\n[bridge] command timed out");
                break;
            }
            Sleep(5);
        }
    }
    return acc;
}

std::vector<std::string> splitLines(const std::string& text) {
    const size_t plen = lstrlenA(kPrompt);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Strip any leading prompt sentinels (the host emits one per read; it can
        // be prefixed onto a real output line since it has no trailing newline).
        while (line.compare(0, plen, kPrompt) == 0) line.erase(0, plen);
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    // Trim leading/trailing blank lines (prompt artifacts).
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

}  // namespace

namespace bridge {

bool sessionAlive() {
    return g_proc && WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT;
}

bool startSession() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
    if (!CreatePipe(&outR, &outW, &sa, 0)) {
        CloseHandle(inR); CloseHandle(inW);
        return false;
    }
    // Parent-side handles must not be inherited by the child.
    SetHandleInformation(inW,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = inR;
    si.hStdOutput = outW;
    si.hStdError  = outW;

    // Interactive host reading commands line-by-line from the redirected stdin
    // (NOT `-Command -`, which waits for EOF and runs everything at once).
    char cmdline[] = "powershell.exe -NoProfile -NoLogo";

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline, nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    // Child owns its ends now; parent drops them.
    CloseHandle(inR);
    CloseHandle(outW);

    if (!ok) {
        CloseHandle(inW); CloseHandle(outR);
        return false;
    }

    g_inW = inW; g_outR = outR; g_proc = pi.hProcess; g_thr = pi.hThread;

    // Session init: sentinel prompt (stripped from output), plain ASCII output,
    // quiet progress, then a marker so we can drain all startup output.
    writeLine("function prompt { '###BO1P###' }");
    writeLine("$ProgressPreference='SilentlyContinue'");
    writeLine("$ErrorActionPreference='Continue'");
    writeLine("[Console]::OutputEncoding=[System.Text.Encoding]::ASCII");
    writeLine("'" "###BO1_BRIDGE_END_7F3A###" "'");
    readUntilMarker(10000);   // swallow banner + init echoes
    return true;
}

std::vector<std::string> exec(const std::string& command, unsigned timeoutMs) {
    if (!sessionAlive()) {
        return { "[bridge] shell session is not running" };
    }
    writeLine(command.c_str());
    // Bare-string marker prints itself; runs even if `command` errored, because
    // it's a separate REPL line.
    writeLine("'" "###BO1_BRIDGE_END_7F3A###" "'");
    return splitLines(readUntilMarker(timeoutMs));
}

void workerLoop() {
    for (;;) {
        auto cmd = g_inbound.tryPop();
        if (!cmd) { Sleep(16); continue; }
        std::vector<std::string> lines = exec(*cmd);
        for (auto& line : lines) g_outbound.push(std::move(line));
        g_outbound.push("");   // blank separator after each command's output
    }
}

void stopSession() {
    if (g_proc) {
        TerminateProcess(g_proc, 0);
        CloseHandle(g_proc); g_proc = nullptr;
    }
    if (g_thr)  { CloseHandle(g_thr);  g_thr  = nullptr; }
    if (g_inW)  { CloseHandle(g_inW);  g_inW  = nullptr; }
    if (g_outR) { CloseHandle(g_outR); g_outR = nullptr; }
}

}  // namespace bridge
