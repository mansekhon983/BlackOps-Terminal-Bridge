#pragma once
// ConPTY backend for the in-game terminal emulator (emulator phase, milestone E0).
//
// A pseudoconsole makes the child process believe it's talking to a real
// terminal: it emits VT/ANSI output (cursor moves, colours, alt-screen) and
// accepts VT input. That byte stream is exactly what a terminal emulator parses.
// This replaces, for "raw/pty mode", the marker-delimited cmd-pipe bridge used
// by the line-shell mode (see bridge.h). Available on Windows 10 1809+.
//
// Threading: read() blocks, so call it from a dedicated reader thread; write()
// and resize() may be called from any thread.

#include <windows.h>
#include <cstddef>

namespace conpty {

// True if the OS exports the ConPTY API (Win10 1809+).
bool available();

class Session {
public:
    // Spawn `command` (default cmd.exe) attached to a `cols`x`rows` pseudoconsole.
    // `cwd` is the child's starting directory (nullptr/empty = inherit ours).
    bool start(short cols, short rows, const char* command, const char* cwd = nullptr);

    // True while the child process is running.
    bool alive();

    // Send input bytes (already VT-encoded) to the child.
    void write(const char* data, size_t len);

    // Block until output is available; copy up to `cap` bytes into `buf`.
    // Returns the byte count, or 0 on EOF / closed pipe. Call on a reader thread.
    DWORD read(char* buf, DWORD cap);

    // Tell the child the window resized (re-lays-out full-screen apps).
    void resize(short cols, short rows);

    // Terminate the child and release all handles.
    void stop();

private:
    void* hpc_   = nullptr;   // HPCON (opaque pseudoconsole handle)
    HANDLE inW_  = nullptr;    // parent writes child input here
    HANDLE outR_ = nullptr;    // parent reads child output here
    PROCESS_INFORMATION pi_{};
};

}  // namespace conpty
