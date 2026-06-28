#include "hooks.h"
#include "bridge.h"
#include "sigscan.h"
#include "queues.h"
#include "conpty.h"
#include "grid.h"
#include "vtparse.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>
#include <thread>
#include <atomic>

#include "MinHook.h"

// The in-game command that toggles between the normal BO1 terminal and the
// real host shell. Type it to connect; type it again to return to the game.
static const char* const kToggleWord = "connect";

// =============================================================================
// SIGNATURES  --  confirmed from Ghidra, see notes/offsets.md. BlackOps.exe is
// 32-bit (x86) with ASLR OFF (always loads at 0x400000), so absolute operands
// are stable; some are still wildcarded where it keeps the pattern readable.
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

    // frame_render = FUN_00575be0 (RVA 0x175be0). The per-frame terminal renderer:
    // reveals queued rows over time (typewriter) and runs the --MORE-- pager. We
    // hook it as our main-thread pump to drip shell output at the reveal rate.
    // __cdecl with 6 dword-sized args (forwarded verbatim). RVA + verify.
    constexpr uintptr_t  frame_render_rva = 0x00175BE0;
    constexpr const char* frame_render_verify =
        "A1 24 81 BF 00 83 EC 28 53 55 56 57 3B 05 20 81 BF 00";

    // Globals (RVA = VA - 0x400000). ASLR is off so these are read via module base.
    constexpr uintptr_t input_buffer_rva  = 0x007F8130;  // DAT_00bf8130: typed line
    constexpr uintptr_t lines_written_rva = 0x007F8120;  // DAT_00bf8120: write head
    constexpr uintptr_t lines_shown_rva   = 0x007F8124;  // DAT_00bf8124: reveal head

    // Display grid (the emulator target — see terminal-emulator-plan.md). The
    // renderer's draw loop paints these every frame: grid_chars holds the glyphs
    // (row-major, 80 cols = 0x50 stride, ~27 rows up to offset 0x86f); grid_attrs
    // holds per-cell attributes byte = (color<<4)|flag; the high nibble selects
    // the cell's text colour. A row is drawn until a NUL char or column 80.
    constexpr uintptr_t grid_chars_rva = 0x007F56A0;  // DAT_00bf56a0
    constexpr uintptr_t grid_attrs_rva = 0x007F44D8;  // DAT_00bf44d8
    constexpr int       grid_cols = 80;
    constexpr int       grid_rows = 27;   // rows 0..26; row 26 is the input line
}

