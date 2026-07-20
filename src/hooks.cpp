#include "hooks.h"
#include "bridge.h"
#include "sigscan.h"
#include "queues.h"
#include "conpty.h"
#include "grid.h"
#include "vtparse.h"

#include <windows.h>
#include <timeapi.h>     // timeGetTime (excluded by WIN32_LEAN_AND_MEAN); link winmm
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

    // glyph_run_draw = FUN_005f4a90 (RVA 0x1F4A90). void __cdecl(ctx, text, count,
    // ...11 args). The engine's text-run drawer (measure + transform + emit). We
    // hook it to suppress exactly ONE draw: the game's native input line. The
    // renderer draws that line at the bottom (screen line 0x23) using the read-only
    // string &DAT_00a321c4 as its text and the *previous* row's pixel width as its
    // x-offset, so it shows a blinking caret that drifts right with our row-26 text.
    // We can't blank the buffer (it's in .rdata -> writing crashes), so instead we
    // compare the text pointer and skip the call when it's that string in emu mode.
    constexpr uintptr_t  glyph_run_draw_rva = 0x001F4A90;
    constexpr const char* glyph_run_draw_verify =
        "83 EC 08 D9 44 24 2C 56 8B 74 24 1C 51 D9 1C 24 56";
    constexpr uintptr_t  native_input_rva   = 0x006321C4;  // &DAT_00a321c4 (.rdata, RO)
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
    for (int i = 0; i < n; ++i) { ch[i] = ' '; at[i] = (0 << 4) | 1; }
}

// --- E2 pipeline: ConPTY child -> reader thread -> VT parser -> grid -> blit --
// The engine only DISPLAYS 27 rows (the full engine grid, 0..26), but we run a
// TALLER virtual grid + PTY so a repaint-style TUI (Claude Code) lays out across
// more rows than fit on screen; the visible 27-row window follows the cursor and
// PgUp/PgDn pages up/down through it (and on into the bounded scroll-back). cmd and
// scrolling output are unaffected -- the window just tracks the cursor.
const int kEmuDisplayRows = 27;    // engine-visible rows (== sig::grid_rows)
const int kEmuGridRows    = 8000;  // grid == PTY height reported to apps. STEP 1 proved
                                   // Claude clips each render to the reported height and
                                   // repaints in place (no scrollback of its own), so a
                                   // tall viewport is the ONLY way to retain long responses
                                   // + conversation history; this tall grid IS Claude's
                                   // scrollback. The 27-row display windows over it with
                                   // large blank gaps collapsed (Screen::updateViewAnchor).
// Right-edge (80 cols total). Col 79 is ALWAYS a NUL terminator: the glyph emitter
// FUN_006d65c0 does strlen() per run and skips rows whose first char is NUL, so a row with
// no NUL over-reads and each run costs strlen+0x60 bytes -- a screenful exhausts the shared
// vertex buffer and the bottom rows get dropped. So content is 79 cols (0..78). The engine's
// run-colour off-by-one (FUN_00575be0 colours a run by the cell that BREAKS it) is
// neutralised WITHOUT spending a column: the terminating NUL cell's attr is set to the last
// drawn colour (the engine reads that attr for the colour but strlen never draws the NUL).
// The scrollbar THUMB doesn't cover text: on its rows it RECOLOURS the rightmost column (78)
// -- cyan, or blue if that cell is already cyan -- keeping whatever glyph is there; a blank
// thumb cell gets a bar so the thumb stays visible. Only thumb rows pay vertex budget.
const int kThumbCol    = sig::grid_cols - 2;   // col 78 = rightmost content col = thumb marker
const int kContentCols = sig::grid_cols - 1;   // 79 columns reported to the PTY (0..78)

conpty::Session  g_pty;
grid::Screen     g_screen;
vt::Parser*      g_parser = nullptr;
std::thread      g_ptyReader;
std::atomic<bool> g_ptyRunning{false};

