// Standalone exerciser for the ConPTY backend (emulator milestone E0).
//
// Built as a GUI-subsystem app (no console) so it mirrors the game environment:
// a process with NO console can't let the child attach to a parent console and
// leak output there -- the child MUST use the pseudoconsole, so conpty_pipe.bin
// is a faithful capture of what the pipe will deliver inside BlackOps.exe.
//
// All diagnostics go to files next to the working dir:
//   conpty_pipe.bin  -- raw bytes the pipe delivered (VT-laden)
//   conpty_log.txt   -- summary: each read size, totals, pass/fail
//
// Usage (argv): conpty_test "<command>" ["<input-to-type>"]
//   default command: cmd.exe ; if input given, it's written to the child + CRLF.

#include "../src/conpty.h"

#include <cstdio>
#include <string>
#include <thread>
#include <atomic>

static FILE* g_log = nullptr;
static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    g_log = fopen("conpty_log.txt", "w");

    if (!conpty::available()) { logf("ConPTY not available\n"); return 1; }

    std::string cmd = (__argc > 1) ? __argv[1] : "cmd.exe";
    logf("starting 80x26 pseudoconsole: %s\n", cmd.c_str());

    conpty::Session s;
    if (!s.start(80, 26, cmd.c_str())) {
        logf("start FAILED (GetLastError=%lu)\n", GetLastError());
        return 1;
    }

    FILE* cap = fopen("conpty_pipe.bin", "wb");
    std::atomic<size_t> total{0};
    std::thread reader([&] {
        char buf[4096];
        for (;;) {
            DWORD n = s.read(buf, sizeof(buf));
            if (n == 0) break;
            if (cap) { fwrite(buf, 1, n, cap); fflush(cap); }
            logf("[read %lu]\n", n);
            total += n;
        }
    });

    // Optionally type a command into the child after it settles (exercises
    // write() + the interactive round-trip, the E3 path).
    if (__argc > 2) {
        Sleep(500);
        std::string in = std::string(__argv[2]) + "\r\n";
        s.write(in.data(), in.size());
        logf("wrote input: %s", in.c_str());
    }

    Sleep(1500);            // drain window
    s.stop();               // ClosePseudoConsole -> unblocks the reader
    reader.join();
    if (cap) fclose(cap);

    logf("DONE: pipe delivered %zu bytes total\n", total.load());
    if (g_log) fclose(g_log);
    return 0;
}
