// Off-game unit checks for the grid model + VT parser (emulator E2). No game, no
// ConPTY -- just feed canned VT byte sequences and dump the resulting screen, so
// the parser logic can be iterated fast without an inject cycle.

#include "../src/grid.h"
#include "../src/vtparse.h"

#include <cstdio>
#include <cstring>
#include <string>

static const char* kColorName[16] = {
    "blk","red","grn","yel","blu","mag","cyn","wht",
    "BLK","RED","GRN","YEL","BLU","MAG","CYN","WHT"
};

static void dump(const char* title, grid::Screen& s) {
    printf("=== %s  (cursor r%d c%d) ===\n", title, s.curRow(), s.curCol());
    printf("    +");
    for (int c = 0; c < s.cols(); ++c) putchar('-');
    printf("+\n");
    for (int r = 0; r < s.rows(); ++r) {
        printf("%2d  |", r);
        for (int c = 0; c < s.cols(); ++c) putchar(s.at(r, c).ch);
        printf("|\n");
    }
    printf("    +");
    for (int c = 0; c < s.cols(); ++c) putchar('-');
    printf("+\n");
}

// Dump the per-cell colours of one row (to verify SGR handling).
static void dumpRowColors(const char* title, grid::Screen& s, int row) {
    printf("--- colours of row %d (%s) ---\n", row, title);
    uint8_t last = 255;
    for (int c = 0; c < s.cols(); ++c) {
        uint8_t col = s.at(row, c).color;
        char ch = s.at(row, c).ch;
        if (ch == ' ') continue;
        if (col != last) { printf(" [%s]", kColorName[col & 15]); last = col; }
        putchar(ch);
    }
    printf("\n");
}

static void feed(vt::Parser& p, const char* s) { p.feed(s, (int)strlen(s)); }

int main() {
    grid::Screen s;
    s.init(80, 20);
    vt::Parser p(s);

    // 1. plain text + newlines + CR
    feed(p, "hello world\r\nsecond line\r\nthird");
    dump("1: plain text + CRLF", s);

    // 2. absolute cursor positioning then overwrite
    feed(p, "\x1b[1;1HTOP-LEFT\x1b[5;10Hmid\x1b[20;70Hbr");
    dump("2: CUP positioning", s);

    // 3. erase: clear whole screen, then draw a box-ish thing with moves
    feed(p, "\x1b[2J");
    feed(p, "\x1b[3;5HABC\x1b[B\x1b[3Dxyz");      // down one, left 3, type
    dump("3: ED clear + relative moves", s);

    // 4. SGR colours on one line
    feed(p, "\x1b[2J\x1b[1;1H");
    feed(p, "\x1b[31mRED\x1b[32mGREEN\x1b[34mBLUE\x1b[0mwhite\x1b[1;33mBYEL");
    dump("4: SGR colours", s);
    dumpRowColors("SGR", s, 0);

    // 5. scrolling: write more lines than the screen has
    s.init(80, 6);
    vt::Parser p2(s);
    for (int i = 1; i <= 10; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "line %d\r\n", i);
        feed(p2, line);
    }
    dump("5: scroll (6-row screen, 10 lines)", s);

    // 6. scroll region (DECSTBM): lock top/bottom rows, scroll the middle
    s.init(80, 6);
    vt::Parser p3(s);
    feed(p3, "\x1b[1;1HHEADER");
    feed(p3, "\x1b[6;1HFOOTER");
    feed(p3, "\x1b[2;5r");                          // scroll region rows 2..5
    feed(p3, "\x1b[2;1H");
    for (int i = 1; i <= 8; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "body %d\r\n", i);
        feed(p3, line);
    }
    dump("6: scroll region keeps header/footer", s);

    printf("\n[vtparse_test] done\n");
    return 0;
}
