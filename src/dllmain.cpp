// Injection entry point. On attach: start the persistent shell session, then
// install the engine hooks. The shell is started first (and fully warmed up)
// so the dispatch hook can use it immediately without a startup race.
//
// BlackOps.exe is 32-bit -> this DLL must be built x86 (see CMakeLists.txt).

#include <windows.h>
#include <thread>

#include "hooks.h"
#include "bridge.h"

static DWORD WINAPI OnAttach(LPVOID) {
    // Start (and drain startup of) the persistent powershell session off the
    // loader lock, then launch the worker thread, then install hooks. Order
    // matters: the shell is ready and the worker is running before the dispatch
    // hook can queue anything.
    bridge::startSession();
    std::thread(bridge::workerLoop).detach();
    hooks::install();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, OnAttach, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        hooks::remove();
        bridge::stopSession();
        break;
    }
    return TRUE;
}