// Typewriter reveal (matches the game's native terminal). The reader thread only
// buffers raw pty bytes here; the per-frame pump (emuPump, main thread) meters
// them into the parser at the native rate -- ~1 visible glyph per millisecond
// (the engine's own reveal: budget = elapsed ms, paced by timeGetTime; see
// FUN_00575be0 / FUN_0062ef60). g_pending is the unrevealed backlog.
std::mutex   g_pendMtx;
std::string  g_pending;
DWORD        g_revealLast  = 0;  // timeGetTime() at the last pump (0 = uninit)
DWORD        g_instantUntil = 0; // feed instantly (no typewriter) until this time
bool         g_instantAll  = false; // Ctrl+\ toggles: force instant reveal (no typewriter)

// Blit the grid model into the engine char/attr grids.
//
// Two engine quirks shape this (both confirmed from the decompiled renderer
// FUN_00575be0 / glyph emitter FUN_006d65c0):
//
//  1. NUL-terminated rows + shared vertex budget. The draw loop reads each grid
//     row as a NUL-terminated C-string into one per-frame text-vertex buffer with
//     a hard size cap; a row with no NUL is read ~2KB past its end, and a screen
//     of such runs exhausts the buffer, after which EVERY later run that frame is
//     silently dropped (bottom rows vanish). So every row is NUL-terminated after
//     its last glyph.
//
//  2. Run-colour off-by-one. The draw loop splits a row into runs of equal attr
//     and colours each run by the attr of the cell that ENDS the run (its
//     boundary), not the run's own cells. Painting each cell its true colour
//     therefore bleeds every segment's colour onto its neighbour. We invert it
//     with a "filler" scheme: within a colour segment the interior cells get a
//     throwaway attr and only the segment's LAST cell carries the real colour, so
//     the engine's run boundary lands exactly on the segment end. The filler is
//     the PREVIOUS segment's colour -- always != this segment's colour (adjacent
//     segments differ by definition), and it makes the boundary cell that the
//     run-drawer "steals" from the next segment land on the correct colour.
// Per-cell true 24-bit foreground for the current frame (0x00RRGGBB, or kDefaultFg).
// Written by emuBlit (main thread); read by hook_glyphrun during the same frame's render
// to colour each run, bypassing the engine's 8-slot palette. Same thread, sequential.
uint32_t g_trueRGB[sig::grid_rows][sig::grid_cols];

