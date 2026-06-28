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

    // 7. redraw ops: save/restore cursor + erase/insert/delete chars
    s.init(40, 4);
    vt::Parser p4(s);
    feed(p4, "\x1b[1;1HABCDEFGH");          // row0: ABCDEFGH
    feed(p4, "\x1b[1;3H");                   // cursor to col3 (C)
    feed(p4, "\x1b[2P");                     // DCH 2 -> delete CD -> "ABEFGH"
    feed(p4, "\x1b[1;1H\x1b""7");            // home, save cursor (ESC 7)
    feed(p4, "\x1b[3;1Hmoved");              // write on row2
    feed(p4, "\x1b""8X");                    // restore cursor (ESC 8), write X at home
    feed(p4, "\x1b[2;1HIJKL\x1b[2;2H\x1b[2@"); // row1 IJKL, ICH 2 at col2 -> "I  JKL"
    dump("7: DCH / DECSC-DECRC / ICH", s);

    // 8. extended colour: 256-index and truecolour -> nearest ANSI
    s.init(40, 2);
    vt::Parser p5(s);
    feed(p5, "\x1b[38;5;196mRR\x1b[38;5;46mGG\x1b[38;2;0;0;255mBB\x1b[0mWW");
    dumpRowColors("256/truecolour", s, 0);

    // 9. UTF-8 box-drawing -> ASCII (┌─┐ / │ / └─┘)
    s.init(40, 3);
    vt::Parser p6(s);
    feed(p6, "\x1b[1;1H\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90");   // ┌─┐
    feed(p6, "\x1b[2;1H\xe2\x94\x82" "x" "\xe2\x94\x82");         // │x│
    feed(p6, "\x1b[3;1H\xe2\x94\x94\xe2\x94\x80\xe2\x94\x98");   // └─┘
    dump("9: UTF-8 box-drawing -> ASCII", s);

    printf("\n[vtparse_test] done\n");
    return 0;
}
