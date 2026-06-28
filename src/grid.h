#pragma once
// Terminal screen model (emulator milestone E2). A cols x rows grid of cells,
// each {glyph, colour}, plus a cursor and the current SGR colour. The VT parser
// mutates it (reader thread); the frame hook reads a snapshot to blit into the
// engine grid (main thread). Hold `mtx` around a batch of ops / the blit.
//
// Colour is an ANSI index 0..7 (+ bright 8..15); mapping to the engine palette
// happens at blit time in hooks.cpp. kDefaultColor is "normal foreground".

#include <cstdint>
#include <vector>
#include <mutex>

namespace grid {

struct Cell {
    char    ch    = ' ';
    uint8_t color = 7;   // ANSI white = default foreground
};

class Screen {
public:
    static constexpr uint8_t kDefaultColor = 7;

    void init(int cols, int rows);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int curRow() const { return cy_; }
    int curCol() const { return cx_; }
    const Cell& at(int row, int col) const { return cells_[row * cols_ + col]; }

    // cursor
    void gotoXY(int row, int col);      // clamped
    void cursorUp(int n);
    void cursorDown(int n);
    void cursorLeft(int n);
    void cursorRight(int n);
    void columnAbsolute(int col);       // CSI G (1-based col)

    // text
    void putChar(char c);               // at cursor; advances; wraps at right edge
    void carriageReturn();              // col = 0
    void lineFeed();                    // row+1; scroll within region at bottom
    void backspace();                   // col-1
    void tab();                         // next 8-col tab stop

    // erase
    void eraseInLine(int mode);         // 0=cur..eol  1=bol..cur  2=whole line
    void eraseInDisplay(int mode);      // 0=cur..end  1=start..cur  2=all (+home)

    // attributes
    void setColor(uint8_t c) { color_ = c; }
    void resetColor()        { color_ = kDefaultColor; }

    // scroll region (DECSTBM); defaults to the whole screen
    void setScrollRegion(int top, int bottom);
    void scrollUp(int n);

    std::mutex mtx;

private:
    void clearCell(int row, int col);
    void clampCursor();

    int cols_ = 80, rows_ = 20;
    int cx_ = 0, cy_ = 0;
    int scrollTop_ = 0, scrollBot_ = 19;
    uint8_t color_ = kDefaultColor;
    std::vector<Cell> cells_;
};

}  // namespace grid