void emuBlit() {
    if (!g_moduleBase) return;
    char* gch = gridChars();
    char* gat = gridAttrs();

    std::lock_guard<std::mutex> lk(g_screen.mtx);
    // ONE display model for everything (cmd and Claude): a `kEmuDisplayRows` window over
    // the history++grid buffer, anchored to the bottom-most non-blank row. The tall grid
    // holds Claude's full render; the anchor auto-follows the newest line and collapses
    // trailing blanks. PgUp/wheel slide the window up.
    g_screen.updateViewAnchor();
    const int cols = g_screen.cols();   // 80
    const bool scrolled = g_screen.viewOffset() > 0;   // scroll-back review

    // First pass: glyphs + the true 24-bit fg per cell. Spaces inherit the preceding
    // glyph's fg so a lone coloured char extends across following spaces into one colour
    // segment (leading spaces keep the default).
    for (int r = 0; r < sig::grid_rows; ++r) {
        char* row = gch + r * sig::grid_cols;
        uint32_t lastFg = grid::kDefaultFg;
        for (int c = 0; c < sig::grid_cols; ++c) {
            if (r < kEmuDisplayRows && c < cols) {
                const grid::Cell& cell = g_screen.viewAt(r, c);
                char ch = cell.ch ? cell.ch : ' ';
                row[c] = ch;
                if (ch == ' ') { g_trueRGB[r][c] = lastFg; }
                else           { g_trueRGB[r][c] = cell.fg; lastFg = cell.fg; }
            } else {
                row[c] = ' ';
                g_trueRGB[r][c] = lastFg;
            }
        }
    }

    // Scrollbar thumb geometry (thumb only, so blank/track rows stay NUL-at-0 = free
    // vertex budget). A proportional thumb in col 78 marks where the window sits.
    const int seqLen  = g_screen.viewSeqLen();
    const bool sbOn   = seqLen > kEmuDisplayRows;
    int thumbTop = 0, thumbLen = 0;
    if (sbOn) {
        thumbLen = kEmuDisplayRows * kEmuDisplayRows / seqLen;
        if (thumbLen < 2) thumbLen = 2;
        if (thumbLen > kEmuDisplayRows) thumbLen = kEmuDisplayRows;
        int span  = kEmuDisplayRows - thumbLen;
        int denom = seqLen - kEmuDisplayRows;
        thumbTop  = denom > 0 ? g_screen.viewTop() * span / denom : 0;
        if (thumbTop < 0) thumbTop = 0;
        if (thumbTop > span) thumbTop = span;
    }

    // Thumb: recolour col 78 on thumb rows (cyan, or blue over a cyan cell), preserving the
    // glyph (blank -> a '#' bar). The colour rides the same true-fg path (the hook renders
    // it); the distinct fg just needs to break a run there.
    if (sbOn) {
        for (int r = thumbTop; r < thumbTop + thumbLen && r < kEmuDisplayRows; ++r) {
            char* row = gch + r * sig::grid_cols;
            uint32_t u = g_trueRGB[r][kThumbCol];
            bool cyanish = ((u >> 16 & 0xFF) < 0x40) && ((u >> 8 & 0xFF) > 0xA0) && ((u & 0xFF) > 0xA0);
            g_trueRGB[r][kThumbCol] = cyanish ? 0x5C5CFFu : 0x00FFFFu;
            if (row[kThumbCol] == ' ' || row[kThumbCol] == '\0') row[kThumbCol] = '#';
        }
    }

    // Blinking caret at the cursor (glyph only), hidden while reviewing scroll-back.
    if (!scrolled) {
        int cr = g_screen.cursorVisibleRow();
        int cc = g_screen.curCol();
        if (cr >= 0 && cr < kEmuDisplayRows && cc >= 0 && cc < sig::grid_cols) {
            int gidx = cr * sig::grid_cols + cc;
            bool blinkOn = ((GetTickCount() / 500) & 1) == 0;
            if (gch[gidx] == ' ' || gch[gidx] == '\0') {
                gch[gidx] = blinkOn ? '_' : ' ';
            } else if (!blinkOn) {
                gch[gidx] = ' ';
            }
        }
    }

    // Run-id per cell: a nibble that changes whenever the true fg changes, cycling 1..7 so
    // adjacent segments differ. This makes the engine break runs exactly at colour
    // boundaries; the filler scheme below aligns each run to its segment (so the run-colour
    // off-by-one is harmless) and hook_glyphrun colours each run from g_trueRGB.
    static uint8_t hicol[sig::grid_rows][sig::grid_cols];
    for (int r = 0; r < sig::grid_rows; ++r) {
        uint8_t nib = 0; uint32_t prev = 0xFFFFFFFEu;   // impossible fg
        for (int c = 0; c < sig::grid_cols; ++c) {
            if (g_trueRGB[r][c] != prev) { nib = static_cast<uint8_t>((nib % 7) + 1); prev = g_trueRGB[r][c]; }
            hicol[r][c] = nib;
        }
    }

    // Second pass: filler scheme over the run-ids -> attr bytes (aligns each engine run to a
    // colour segment); NUL-terminate the trailing region. Col 79 is ALWAYS NUL; blank rows
    // end NUL-at-0 (zero vertex cost). Colours are applied in the hook, not here.
    for (int r = 0; r < sig::grid_rows; ++r) {
        char* row = gch + r * sig::grid_cols;
        char* att = gat + r * sig::grid_cols;

        int lastGlyph = -1;
        for (int c = 0; c < kContentCols; ++c)
            if (row[c] != ' ' && row[c] != '\0') lastGlyph = c;

        int last;
        const bool thumbRow = sbOn && r >= thumbTop && r < thumbTop + thumbLen;
        if (thumbRow)            last = kThumbCol;
        else if (lastGlyph >= 0) last = lastGlyph;
        else                     last = -1;

        uint8_t prev = 0xFF;        // previous segment's run-id (none yet)
        int segStart = 0;
        for (int c = 0; c <= last; ++c) {
            if (c == last || hicol[r][c + 1] != hicol[r][c]) {
                uint8_t v = hicol[r][c];
                uint8_t filler = (prev == 0xFF) ? (v == 2 ? 0 : 2) : prev;
                for (int k = segStart; k < c; ++k) att[k] = static_cast<char>((filler << 4) | 1);
                att[c] = static_cast<char>((v << 4) | 1);
                prev = v;
                segStart = c + 1;
            }
        }
        for (int c = last + 1; c < sig::grid_cols; ++c) { row[c] = '\0'; att[c] = (0 << 4) | 1; }
    }
}