namespace {

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

// ---- line_append: HOOKED + CALLED -------------------------------------------
// line_append: void __cdecl(const char* text, int color, int flag, int zero).
//  - HOOKED so that, while the game is dispatching a command, we can swallow its
//    "Unknown Command" error (those commands are ours to run in the shell).
//  - CALLED (via the trampoline `orig_line_append`) to render OUR own output,
//    which bypasses the suppression hook.
using LineAppend_t = void(__cdecl*)(const char* text, int color, int flag, int zero);
using LineCommit_t = void(__cdecl*)();
LineAppend_t orig_line_append = nullptr;   // trampoline = the real renderer
LineCommit_t fn_line_commit   = nullptr;
uintptr_t    g_moduleBase     = 0;

// State (main thread only). g_shellMode: are we in the real-shell terminal?
// g_inDispatch: set around orig_dispatch so the game's own output is swallowed.
// g_paused/g_pageLines: the --MORE-- pager (one screenful at a time).
bool g_shellMode  = false;
bool g_inDispatch = false;
bool g_paused     = false;
int  g_pageLines  = 0;
const int kPageSize = 20;   // visible rows before a --MORE-- break

void __cdecl hook_line_append(const char* text, int color, int flag, int zero) {
    if (g_inDispatch) return;   // swallow the game's built-in output
    if (orig_line_append) orig_line_append(text, color, flag, zero);
}

// Render one line into the scrollback via the REAL renderer (not our hook).
void termPrintLine(const char* s) {
    if (!orig_line_append) return;
    orig_line_append(s ? s : "", 1, 1, 0);
    if (fn_line_commit) fn_line_commit();
}

int32_t readGlobalInt(uintptr_t rva) {
    if (!g_moduleBase) return 0;
    return *reinterpret_cast<volatile int32_t*>(g_moduleBase + rva);
}

void writeGlobalInt(uintptr_t rva, int32_t v) {
    if (!g_moduleBase) return;
    *reinterpret_cast<volatile int32_t*>(g_moduleBase + rva) = v;
}

// ===========================================================================
// EMULATOR — milestone E2: ConPTY child -> VT parser -> grid -> blit.
// We drive a real terminal grid (grid::Screen) from a pseudoconsole child via
// the VT parser, then blit that grid into the engine's char/attr grids and let
// the game's own renderer paint it, bypassing line_append's append-only model.
// ===========================================================================
bool g_emuMode = false;

char* gridChars() { return reinterpret_cast<char*>(g_moduleBase + sig::grid_chars_rva); }
char* gridAttrs() { return reinterpret_cast<char*>(g_moduleBase + sig::grid_attrs_rva); }

// Fill the whole engine grid with spaces in the default attribute.
void emuClear() {
    char* ch = gridChars();
    char* at = gridAttrs();
    const int n = sig::grid_cols * sig::grid_rows;
    for (int i = 0; i < n; ++i) { ch[i] = ' '; at[i] = (1 << 4) | 1; }
}

// --- E2 pipeline: ConPTY child -> reader thread -> VT parser -> grid -> blit --
// Visible terminal height confirmed in-game = 27 rows (the full engine grid, 0..26).
const int kEmuRows = 27;

conpty::Session  g_pty;
grid::Screen     g_screen;
vt::Parser*      g_parser = nullptr;
std::thread      g_ptyReader;
std::atomic<bool> g_ptyRunning{false};

// ANSI colour index (0..15) -> engine palette high-nibble. Provisional, from the
// E1 readout (2≈black 3=red 4=green 5=yellow 6=blue …); calibrated in-game.
uint8_t ansiToEngine(uint8_t ansi) {
    static const uint8_t map[8] = {
        2,  // 0 black
        3,  // 1 red
        4,  // 2 green
        5,  // 3 yellow
        6,  // 4 blue
        7,  // 5 magenta
        8,  // 6 cyan
        0,  // 7 white -> engine default (white)
    };
    return map[ansi & 7];
}

// Blit the grid model into the engine char/attr grids. Colour uses a "filler"
// trick to work around the engine's run-drawer off-by-one: it colours a run by
// the attr of the run's LAST cell, so we put the real colour only on the last
// cell of each colour span and a distinct filler on the interior cells (the
// filler is never drawn -- it just makes the run break in the right place).
void emuBlit() {
    if (!g_moduleBase) return;
    char* gch = gridChars();
    char* gat = gridAttrs();

    std::lock_guard<std::mutex> lk(g_screen.mtx);
    const int cols = g_screen.cols();   // 80
    const bool scrolled = g_screen.viewOffset() > 0;

    for (int r = 0; r < kEmuRows; ++r) {
        for (int c = 0; c < sig::grid_cols; ++c) {
            int gidx = r * sig::grid_cols + c;
            if (c < cols) {
                // viewAt resolves through the scroll-back history when scrolled up.
                const grid::Cell& cell = g_screen.viewAt(r, c);
                uint8_t mapped = ansiToEngine(cell.color);
                // last cell of a colour span? (next cell differs or row end)
                bool lastOfRun = (c + 1 >= cols) ||
                                 (ansiToEngine(g_screen.viewAt(r, c + 1).color) != mapped);
                uint8_t hi = lastOfRun ? mapped : (mapped != 0 ? 0 : 1);  // filler != mapped
                gch[gidx] = cell.ch ? cell.ch : ' ';
                gat[gidx] = static_cast<char>((hi << 4) | 1);
            } else {
                gch[gidx] = ' ';
                gat[gidx] = (1 << 4) | 1;
            }
        }
    }
    // Clear any rows below the emulated area (engine grid is taller than kEmuRows).
    for (int r = kEmuRows; r < sig::grid_rows; ++r)
        for (int c = 0; c < sig::grid_cols; ++c) {
            int gidx = r * sig::grid_cols + c;
            gch[gidx] = ' ';
            gat[gidx] = (1 << 4) | 1;
        }

    if (scrolled) {
        // Scroll-back indicator on the last visible row.
        const char* hint = "-- SCROLLED (PgDn) --";
        int row = kEmuRows - 1;
        for (int i = 0; hint[i] && i < sig::grid_cols; ++i)
            gch[row * sig::grid_cols + i] = hint[i];
    } else {
        // Caret at the cursor so typing is visible (only when live).
        int cr = g_screen.curRow(), cc = g_screen.curCol();
        if (cr >= 0 && cr < kEmuRows && cc >= 0 && cc < sig::grid_cols) {
            int gidx = cr * sig::grid_cols + cc;
            if (gch[gidx] == ' ') gch[gidx] = '_';
        }
    }
}

// Reader thread: pull pty output and feed the parser (under the screen lock).
void emuReaderLoop() {
    char buf[4096];
    while (g_ptyRunning.load()) {
        DWORD n = g_pty.read(buf, sizeof(buf));
        if (n == 0) break;                       // pty closed
        std::lock_guard<std::mutex> lk(g_screen.mtx);
        if (g_parser) g_parser->feed(buf, static_cast<int>(n));
    }
}

void emuStart() {
    g_screen.init(sig::grid_cols, kEmuRows);
    g_parser = new vt::Parser(g_screen);
    if (!g_pty.start(static_cast<short>(sig::grid_cols), kEmuRows, "cmd.exe")) {
        logf("[emu] ConPTY start FAILED\n");
        delete g_parser; g_parser = nullptr;
        g_emuMode = false;
        return;
    }
    g_ptyRunning.store(true);
    g_ptyReader = std::thread(emuReaderLoop);
    logf("[emu] started ConPTY %dx%d\n", sig::grid_cols, kEmuRows);
}

void emuStop() {
    g_ptyRunning.store(false);
    g_pty.stop();                       // unblocks the reader's blocking read()
    if (g_ptyReader.joinable()) g_ptyReader.join();
    delete g_parser; g_parser = nullptr;
    emuClear();
}

// ---- E3 input: WndProc subclass --------------------------------------------
// Recon (input-recon.txt) confirmed the game is message-pump driven (no
// DirectInput), so we subclass the game window: while in emu mode, take typed
// characters from WM_CHAR and special keys from WM_KEYDOWN, translate to VT input
// bytes, write them to the pty, and SWALLOW the message so the game does nothing
// with it (no movement, no ESC pause menu). Inert when not in emu mode.
HWND    g_gameWnd     = nullptr;
WNDPROC g_origWndProc = nullptr;

void ptyWriteStr(const char* s) { g_pty.write(s, strlen(s)); }

// Special (non-character) virtual keys -> VT sequences. Returns true if handled.
bool sendSpecialKey(WPARAM vk) {
    const char* seq = nullptr;
    switch (vk) {
    case VK_UP:     seq = "\x1b[A";  break;
    case VK_DOWN:   seq = "\x1b[B";  break;
    case VK_RIGHT:  seq = "\x1b[C";  break;
    case VK_LEFT:   seq = "\x1b[D";  break;
    case VK_HOME:   seq = "\x1b[H";  break;
    case VK_END:    seq = "\x1b[F";  break;
    case VK_PRIOR:  seq = "\x1b[5~"; break;   // PgUp
    case VK_NEXT:   seq = "\x1b[6~"; break;   // PgDn
    case VK_DELETE: seq = "\x1b[3~"; break;
    case VK_INSERT: seq = "\x1b[2~"; break;
    default: return false;
    }
    ptyWriteStr(seq);
    return true;
}

LRESULT CALLBACK emuWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_emuMode) {
        switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Exit hotkey: Ctrl+]  (leaves emu mode; ESC then closes the terminal).
            if (wp == VK_OEM_6 && (GetKeyState(VK_CONTROL) & 0x8000)) {
                g_emuMode = false;
                emuStop();
                return 0;
            }
            // PgUp/PgDn: scroll the local scroll-back on the normal screen; pass
            // through to the app (as VT) when an alt-screen program is running.
            if (wp == VK_PRIOR || wp == VK_NEXT) {
                std::lock_guard<std::mutex> lk(g_screen.mtx);
                if (g_screen.altScreen()) {
                    ptyWriteStr(wp == VK_PRIOR ? "\x1b[5~" : "\x1b[6~");
                } else if (wp == VK_PRIOR) {
                    g_screen.scrollViewUp(kEmuRows / 2);
                } else {
                    g_screen.scrollViewDown(kEmuRows / 2);
                }
                return 0;
            }
            { std::lock_guard<std::mutex> lk(g_screen.mtx); g_screen.scrollToLive(); }
            sendSpecialKey(wp);     // arrows/nav -> VT; printables arrive via WM_CHAR
            return 0;               // swallow so the game never sees the key
        case WM_KEYUP:
        case WM_SYSKEYUP:
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR: {
            { std::lock_guard<std::mutex> lk(g_screen.mtx); g_screen.scrollToLive(); }
            char c = static_cast<char>(wp);
            if (c == '\r')      ptyWriteStr("\r");
            else if (c == '\b') { char del = 0x7f; g_pty.write(&del, 1); }  // BS -> DEL
            else if (c != 0)    g_pty.write(&c, 1);
            return 0;
        }
        }
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

