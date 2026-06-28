#include "bridge.h"
#include "queues.h"

#include <windows.h>
#include <atomic>

namespace {

HANDLE g_inW  = nullptr;   // parent -> child stdin
HANDLE g_outR = nullptr;   // child stdout+stderr -> parent
HANDLE g_proc = nullptr;
HANDLE g_thr  = nullptr;

// Marker that delimits one command's output. Must be something normal output
// won't contain. kMarker is what we MATCH on; kMarkerCmd is the command we send
// to PRODUCE it. cmd echoes every piped command line back (echo off only kills
// the prompt, not the line), and that echoed line would otherwise contain the
// marker verbatim -> readUntilMarker latches onto the echo instead of the real
// output (intermittent one-behind desync). The `^` escapes the next char in
// cmd: the echoed command keeps the caret (so it WON'T match kMarker) while the
// echo OUTPUT drops it (so it WILL). `BO1_BRIDGE_END` is the stem we strip on.
const char* const kMarker    = "###BO1_BRIDGE_END_7F3A###";
const char* const kMarkerCmd = "echo ###BO1_BRIDGE_END^_7F3A###";
const char* const kMarkerStem = "BO1_BRIDGE_END";

// The interactive host writes a prompt before each read. We can't reliably make
// it empty (PowerShell falls back to "PS> " for an empty prompt), so we make the
// prompt a known sentinel and strip it from output instead.
const char* const kPrompt = "###BO1P###";

// Set by the engine thread (requestCancel) when the user hits Ctrl-C / 'q'.
// The worker checks it after exec() returns and discards that command's output
// instead of queueing it. We don't break readUntilMarker mid-read -- that would
// leave unread bytes in the pipe and desync the next command's marker framing;
// instead we let the read finish (or time out) and throw the result away.
std::atomic<bool> g_cancel{false};

void writeLine(const char* s) {
    if (!g_inW) return;
    DWORD wrote = 0;
    WriteFile(g_inW, s, (DWORD)lstrlenA(s), &wrote, nullptr);
    WriteFile(g_inW, "\r\n", 2, &wrote, nullptr);
}

// Read from the shell until kMarker is seen (returns true) or timeout (returns
// false). `out` receives the text before the marker either way.
bool readUntilMarker(unsigned timeoutMs, std::string& out) {
    out.clear();
    DWORD start = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(g_outR, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
            DWORD got = 0;
            if (ReadFile(g_outR, buf, want, &got, nullptr) && got > 0) {
                out.append(buf, got);
                size_t pos = out.find(kMarker);
                if (pos != std::string::npos) { out.resize(pos); return true; }
            }
        } else {
            if (GetTickCount() - start > timeoutMs) return false;
            Sleep(5);
        }
    }
}

// Discard whatever is sitting in the pipe right now. Used to resync after a
// command timed out: stale/partial output (and a marker that arrived late) must
// not bleed into the next command's framing, or every command after it reads
// the previous command's output -- the "one behind" buffering.
void drainPipe() {
    DWORD avail = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        if (!PeekNamedPipe(g_outR, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        char buf[4096];
        DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
        DWORD got = 0;
        if (!ReadFile(g_outR, buf, want, &got, nullptr) || got == 0) break;
    }
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
        // Trim trailing padding spaces so wrapping/line-counting is accurate.
        size_t te = line.find_last_not_of(" \t");
        if (te == std::string::npos) line.clear();
        else line.erase(te + 1);
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

    // cmd.exe as the persistent shell: it executes each piped line immediately
    // and writes output to the pipe UNBUFFERED -- unlike powershell.exe, which
    // block-buffers its stdout to a pipe (causing the one-command-behind lag) and
    // pads every output line with trailing spaces to the console width. `cd` and
    // environment changes persist for the session. For PowerShell semantics, run
    // `powershell -c "..."` as a command.
    char cmdline[] = "cmd.exe";

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

    // Session init: silence command echo, set the prompt to a sentinel we strip
    // from output, then a marker so we can drain all the startup noise.
    writeLine("@echo off");
    writeLine("prompt ###BO1P###");
    writeLine(kMarkerCmd);
    std::string banner;
    readUntilMarker(10000, banner);
    return true;
}

// Set when a command times out: the session framing is suspect, so the next
// exec drains the pipe before sending anything.
bool g_desynced = false;

std::vector<std::string> exec(const std::string& command, unsigned timeoutMs) {
    if (!sessionAlive()) {
        return { "[bridge] shell session is not running" };
    }

    // A previous command timed out -> stale bytes may still be in the pipe.
    // Clear them so they can't be read as THIS command's output.
    if (g_desynced) { drainPipe(); g_desynced = false; }

    // Redirect this command's stdin from NUL. An interactive program (python,
    // more, sort, set /p, ftp, a bare REPL) then gets EOF immediately and exits
    // instead of blocking on the session pipe and swallowing our end-marker --
    // which is what caused the hang / "buffering" / one-command-behind lag. cmd
    // still reads its NEXT line (the marker echo, then future commands) from the
    // pipe as normal, so the session is unaffected. A user's own `< file`
    // redirect on the command still wins for that program.
    std::string sent = command + " <nul";
    writeLine(sent.c_str());
    // Marker on its own line, so it runs even if `command` failed.
    writeLine(kMarkerCmd);

    std::string out;
    if (!readUntilMarker(timeoutMs, out)) {
        g_desynced = true;     // resync before the next command
        out.append("\n[bridge] command timed out (input-waiting command?)");
    }

    // cmd echoed our command line(s) back into the stream. Drop the echo of the
    // command itself (once -- in case real output legitimately repeats it) and
    // the echo of the marker command, leaving only the program's real output.
    std::vector<std::string> lines = splitLines(out);
    std::vector<std::string> result;
    result.reserve(lines.size());
    bool droppedEcho = false;
    for (auto& ln : lines) {
        if (!droppedEcho && ln == sent) { droppedEcho = true; continue; }
        if (ln.find(kMarkerStem) != std::string::npos) continue;  // echoed marker cmd
        result.push_back(std::move(ln));
    }
    return result;
}

void requestCancel() {
    g_cancel.store(true);
}

void workerLoop() {
    for (;;) {
        auto cmd = g_inbound.tryPop();
        if (!cmd) { Sleep(16); continue; }
        g_cancel.store(false);              // arm for this command
        std::vector<std::string> lines = exec(*cmd);
        if (g_cancel.load()) {              // canceled mid-run: drop the output
            g_cancel.store(false);
            continue;
        }
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