// Diagnostic (off by default): capture raw pty bytes to a file for offline replay through
// feed_log. Flip to true when debugging the parser/renderer against a real session.
constexpr bool kCapturePty = false;
static const char* kCaptureFile = "C:\\Users\\manus\\bo1_pty.log";

// Reader thread: pull pty output and buffer it raw. Parsing happens on the main
// thread in emuPump so the reveal can be metered to the engine's native rate.
void emuReaderLoop() {
    char buf[4096];
    while (g_ptyRunning.load()) {
        DWORD n = g_pty.read(buf, sizeof(buf));
        if (n == 0) break;                       // pty closed
        if (kCapturePty) {
            FILE* f = fopen(kCaptureFile, "ab");
            if (f) { fwrite(buf, 1, n, f); fclose(f); }
        }
        std::lock_guard<std::mutex> lk(g_pendMtx);
        g_pending.append(buf, n);
    }
}

// Per-frame metered feed (main thread). Two modes:
//   * Typewriter (plain append output: dir, cat, build logs) -- reveals at the
//     engine's native rate: ~1 visible glyph per elapsed millisecond, in stream
//     order, so text appears left-to-right / top-to-bottom exactly like the
//     game's own terminal (paced off timeGetTime; see FUN_00575be0/FUN_0062ef60).
//   * Instant (full-screen TUIs: vim, htop, Claude Code) -- fed all at once so
//     in-place repaints/animations aren't slowed. A TUI is detected by the alt
//     screen OR by the stream moving the cursor up (Screen::repaint()); once
//     seen, instant mode sticks for a short window so steady repaint traffic
//     stays smooth, then typewriter resumes for ordinary output.
// Escape/control bytes are processed for free (only visible glyphs are metered).
// A large backlog (e.g. `cat bigfile`) is fast-forwarded above a cap so it can't
// take minutes.
constexpr size_t kBacklogCap   = 8192;   // bytes; excess dumped instantly
constexpr DWORD  kInstantHold  = 750;    // ms instant mode lingers after a repaint
void emuPump() {
    if (!g_parser) return;

    DWORD now = timeGetTime();
    if (g_revealLast == 0) g_revealLast = now;   // first frame: no burst
    DWORD budget = now - g_revealLast;           // glyphs to reveal this frame (1/ms)
    g_revealLast = now;

    std::string chunk;
    { std::lock_guard<std::mutex> lk(g_pendMtx); chunk.swap(g_pending); }
    if (chunk.empty()) return;

    size_t pos = 0;
    {
        std::lock_guard<std::mutex> lk(g_screen.mtx);
        bool instant = g_instantAll || g_screen.altScreen() ||
                       static_cast<int>(g_instantUntil - now) > 0;
        g_screen.setTuiActive(instant);     // gates the cls-detector during this feed
        if (instant) {
            // Repaint TUI (Claude): feed the whole burst at once (no typewriter). The
            // tall grid holds the full render; emuBlit shows the split window into it.
            g_parser->feed(chunk.data(), static_cast<int>(chunk.size()));
            pos = chunk.size();
            if (g_screen.repaint()) g_instantUntil = now + kInstantHold;
        } else {
            // Fast-forward: keep the reveal no more than kBacklogCap bytes behind
            // the live stream, dumping the excess instantly.
            if (chunk.size() > kBacklogCap) {
                size_t ff = chunk.size() - kBacklogCap;
                g_parser->feed(chunk.data(), static_cast<int>(ff));
                pos = ff;
            }
            // Typewriter: feed one byte at a time until `budget` glyphs land, or
            // until a cursor-up reveals a TUI -> flip to instant for the rest.
            g_screen.clearRepaint();
            uint64_t start = g_screen.produced();
            for (; pos < chunk.size(); ++pos) {
                g_parser->feed(chunk.data() + pos, 1);
                if (g_screen.repaint()) {                 // TUI repaint detected
                    g_instantUntil = now + kInstantHold;
                    if (++pos < chunk.size())
                        g_parser->feed(chunk.data() + pos,
                                       static_cast<int>(chunk.size() - pos));
                    pos = chunk.size();
                    break;
                }
                if (g_screen.produced() - start >= budget) { ++pos; break; }
            }
        }
    }
    // Return the unrevealed tail to the front of the backlog for next frame.
    if (pos < chunk.size()) {
        std::lock_guard<std::mutex> lk(g_pendMtx);
        g_pending.insert(0, chunk, pos, chunk.size() - pos);
    }
}

