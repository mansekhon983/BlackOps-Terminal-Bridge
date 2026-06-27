#include "hooks.h"
#include "queues.h"
#include "sigscan.h"

#include <windows.h>
#include <cstdio>

#include "MinHook.h"

// =============================================================================
// SIGNATURES  --  confirmed from Ghidra, see notes/offsets.md. BlackOps.exe is
// 32-bit (x86). Absolute-address operands are wildcarded (?? ?? ?? ??) because
// the module is ASLR-relocated at load; the scan runs over the relocated image.
// =============================================================================
namespace sig {
    // line_append = FUN_004ef440 (RVA 0xEF440). __cdecl(text, color, flag, zero).
    // We CALL this; we do not hook it. No abs operands in prologue -> exact bytes.
    constexpr const char* line_append =
        "83 EC 08 8B 44 24 0C 55 56 33 F6 89 74 24 08 33 ED 8D 50 01 8A 08 40 84";

    // line_commit = FUN_00433720 (RVA 0x33720). __cdecl(void). Called after each
    // line_append. Its whole body is the generic (x%100)*0x56 idiom on the global
    // line counter, which recurs ~10x in the image -> NO unique signature. Since
    // ASLR is OFF (BlackOps.exe lacks DYNAMIC_BASE; always loads at 0x400000) we
    // resolve it by RVA and verify these prologue bytes before trusting it.
    constexpr uintptr_t  line_commit_rva = 0x00033720;
    constexpr const char* line_commit_verify =
        "8B 0D 20 81 BF 00 B8 1F 85 EB 51 F7 E9 C1 FA 05 8B C2 C1 E8 1F 03 D0 6B";

    // dispatch_submit = FUN_0055f800 (RVA 0x15F800). __cdecl(ctx). Terminal
    // command dispatcher; runs on the main thread. Our submit hook + M0 anchor.
    constexpr const char* submit_handler =
        "83 EC 08 A1 ?? ?? ?? ?? 8D 04 80 8B 04 85 ?? ?? ?? ?? C7 04 24 ?? ?? ?? ??";

    // TODO(RE): per-frame pump for M3 (async output). Not needed for M0/M1.
    constexpr const char* frame_tick = nullptr;

    // Global: raw typed command line read by the dispatcher (DAT_00bf8130).
    // Static VA at default base; rebased at runtime -> resolve relative to module
    // base, not hardcoded. (M1 will read it; left here for reference.)
    constexpr uintptr_t input_buffer_rva = 0x007F8130;  // 0xBF8130 - 0x400000
}

namespace {

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

// ---- line_append / line_commit (RESOLVED, not hooked -- we CALL them) --------
// line_append: void __cdecl(const char* text, int color, int flag, int zero).
// Mirrors the game's own call: line_append(text, 1, 1, 0); line_commit();
using LineAppend_t = void(__cdecl*)(const char* text, int color, int flag, int zero);
using LineCommit_t = void(__cdecl*)();
LineAppend_t fn_line_append = nullptr;
LineCommit_t fn_line_commit = nullptr;

// Print one line into the terminal scrollback. MAIN THREAD ONLY (call from the
// submit/frame hooks, never from the shell worker -- off-thread engine calls
// crash, see CLAUDE.md threading model).
void termPrintLine(const char* s) {
    if (!fn_line_append) return;
    fn_line_append(s ? s : "", 1, 1, 0);
    if (fn_line_commit) fn_line_commit();
}

// ---- dispatch_submit hook ---------------------------------------------------
// FUN_0055f800: void __cdecl(ctx). Fires when a command is submitted in the
// terminal; runs on the main thread. The raw typed line lives at the global
// input buffer (input_buffer_rva from the module base).
using Dispatch_t = void(__cdecl*)(void* ctx);
Dispatch_t   orig_dispatch = nullptr;
uintptr_t    g_moduleBase  = 0;

const char* readInputBuffer() {
    if (!g_moduleBase) return nullptr;
    return reinterpret_cast<const char*>(g_moduleBase + sig::input_buffer_rva);
}

void __cdecl hook_dispatch(void* ctx) {
    // ---- M0: prove output injection -------------------------------------
    termPrintLine("hello from the void");

    // ---- M1: capture the submitted command (logged for now) -------------
    if (const char* input = readInputBuffer()) {
        if (*input) {
            g_inbound.push(input);
            logf("[hooks] submit: \"%s\"\n", input);
        }
    }

    // ---- M3 (future): drain shell output here, on the main thread -------
    // while (auto line = g_outbound.tryPop()) termPrintLine(line->c_str());

    if (orig_dispatch) orig_dispatch(ctx);
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

uintptr_t resolve(const char* name, const char* pattern) {
    uintptr_t addr = pattern ? sigscan::findInModule("BlackOps.exe", pattern) : 0;
    logf("[hooks] resolve %s: %p\n", name, (void*)addr);
    return addr;
}

}  // namespace

namespace hooks {

bool install() {
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("BlackOps.exe"));

    if (MH_Initialize() != MH_OK) {
        logf("[hooks] MH_Initialize failed\n");
        return false;
    }

    // Resolve the call targets we invoke (line_append is the M0 requirement).
    fn_line_append = reinterpret_cast<LineAppend_t>(resolve("line_append", sig::line_append));

    // line_commit has no unique signature -> RVA + prologue verification.
    uintptr_t commit = g_moduleBase + sig::line_commit_rva;
    if (sigscan::matches(commit, sig::line_commit_verify)) {
        fn_line_commit = reinterpret_cast<LineCommit_t>(commit);
        logf("[hooks] line_commit verified @ %p\n", (void*)commit);
    } else {
        fn_line_commit = nullptr;
        logf("[hooks] line_commit prologue MISMATCH @ %p -- commit skipped\n", (void*)commit);
    }

    bool any = false;
    any |= createHook("dispatch_submit", sig::submit_handler, (void*)&hook_dispatch, orig_dispatch);
    // frame_tick hook deferred to M3 (sig::frame_tick still TBD).

    return any && fn_line_append != nullptr;
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
