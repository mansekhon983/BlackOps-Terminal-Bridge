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

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: feed_log <file> [cols] [rows]\n"); return 1; }
    int cols = (argc > 2) ? atoi(argv[2]) : 80;
    int rows = (argc > 3) ? atoi(argv[3]) : 27;

    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> data(n);
    fread(data.data(), 1, n, f);
    fclose(f);

    grid::Screen s; s.init(cols, rows);
    vt::Parser p(s);
    p.feed(data.data(), (int)data.size());

    printf("replayed %ld bytes -> %dx%d  (cursor r%d c%d)\n", n, cols, rows, s.curRow(), s.curCol());
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
