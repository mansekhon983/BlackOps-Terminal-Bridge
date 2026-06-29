#include "conpty.h"

#include <string>
#include <vector>

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE may be absent from older SDK headers;
// its value is fixed (ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)).
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace {

// The ConPTY entry points are resolved dynamically from kernel32 so the build
// doesn't depend on SDK header/WINVER settings and degrades gracefully on an OS
// without ConPTY.
typedef HRESULT (WINAPI *CreatePseudoConsole_t)(COORD, HANDLE, HANDLE, DWORD, void**);
typedef HRESULT (WINAPI *ResizePseudoConsole_t)(void*, COORD);
typedef void    (WINAPI *ClosePseudoConsole_t)(void*);

CreatePseudoConsole_t pCreate = nullptr;
ResizePseudoConsole_t pResize = nullptr;
ClosePseudoConsole_t  pClose  = nullptr;

bool resolve() {
    static bool tried = false;
    static bool ok = false;
    if (tried) return ok;
    tried = true;
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!k) return false;
    pCreate = (CreatePseudoConsole_t)GetProcAddress(k, "CreatePseudoConsole");
    pResize = (ResizePseudoConsole_t)GetProcAddress(k, "ResizePseudoConsole");
    pClose  = (ClosePseudoConsole_t) GetProcAddress(k, "ClosePseudoConsole");
    ok = pCreate && pResize && pClose;
    return ok;
}

}  // namespace

namespace conpty {

bool available() { return resolve(); }

bool Session::start(short cols, short rows, const char* command, const char* cwd) {
    if (!resolve()) return false;

    // Two pipes: one for our input -> child, one for child output -> us.
    HANDLE inR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW_, nullptr, 0)) return false;
    if (!CreatePipe(&outR_, &outW, nullptr, 0)) {
        CloseHandle(inR); CloseHandle(inW_); inW_ = nullptr;
        return false;
    }

    COORD size{ cols, rows };
    HRESULT hr = pCreate(size, inR, outW, 0, &hpc_);
    // The pseudoconsole owns inR (its stdin source) and outW (its stdout sink);
    // we keep only inW_ (write) and outR_ (read).
    CloseHandle(inR);
    CloseHandle(outW);
    if (FAILED(hr)) { stop(); return false; }

    // STARTUPINFOEX carrying the pseudoconsole as a process-thread attribute.
    STARTUPINFOEXA si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<char> attrBuf(bytes);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) { stop(); return false; }
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hpc_, sizeof(hpc_), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(list);
        stop();
        return false;
    }
    si.lpAttributeList = list;

    std::string cmd = (command && *command) ? command : "cmd.exe";
    std::vector<char> mut(cmd.begin(), cmd.end());
    mut.push_back('\0');

    const char* dir = (cwd && *cwd) ? cwd : nullptr;
    BOOL ok = CreateProcessA(nullptr, mut.data(), nullptr, nullptr, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT, nullptr, dir,
                             &si.StartupInfo, &pi_);
    DeleteProcThreadAttributeList(list);
    if (!ok) { stop(); return false; }
    return true;
}

bool Session::alive() {
    return pi_.hProcess && WaitForSingleObject(pi_.hProcess, 0) == WAIT_TIMEOUT;
}

void Session::write(const char* data, size_t len) {
    if (!inW_ || len == 0) return;
    DWORD wrote = 0;
    WriteFile(inW_, data, (DWORD)len, &wrote, nullptr);
}

DWORD Session::read(char* buf, DWORD cap) {
    if (!outR_) return 0;
    DWORD got = 0;
    if (!ReadFile(outR_, buf, cap, &got, nullptr)) return 0;
    return got;
}

void Session::resize(short cols, short rows) {
    if (hpc_ && pResize) {
        COORD size{ cols, rows };
        pResize(hpc_, size);
    }
}

void Session::stop() {
    // Close the pseudoconsole first so the child sees its console go away.
    if (hpc_ && pClose) { pClose(hpc_); hpc_ = nullptr; }
    if (pi_.hProcess) {
        TerminateProcess(pi_.hProcess, 0);
        CloseHandle(pi_.hProcess);
        pi_.hProcess = nullptr;
    }
    if (pi_.hThread) { CloseHandle(pi_.hThread); pi_.hThread = nullptr; }
    if (inW_)  { CloseHandle(inW_);  inW_  = nullptr; }
    if (outR_) { CloseHandle(outR_); outR_ = nullptr; }
}

}  // namespace conpty