BOOL CALLBACK enumWndCb(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) &&
        GetWindow(h, GW_OWNER) == nullptr) {
        char title[256] = {0};
        GetWindowTextA(h, title, sizeof(title));
        if (title[0]) { g_gameWnd = h; return FALSE; }   // first visible titled top-level
    }
    return TRUE;
}

void installInputHook() {
    EnumWindows(enumWndCb, 0);
    if (g_gameWnd) {
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(emuWndProc)));
        logf("[emu] input hook on HWND %p (orig %p)\n", g_gameWnd, (void*)g_origWndProc);
    } else {
        logf("[emu] input hook: game window NOT FOUND\n");
    }
}

void removeInputHook() {
    if (g_gameWnd && g_origWndProc) {
        SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
    }
}

// Drip queued shell output into the terminal, a little per frame, throttled so
// the write head never races too far ahead of the reveal head (which would
// overrun the 100-row ring). Called from the per-frame renderer hook.
void pumpOutput() {
    if (g_paused) return;            // waiting for the user to continue past --MORE--
    const int kMaxBacklog   = 24;    // keep written-minus-revealed under this
    const int kLinesPerFrame = 3;    // cap work per frame
    for (int i = 0; i < kLinesPerFrame; ++i) {
        int written  = readGlobalInt(sig::lines_written_rva);
        int revealed = readGlobalInt(sig::lines_shown_rva);
        if (written - revealed >= kMaxBacklog) break;   // let the reveal catch up

        // One screenful done and more is waiting -> page break.
        if (g_pageLines >= kPageSize && !g_outbound.empty()) {
            termPrintLine("--MORE--");
            g_paused = true;
            return;
        }
        auto line = g_outbound.tryPop();
        if (!line) break;
        termPrintLine(line->c_str());
        // Count the on-screen rows this line occupies: it wraps every 80 cols.
        int len = static_cast<int>(line->size());
        g_pageLines += (len <= 0) ? 1 : (len + 79) / 80;
    }
}

