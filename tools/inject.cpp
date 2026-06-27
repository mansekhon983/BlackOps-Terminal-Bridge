// Minimal DLL injector for the BO1 bridge.  CreateRemoteThread + LoadLibraryA.
// Must be built 32-bit to match BlackOps.exe (kernel32/LoadLibraryA address in a
// 32-bit target must come from a 32-bit injector's own kernel32).
//
// Usage:
//   inject.exe                       # defaults: BlackOps.exe + bo1bridge.dll next to this exe
//   inject.exe <process.exe> <full\path\to\some.dll>
//
// The target process must already be running. Run BO1 OFFLINE only (VAC).

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

static DWORD findPid(const char* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, exeName) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Absolute path to a file sitting next to this executable.
static std::string siblingPath(const char* leaf) {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + leaf;
}

int main(int argc, char** argv) {
    const char* proc = (argc > 1) ? argv[1] : "BlackOps.exe";
    std::string dll  = (argc > 2) ? argv[2] : siblingPath("bo1bridge.dll");

    // Verify the DLL exists and we have its full path.
    char full[MAX_PATH]{};
    if (!GetFullPathNameA(dll.c_str(), MAX_PATH, full, nullptr) ||
        GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        printf("[inject] DLL not found: %s\n", dll.c_str());
        return 1;
    }

    DWORD pid = findPid(proc);
    if (!pid) {
        printf("[inject] process not running: %s  (launch the game first)\n", proc);
        return 1;
    }
    printf("[inject] %s pid=%lu\n", proc, pid);
    printf("[inject] dll=%s\n", full);

    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                           PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                           FALSE, pid);
    if (!h) { printf("[inject] OpenProcess failed: %lu\n", GetLastError()); return 1; }

    const size_t len = strlen(full) + 1;
    void* remote = VirtualAllocEx(h, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("[inject] VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(h); return 1; }

    if (!WriteProcessMemory(h, remote, full, len, nullptr)) {
        printf("[inject] WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE); CloseHandle(h); return 1;
    }

    // LoadLibraryA lives in kernel32 at the same address across processes of the
    // same bitness, so our own address is valid in the target.
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryA"));

    HANDLE thr = CreateRemoteThread(h, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!thr) {
        printf("[inject] CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(h, remote, 0, MEM_RELEASE); CloseHandle(h); return 1;
    }

    WaitForSingleObject(thr, INFINITE);
    DWORD exitCode = 0; GetExitCodeThread(thr, &exitCode);  // = HMODULE of loaded dll (low 32 bits)

    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(thr);
    CloseHandle(h);

    if (exitCode == 0)
        printf("[inject] LoadLibrary returned 0 — injection FAILED in target.\n");
    else
        printf("[inject] injected OK (module base 0x%08lX in target).\n", exitCode);
    return exitCode ? 0 : 1;
}
