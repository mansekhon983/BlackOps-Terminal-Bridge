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
#include <deque>
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
    void rowAbsolute(int row);          // CSI d (1-based row)
    void saveCursor();                  // DECSC / CSI s
    void restoreCursor();               // DECRC / CSI u

    // text
    void putChar(char c);               // at cursor; advances; wraps at right edge
    void carriageReturn();              // col = 0
    void lineFeed();                    // row+1; scroll within region at bottom
    void reverseIndex();                // ESC M: row-1; scroll down at region top
    void backspace();                   // col-1
    void tab();                         // next 8-col tab stop

    // erase / in-line edits
    void eraseInLine(int mode);         // 0=cur..eol  1=bol..cur  2=whole line
    void eraseInDisplay(int mode);      // 0=cur..end  1=start..cur  2=all (+home)
    void eraseChars(int n);             // CSI X: blank n cells at cursor, no move
    void insertChars(int n);            // CSI @: shift row right at cursor
    void deleteChars(int n);            // CSI P: shift row left at cursor
    void insertLines(int n);            // CSI L: open n blank lines at cursor (region)
    void deleteLines(int n);            // CSI M: remove n lines at cursor (region)

    // attributes
    void setColor(uint8_t c) { color_ = c; }
    void resetColor()        { color_ = kDefaultColor; }

    // scroll region (DECSTBM); defaults to the whole screen
    void setScrollRegion(int top, int bottom);
    void scrollUp(int n);
    void scrollDown(int n);

    // alternate screen (vim/less): no scroll-back while active
    void setAltScreen(bool on);
    bool altScreen() const { return altScreen_; }

    // scroll-back view (call under mtx). viewOffset 0 = live bottom.
    void scrollViewUp(int n);
    void scrollViewDown(int n);
    void scrollToLive() { viewOffset_ = 0; }
    int  viewOffset() const { return viewOffset_; }
    const Cell& viewAt(int vrow, int col) const;   // resolve a visible row via history

    std::mutex mtx;

private:
    void clearCell(int row, int col);
    void clampCursor();

    int cols_ = 80, rows_ = 20;
    int cx_ = 0, cy_ = 0;
    bool wrapPending_ = false;          // deferred wrap at right margin (DECAWM)
    int scrollTop_ = 0, scrollBot_ = 19;
    int scx_ = 0, scy_ = 0;             // saved cursor
    uint8_t color_ = kDefaultColor;
    uint8_t scolor_ = kDefaultColor;    // saved colour
    bool altScreen_ = false;
    int viewOffset_ = 0;
    std::vector<Cell> cells_;
    std::deque<std::vector<Cell>> history_;   // rows scrolled off the top
    static const size_t kMaxHistory = 1000;
};

}  // namespace grid