// BlackOps.exe is 32-bit, so launching a plain "cmd.exe" gets the 32-bit cmd
// (WOW64 redirects System32 -> SysWOW64). Inside that 32-bit shell, C:\Windows\
// System32 is likewise redirected, so 64-bit-only tools that live only in the
// real System32 -- notably ssh/scp in System32\OpenSSH -- come back "not
// recognized". Launch the native 64-bit cmd via the Sysnative alias (visible
// only to 32-bit processes) so the shell sees the real System32 and PATH stays
// untouched. Falls back to "cmd.exe" if Sysnative isn't present.
// Build the shell launch command line. Default to PowerShell (per user choice): pwsh 7 if
// installed (native UTF-8), else Windows PowerShell 5.1. Both are launched with a startup
// script that forces UTF-8 for the console AND makes Get-Content/Set-Content default to
// UTF-8 -- so `cat`/`gc` of a UTF-8 file renders correctly (5.1's Get-Content otherwise
// defaults to ANSI and mojibakes; cmd's `type` mangles UTF-8 at buffer boundaries). Falls
// back to cmd.exe (UTF-8 code page) if no PowerShell is found. 64-bit (pwsh, or 5.1 via the
// Sysnative alias) so System32 tools (ssh) stay visible. You can still switch shells at
// runtime by typing cmd / wsl / etc.
std::string shellCommand() {
    // Startup script: UTF-8 console + default UTF-8 file encodings. Single quotes inside so
    // the whole -Command argument can sit in one double-quoted string.
    static const char* kPsInit =
        " -NoLogo -NoExit -Command \""
        "[Console]::OutputEncoding=[Text.Encoding]::UTF8;"
        "$OutputEncoding=[Text.Encoding]::UTF8;"
        "$PSDefaultParameterValues['Get-Content:Encoding']='UTF8';"
        "$PSDefaultParameterValues['Set-Content:Encoding']='UTF8'\"";

    char buf[MAX_PATH];

    // 1. PowerShell 7 (pwsh): <ProgramW6432>\PowerShell\7\pwsh.exe, else on PATH.
    std::string pwsh;
    DWORD n = GetEnvironmentVariableA("ProgramW6432", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string cand = std::string(buf) + "\\PowerShell\\7\\pwsh.exe";
        if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) pwsh = cand;
    }
    if (pwsh.empty()) {
        char found[MAX_PATH];
        if (SearchPathA(nullptr, "pwsh.exe", nullptr, MAX_PATH, found, nullptr)) pwsh = found;
    }
    if (!pwsh.empty()) return "\"" + pwsh + "\"" + kPsInit;

    // 2. Windows PowerShell 5.1 (64-bit via Sysnative; System32 if not under WOW64).
    if (GetWindowsDirectoryA(buf, MAX_PATH)) {
        std::string base = buf;
        std::string ps = base + "\\Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe";
        if (GetFileAttributesA(ps.c_str()) == INVALID_FILE_ATTRIBUTES)
            ps = base + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        if (GetFileAttributesA(ps.c_str()) != INVALID_FILE_ATTRIBUTES)
            return "\"" + ps + "\"" + kPsInit;
    }

    // 3. Last resort: cmd.exe with the UTF-8 code page.
    if (GetWindowsDirectoryA(buf, MAX_PATH)) {
        std::string cmd = std::string(buf) + "\\Sysnative\\cmd.exe";
        if (GetFileAttributesA(cmd.c_str()) != INVALID_FILE_ATTRIBUTES)
            return "\"" + cmd + "\" /k chcp 65001>nul";
    }
    return "cmd.exe /k chcp 65001>nul";
}

