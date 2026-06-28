#include "grid.h"

#include <algorithm>

namespace grid {

void Screen::init(int cols, int rows) {
    cols_ = cols > 0 ? cols : 80;
    rows_ = rows > 0 ? rows : 20;
    cells_.assign(static_cast<size_t>(cols_) * rows_, Cell{});
    cx_ = cy_ = 0;
    scrollTop_ = 0;
    scrollBot_ = rows_ - 1;
    color_ = kDefaultColor;
}

void Screen::clearCell(int row, int col) {
    Cell& c = cells_[row * cols_ + col];
    c.ch = ' ';
    c.color = kDefaultColor;
}

void Screen::clampCursor() {
    cx_ = std::max(0, std::min(cx_, cols_ - 1));
    cy_ = std::max(0, std::min(cy_, rows_ - 1));
}

// ---- cursor -----------------------------------------------------------------
void Screen::gotoXY(int row, int col) { cy_ = row; cx_ = col; clampCursor(); }
void Screen::cursorUp(int n)    { cy_ -= std::max(1, n); clampCursor(); }
void Screen::cursorDown(int n)  { cy_ += std::max(1, n); clampCursor(); }
void Screen::cursorLeft(int n)  { cx_ -= std::max(1, n); clampCursor(); }
void Screen::cursorRight(int n) { cx_ += std::max(1, n); clampCursor(); }
void Screen::columnAbsolute(int col) { cx_ = col; clampCursor(); }

// ---- text -------------------------------------------------------------------
void Screen::putChar(char c) {
    if (cx_ >= cols_) {            // wrap
        cx_ = 0;
        lineFeed();
    }
    Cell& cell = cells_[cy_ * cols_ + cx_];
    cell.ch = c;
    cell.color = color_;
    ++cx_;
}

void Screen::carriageReturn() { cx_ = 0; }

void Screen::lineFeed() {
    if (cy_ >= scrollBot_) {
        scrollUp(1);              // at bottom of region -> scroll, cursor stays
    } else {
        ++cy_;
    }
    clampCursor();
}

void Screen::backspace() { if (cx_ > 0) --cx_; }

void Screen::tab() {
    int next = (cx_ / 8 + 1) * 8;
    cx_ = std::min(next, cols_ - 1);
}

// ---- erase ------------------------------------------------------------------
void Screen::eraseInLine(int mode) {
    int from = 0, to = cols_ - 1;
    if (mode == 0) from = cx_;            // cursor..eol
    else if (mode == 1) to = cx_;         // bol..cursor
    // mode 2: whole line
    for (int c = from; c <= to && c < cols_; ++c) clearCell(cy_, c);
}

void Screen::eraseInDisplay(int mode) {
    if (mode == 2) {                      // whole screen + home
        for (int i = 0; i < cols_ * rows_; ++i) { cells_[i].ch = ' '; cells_[i].color = kDefaultColor; }
        cx_ = cy_ = 0;
        return;
    }
    if (mode == 0) {                      // cursor..end
        eraseInLine(0);
        for (int r = cy_ + 1; r < rows_; ++r)
            for (int c = 0; c < cols_; ++c) clearCell(r, c);
    } else if (mode == 1) {               // start..cursor
        for (int r = 0; r < cy_; ++r)
            for (int c = 0; c < cols_; ++c) clearCell(r, c);
        eraseInLine(1);
    }
}

// ---- scroll -----------------------------------------------------------------
void Screen::setScrollRegion(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom > rows_ - 1) bottom = rows_ - 1;
    if (top >= bottom) { top = 0; bottom = rows_ - 1; }
    scrollTop_ = top;
    scrollBot_ = bottom;
    cx_ = 0; cy_ = scrollTop_;            // DECSTBM homes the cursor
}

void Screen::scrollUp(int n) {
    n = std::max(1, n);
    for (int k = 0; k < n; ++k) {
        for (int r = scrollTop_; r < scrollBot_; ++r)
            for (int c = 0; c < cols_; ++c)
                cells_[r * cols_ + c] = cells_[(r + 1) * cols_ + c];
        for (int c = 0; c < cols_; ++c) clearCell(scrollBot_, c);
    }
}

}  // namespace grid
