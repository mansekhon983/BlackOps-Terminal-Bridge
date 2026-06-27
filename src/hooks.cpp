#include "hooks.h"
#include "queues.h"
#include "sigscan.h"

#include <windows.h>
#include <cstdio>

#include "MinHook.h"

// =============================================================================
// SIGNATURES  --  fill these in from notes/offsets.md as they are confirmed in
// Ghidra / x32dbg. Until a signature is known, leave it nullptr and that hook
// is skipped. BlackOps.exe is 32-bit (x86), so conventions are likely
// __thiscall / __cdecl / __fastcall -- match exactly what the game uses.
// =============================================================================
namespace sig {
    // TODO(RE): the routine that prints one line into the terminal scrollback.
    constexpr const char* line_append   = nullptr;
    // TODO(RE): the handler that fires on Enter with the assembled input string.
    constexpr const char* submit_handler = nullptr;
    // TODO(RE): the per-frame update/render function (runs every frame).
    constexpr const char* frame_tick    = nullptr;
}

namespace {

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

// ---- line_append ------------------------------------------------------------
// PLACEHOLDER prototype/convention -- correct these once confirmed in x32dbg.
// Common BO1/idTech form is something like a console/terminal print taking a
// channel/window context (the likely `this`) and a string. Mirror it exactly.
using LineAppend_t = void(__thiscall*)(void* termCtx, const char* text);
LineAppend_t orig_line_append = nullptr;
void* g_termCtx = nullptr;   // captured from the submit hook at runtime.

// ---- submit_handler ---------------------------------------------------------
// PLACEHOLDER. The real one assembles the typed command; we read it out and
// push to g_inbound, then call the original so the game still behaves.
using SubmitHandler_t = int(__thiscall*)(void* termCtx, const char* input);
SubmitHandler_t orig_submit = nullptr;

int __fastcall hook_submit(void* termCtx, void* /*edx*/, const char* input) {
    g_termCtx = termCtx;                 // stash for line_append calls
    if (input && *input) g_inbound.push(input);
    return orig_submit ? orig_submit(termCtx, input) : 0;
}

// ---- frame_tick -------------------------------------------------------------
using FrameTick_t = void(__cdecl*)();
FrameTick_t orig_frame = nullptr;

void __cdecl hook_frame() {
    // Drain shell output ON THE MAIN THREAD and render via line_append.
    if (orig_line_append && g_termCtx) {
        while (auto line = g_outbound.tryPop()) {
            orig_line_append(g_termCtx, line->c_str());
        }
    }
    if (orig_frame) orig_frame();
}

template <typename T>
bool createHook(const char* name, const char* pattern, void* detour, T& orig) {
    if (!pattern) { logf("[hooks] %s: no signature yet, skipped\n", name); return false; }
    uintptr_t addr = sigscan::findInModule("BlackOps.exe", pattern);
    if (!addr) { logf("[hooks] %s: signature NOT FOUND\n", name); return false; }
    if (MH_CreateHook(reinterpret_cast<void*>(addr), detour,
                      reinterpret_cast<void**>(&orig)) != MH_OK) {
        logf("[hooks] %s: MH_CreateHook failed @ %p\n", name, (void*)addr);
        return false;
    }
    MH_EnableHook(reinterpret_cast<void*>(addr));
    logf("[hooks] %s: hooked @ %p\n", name, (void*)addr);
    return true;
}

}  // namespace

namespace hooks {

bool install() {
    if (MH_Initialize() != MH_OK) {
        logf("[hooks] MH_Initialize failed\n");
        return false;
    }

    // Resolve line_append first; it's required for M0 and used by frame_tick.
    if (sig::line_append) {
        uintptr_t addr = sigscan::findInModule("BlackOps.exe", sig::line_append);
        orig_line_append = reinterpret_cast<LineAppend_t>(addr);
        logf("[hooks] line_append @ %p\n", (void*)addr);
    } else {
        logf("[hooks] line_append: no signature yet (M0 blocked on RE)\n");
    }

    bool any = false;
    any |= createHook("submit_handler", sig::submit_handler, (void*)&hook_submit, orig_submit);
    any |= createHook("frame_tick",     sig::frame_tick,     (void*)&hook_frame,  orig_frame);
    return any && orig_line_append != nullptr;
}

void remove() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

void debugEmitLine(const char* text) {
    // Routed through the outbound queue so it renders on the main thread.
    g_outbound.push(text ? text : "");
}

}  // namespace hooks