void emuStart() {
    g_screen.init(sig::grid_cols, kEmuGridRows);
    g_screen.setDisplayRows(kEmuDisplayRows);   // engine shows 27 of the taller grid
    { std::lock_guard<std::mutex> lk(g_pendMtx); g_pending.clear(); }
    g_revealLast = 0;
    g_instantUntil = 0;
    if (kCapturePty) { FILE* f = fopen(kCaptureFile, "wb"); if (f) fclose(f); }
    g_parser = new vt::Parser(g_screen);
    // Answer terminal queries (DSR/DA) by writing back to the pty so size
    // detection works instantly instead of stalling on a ~15s timeout.
    g_parser->setResponder([](const char* s, int n) { g_pty.write(s, (size_t)n); });
    // Start the native 64-bit shell (see nativeShell) in the user's home dir
    // rather than the game's install folder (the child would otherwise inherit
    // BlackOps.exe's working directory).
    // Default to PowerShell with UTF-8 set up (see shellCommand) so UTF-8 files/output
    // render correctly. Switch to any other shell at runtime by typing its name.
    std::string shell = shellCommand();
    const char* home = getenv("USERPROFILE");
    // Report width kContentCols (79): apps wrap at 79, leaving only col 79 as the NUL
    // terminator. Height = the tall grid so Claude renders its full content into it.
    if (!g_pty.start(static_cast<short>(kContentCols), kEmuGridRows, shell.c_str(), home)) {
        logf("[emu] ConPTY start FAILED\n");
        delete g_parser; g_parser = nullptr;
        g_emuMode = false;
        return;
    }
    g_ptyRunning.store(true);
    g_ptyReader = std::thread(emuReaderLoop);
    logf("[emu] started ConPTY %dx%d (display %d)\n", sig::grid_cols, kEmuGridRows, kEmuDisplayRows);
}

