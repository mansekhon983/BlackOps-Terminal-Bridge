// Injection entry point. On attach: start the shell worker thread and install
// engine hooks. The heavy lifting happens in hooks.cpp / bridge.cpp.
//
// BlackOps.exe is 32-bit -> this DLL must be built x86 (see CMakeLists.txt).

#include <windows.h>
#include <thread>

#include "hooks.h"
#include "bridge.h"

static DWORD WINAPI OnAttach(LPVOID) {
    // Shell worker runs for the life of the process; engine functions are never
    // called from here -- only the per-frame hook touches the engine.
    std::thread(bridge::workerLoop).detach();

    hooks::install();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // Do real work off the loader lock.
        CreateThread(nullptr, 0, OnAttach, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        hooks::remove();
        break;
    }
    return TRUE;
}
