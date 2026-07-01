// Replay a captured raw-pty log through the real grid + VT parser and dump the
// final screen. Deterministic offline reproduction of what the in-game emulator
// would display -- lets us debug rendering against real app output (Claude Code)
// with no inject cycle.
//   feed_log <file> [cols] [rows]

#include "../src/grid.h"
#include "../src/vtparse.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: feed_log <file> [cols] [rows] [display] [pgup] | feed_log <file> synth\n"); return 1; }
    bool synth = false;
    for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "synth")) synth = true;
    int cols    = (argc > 2 && atoi(argv[2])) ? atoi(argv[2]) : 80;
    int rows    = (argc > 3 && atoi(argv[3])) ? atoi(argv[3]) : 27;
    int display = (argc > 4) ? atoi(argv[4]) : 0;     // 0 = dump whole grid
    int pgup    = (argc > 5) ? atoi(argv[5]) : 0;     // scroll-back steps (display/2 each)

    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> data(n);
    fread(data.data(), 1, n, f);
    fclose(f);

    grid::Screen s; s.init(cols, rows);
    vt::Parser p(s);

    bool slow = false;
    for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "slow")) slow = true;
    auto feedAll = [&]() {
        if (slow) for (size_t i = 0; i < data.size(); ++i) p.feed(data.data() + i, 1);
        else      p.feed(data.data(), (int)data.size());
    };

    // synth mode: feed everything at once (like the live instant path) and dump the
    // unified 27-row windowed view (bottom-anchored over history++grid, same as live).
    // Optional 5th numeric arg = how many PgUp steps (display/2 each) to scroll first.
    if (synth) {
        feedAll();
        s.setDisplayRows(27);
        s.updateViewAnchor();
        int pgup = (argc > 4) ? atoi(argv[4]) : 0;
        for (int k = 0; k < pgup; ++k) s.scrollViewUp(27 / 2);
        printf("windowed view (1000-row grid): viewTop=%d viewOffset=%d cursorRow=%d history=%zu\n",
               s.viewTop(), s.viewOffset(), s.cursorVisibleRow(), s.historySize());
        printf("    +"); for (int c = 0; c < cols; ++c) putchar('-'); printf("+\n");
        for (int r = 0; r < 27; ++r) {
            printf("%2d  |", r);
            for (int c = 0; c < cols; ++c) {
                char ch = s.viewAt(r, c).ch;
                putchar((ch >= 32 && ch < 127) ? ch : ' ');
            }
            printf("|\n");
        }
        printf("    +"); for (int c = 0; c < cols; ++c) putchar('-'); printf("+\n");
        return 0;
    }

    feedAll();

    printf("replayed %ld bytes -> %dx%d  (cursor r%d c%d)\n", n, cols, rows, s.curRow(), s.curCol());
    printf("scroll-back: history=%zu  peak=%zu  cls-detector-clears=%d  altScreen=%d\n",
           s.historySize(), s.historyMax(), s.clearCount(), (int)s.altScreen());

    // No display window requested: dump the raw grid.
    if (display <= 0) {
        printf("    +"); for (int c = 0; c < cols; ++c) putchar('-'); printf("+\n");
        for (int r = 0; r < rows; ++r) {
            printf("%2d  |", r);
            for (int c = 0; c < cols; ++c) {
                char ch = s.at(r, c).ch;
                putchar((ch >= 32 && ch < 127) ? ch : ' ');
            }
            printf("|\n");
        }
        printf("    +"); for (int c = 0; c < cols; ++c) putchar('-'); printf("+\n");
        return 0;
    }

    // Show the windowed view (what the engine would blit), optionally scrolled up.
    s.setDisplayRows(display);
    s.updateViewAnchor();              // matches emuBlit (anchor to bottom content)
    for (int k = 0; k < pgup; ++k) s.scrollViewUp(display / 2);
    printf("windowed view: display=%d  viewTop=%d  viewOffset=%d  cursorRow=%d\n",
           display, s.viewTop(), s.viewOffset(), s.cursorVisibleRow());
    printf("    +"); for (int c = 0; c < cols; ++c) putchar('-'); printf("+\n");
    for (int r = 0; r < display; ++r) {
        printf("%2d  |", r);
        for (int c = 0; c < cols; ++c) {
            char ch = s.viewAt(r, c).ch;
            putchar((ch >= 32 && ch < 127) ? ch : ' ');
        }
        printf("|\n");
    }
    printf("    +"); for (int c = 0; c < cols; ++c) putchar('-'); printf("+\n");
    return 0;
}