// Abort the command in flight and the flood of output waiting to render: tell
// the worker to drop the running command's result, empty both queues, and snap
// the pager back to a clean prompt. NOTE: this stops the *output* -- it does not
// hard-kill a child process that is still streaming (e.g. `ping -t`); that one
// keeps running on the worker until its 15s timeout, but its output is discarded.
void cancelOutput() {
    bridge::requestCancel();
    while (g_inbound.tryPop()) {}        // drop anything queued but not yet run
    while (g_outbound.tryPop()) {}       // drop output not yet rendered
    g_paused = false;
    g_pageLines = 0;
    // '^' starts a colour code in line_append, so don't print "^C" literally.
    termPrintLine("(canceled)");
}

// ---- frame_render hook (the per-frame pump) ---------------------------------
// FUN_00575be0: __cdecl with 6 dword-sized args; we forward them verbatim.
using Frame_t = void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
Frame_t orig_frame = nullptr;

// Poll Ctrl-C each frame while connected. Edge-detected so holding the keys
// fires once, not every frame. GetAsyncKeyState works regardless of which window
// element has focus, which the in-game text input doesn't route to us.
void checkCancel() {
    static bool prev = false;
    bool down = g_shellMode
             && (GetAsyncKeyState(VK_CONTROL) & 0x8000)
             && (GetAsyncKeyState('C') & 0x8000);
    if (down && !prev) cancelOutput();
    prev = down;
}