void emuStop() {
    g_ptyRunning.store(false);
    g_pty.stop();                       // unblocks the reader's blocking read()
    if (g_ptyReader.joinable()) g_ptyReader.join();
    delete g_parser; g_parser = nullptr;
    { std::lock_guard<std::mutex> lk(g_pendMtx); g_pending.clear(); }
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
        case WM_MOUSEWHEEL: {
            // Wheel scrolls the scroll-back window (alt-screen apps get wheel-as-arrows).
            int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;   // +up / -down
            std::lock_guard<std::mutex> lk(g_screen.mtx);
            if (g_screen.altScreen()) {
                for (int i = 0; i < abs(notches) * 3; ++i)
                    ptyWriteStr(notches > 0 ? "\x1b[A" : "\x1b[B");
            } else if (notches > 0) {
                g_screen.scrollViewUp(notches * 3);
            } else if (notches < 0) {
                g_screen.scrollViewDown(-notches * 3);
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Instant-reveal toggle: Ctrl+\  (turn the typewriter reveal on/off).
            if (wp == VK_OEM_5 && (GetKeyState(VK_CONTROL) & 0x8000)) {
                g_instantAll = !g_instantAll;
                return 0;
            }
            // Exit hotkey: Ctrl+]  (leaves emu mode; ESC then closes the terminal).
            if (wp == VK_OEM_6 && (GetKeyState(VK_CONTROL) & 0x8000)) {
                g_emuMode = false;
                emuStop();
                return 0;
            }
            // PgUp/PgDn: alt-screen apps (vim/less) page themselves (forward VT);
            // everything else (cmd AND Claude) scrolls the unified scroll-back window.
            if (wp == VK_PRIOR || wp == VK_NEXT) {
                std::lock_guard<std::mutex> lk(g_screen.mtx);
                if (g_screen.altScreen()) {
                    ptyWriteStr(wp == VK_PRIOR ? "\x1b[5~" : "\x1b[6~");
                } else if (wp == VK_PRIOR) {
                    g_screen.scrollViewUp(kEmuDisplayRows / 2);
                } else {
                    g_screen.scrollViewDown(kEmuDisplayRows / 2);
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
        // Paint our grid, then pin the reveal head to the write head so the
        // renderer's typewriter-reveal half is skipped (it would otherwise repaint
        // the line ring over our grid). The draw loop's second half then paints all
        // 27 grid rows unconditionally -- every row is visible thanks to the
        // per-row NUL termination in emuBlit (see the notes there).
        emuPump();      // meter pty output into the grid at the native reveal rate
        emuBlit();
        writeGlobalInt(sig::lines_shown_rva, readGlobalInt(sig::lines_written_rva));
    } else {
        checkCancel();                  // Ctrl-C aborts a flood of output
        pumpOutput();                   // feed output BEFORE the reveal pass
    }
    if (orig_frame) orig_frame(a, b, c, d, e, f);
}

// ---- glyph_run_draw hook (suppress the native input caret in emu mode) ------
// FUN_005f4a90: void __cdecl(ctx, text, count, ...). The engine's text-run
// drawer. In emu mode we skip the single call that draws the game's own input
// line (text == &DAT_00a321c4) so its blinking caret doesn't show over our grid.
// Pointer compare only -- we never write to that read-only string. All other
// draws (including our grid rows) pass straight through.
using GlyphRunDraw_t = void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t,
                                      float, float, uint32_t, uint32_t,
                                      uint32_t, uint32_t, uint32_t);
GlyphRunDraw_t orig_glyphrun = nullptr;

void __cdecl hook_glyphrun(uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4,
                           float p5, float p6, uint32_t p7, uint32_t p8,
                           uint32_t p9, uint32_t p10, uint32_t p11) {
    if (g_emuMode && g_moduleBase) {
        if (p2 == static_cast<uint32_t>(g_moduleBase + sig::native_input_rva))
            return;  // skip the game's native input line over our grid

        // True-colour override: p2 is the run's text ptr into our grid; p10 is the RGBA
        // float[4] the engine resolved from its 8-slot palette. Look up the run's true fg
        // (from its first cell) and substitute it, so we're not limited to 8 colours. The
        // engine copies the colour synchronously in this call, so a stack array is safe.
        uint32_t gridBase = static_cast<uint32_t>(g_moduleBase + sig::grid_chars_rva);
        uint32_t end = gridBase + sig::grid_rows * sig::grid_cols;
        if (p2 >= gridBase && p2 < end) {
            uint32_t off = p2 - gridBase;
            int row = off / sig::grid_cols, col = off % sig::grid_cols;
            uint32_t fg = g_trueRGB[row][col];
            uint32_t rgb = (fg == grid::kDefaultFg) ? 0xFFFFFFu : (fg & 0xFFFFFFu);
            if ((rgb & 0xFFFFFFu) == 0) rgb = 0xFFFFFFu;   // never invisible black-on-black
            const float* o = reinterpret_cast<const float*>(p10);
            float col4[4] = { ((rgb >> 16) & 0xFF) / 255.0f,
                              ((rgb >> 8)  & 0xFF) / 255.0f,
                              ( rgb        & 0xFF) / 255.0f,
                              o ? o[3] : 1.0f };
            if (orig_glyphrun)
                orig_glyphrun(p1, p2, p3, p4, p5, p6, p7, p8, p9,
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(col4)), p11);
            return;
        }
    }
    if (orig_glyphrun)
        orig_glyphrun(p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11);
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

    // glyph_run_draw: hooked only to suppress the native input caret in emu mode
    // (see hook_glyphrun). Not fatal if missing -- the emulator still works.
    uintptr_t glyphAddr = resolveRva("glyph_run_draw", sig::glyph_run_draw_rva, sig::glyph_run_draw_verify);
    if (glyphAddr)
        createHookAt("glyph_run_draw", glyphAddr, (void*)&hook_glyphrun, reinterpret_cast<void**>(&orig_glyphrun));

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