void __cdecl hook_frame(uint32_t a, uint32_t b, uint32_t c,
                        uint32_t d, uint32_t e, uint32_t f) {
    if (g_emuMode) {
        // Paint our grid, then pin reveal==write so the renderer's reveal/ring
        // half is skipped and only the draw loop runs (drawing our grid).
        emuBlit();
        writeGlobalInt(sig::lines_shown_rva, readGlobalInt(sig::lines_written_rva));
    } else {
        checkCancel();                  // Ctrl-C aborts a flood of output
        pumpOutput();                   // feed output BEFORE the reveal pass
    }
    if (orig_frame) orig_frame(a, b, c, d, e, f);
}

// ---- dispatch_submit hook ---------------------------------------------------
// FUN_0055f800: void __cdecl(ctx). Fires on Enter; main thread.
using Dispatch_t = void(__cdecl*)(void* ctx);
Dispatch_t orig_dispatch = nullptr;

std::string readInput() {
    if (!g_moduleBase) return {};
    const char* p = reinterpret_cast<const char*>(g_moduleBase + sig::input_buffer_rva);
    std::string s = p ? p : "";
    // trim leading/trailing whitespace
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

bool iequals(const std::string& x, const char* y) {
    size_t i = 0;
    for (; i < x.size() && y[i]; ++i)
        if (std::tolower((unsigned char)x[i]) != std::tolower((unsigned char)y[i])) return false;
    return i == x.size() && y[i] == '\0';
}

// Run orig_dispatch with the game's visible output suppressed (preserves the
// engine's input/terminal bookkeeping).
void runDispatchSilently(void* ctx) {
    g_inDispatch = true;
    if (orig_dispatch) orig_dispatch(ctx);
    g_inDispatch = false;
}

void __cdecl hook_dispatch(void* ctx) {
    // If we're paused at a --MORE-- break, this Enter just continues paging --
    // consume it (don't treat it as a command). Typing 'q'/'quit' here stops the
    // rest of the output, like a real pager.
    if (g_paused) {
        if (iequals(readInput(), "q") || iequals(readInput(), "quit")) {
            cancelOutput();
        } else {
            g_paused = false;
            g_pageLines = 0;
        }
        return;
    }

    std::string input = readInput();

    // Emulator (E1) toggle: 'emu' flips raw-grid mode on/off. While on, swallow
    // every other command (we're painting the grid, not running the BO1 shell).
    if (iequals(input, "emu")) {
        g_emuMode = !g_emuMode;
        if (g_emuMode) emuStart();           // spawn the pty + parser pipeline
        else           emuStop();            // tear it down, wipe the grid
        runDispatchSilently(ctx);
        logf("[hooks] emu mode %s\n", g_emuMode ? "ON" : "OFF");
        return;
    }
    if (g_emuMode) { runDispatchSilently(ctx); return; }

    const bool toggle = iequals(input, kToggleWord);

    if (!g_shellMode) {
        if (toggle) {
            g_shellMode = true;
            runDispatchSilently(ctx);                // hide the toggle word echo path
            g_outbound.push("");
            g_outbound.push("[ host shell connected -- type 'connect' to return ]");
            g_outbound.push("");
            return;
        }
        if (orig_dispatch) orig_dispatch(ctx);       // normal BO1 terminal
        return;
    }

    // --- shell mode ---
    if (toggle) {
        g_shellMode = false;
        runDispatchSilently(ctx);
        g_outbound.push("[ disconnected ]");
        return;
    }
    if (input.empty()) {
        if (orig_dispatch) orig_dispatch(ctx);
        return;
    }
    // Suppress the game's built-in handling; queue the command for the worker
    // thread (async -- the per-frame pump renders the output). Reset the pager
    // so this command's output starts a fresh screenful.
    runDispatchSilently(ctx);
    g_pageLines = 0;
    logf("[hooks] shell: \"%s\"\n", input.c_str());
    g_inbound.push(input);
}

bool createHookAt(const char* name, uintptr_t addr, void* detour, void** orig) {
    if (!addr) { logf("[hooks] %s: null addr\n", name); return false; }
    if (MH_CreateHook(reinterpret_cast<void*>(addr), detour, orig) != MH_OK) {
        logf("[hooks] %s: MH_CreateHook failed @ %p\n", name, (void*)addr);
        return false;
    }
    MH_EnableHook(reinterpret_cast<void*>(addr));
    logf("[hooks] %s: hooked @ %p\n", name, (void*)addr);
    return true;
}

template <typename T>
bool createHook(const char* name, const char* pattern, void* detour, T& orig) {
    if (!pattern) { logf("[hooks] %s: no signature, skipped\n", name); return false; }
    uintptr_t addr = sigscan::findInModule("BlackOps.exe", pattern);
    if (!addr) { logf("[hooks] %s: signature NOT FOUND\n", name); return false; }
    return createHookAt(name, addr, detour, reinterpret_cast<void**>(&orig));
}

// Resolve an address by RVA (ASLR is off) and verify its prologue before use.
uintptr_t resolveRva(const char* name, uintptr_t rva, const char* verify) {
    uintptr_t addr = g_moduleBase + rva;
    if (verify && !sigscan::matches(addr, verify)) {
        logf("[hooks] %s: prologue MISMATCH @ %p\n", name, (void*)addr);
        return 0;
    }
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

    // line_commit has no unique signature -> RVA + prologue verification.
    uintptr_t commit = g_moduleBase + sig::line_commit_rva;
    if (sigscan::matches(commit, sig::line_commit_verify)) {
        fn_line_commit = reinterpret_cast<LineCommit_t>(commit);
        logf("[hooks] line_commit verified @ %p\n", (void*)commit);
    } else {
        fn_line_commit = nullptr;
        logf("[hooks] line_commit prologue MISMATCH @ %p -- commit skipped\n", (void*)commit);
    }

    bool la = createHook("line_append",     sig::line_append,     (void*)&hook_line_append, orig_line_append);
    bool dp = createHook("dispatch_submit", sig::submit_handler,  (void*)&hook_dispatch,    orig_dispatch);

    // Per-frame pump: hook the terminal renderer (RVA + verify, ASLR off).
    uintptr_t frameAddr = resolveRva("frame_render", sig::frame_render_rva, sig::frame_render_verify);
    bool fr = createHookAt("frame_render", frameAddr, (void*)&hook_frame, reinterpret_cast<void**>(&orig_frame));

    // Emulator E3: subclass the game window for live keyboard input (inert until
    // emu mode is on). Not fatal if the window isn't found.
    installInputHook();

    return la && dp && fr;
}

void remove() {
    if (g_emuMode) { g_emuMode = false; emuStop(); }
    removeInputHook();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

}  // namespace hooks
