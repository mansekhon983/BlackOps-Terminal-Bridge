#include "grid.h"

#include <algorithm>

namespace grid {

void Screen::init(int cols, int rows) {
    cols_ = cols > 0 ? cols : 80;
    rows_ = rows > 0 ? rows : 20;
    cells_.assign(static_cast<size_t>(cols_) * rows_, Cell{});
    cx_ = cy_ = 0;
    scx_ = scy_ = 0;
    wrapPending_ = false;
    scrollTop_ = 0;
    scrollBot_ = rows_ - 1;
    color_ = scolor_ = kDefaultColor;
    altScreen_ = false;
    viewOffset_ = 0;
    produced_ = 0;
    repaint_ = false;
    lastWasEL_ = false;
    clearStreak_ = 0;
    history_.clear();
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
// Any explicit cursor movement cancels a pending right-margin wrap.
void Screen::gotoXY(int row, int col) {
    if (row < cy_) repaint_ = true;     // absolute move upward = an app repainting
    clearStreak_ = 0; lastWasEL_ = false;
    cy_ = row; cx_ = col; wrapPending_ = false; clampCursor();
}
void Screen::cursorUp(int n)    { cy_ -= std::max(1, n); wrapPending_ = false; repaint_ = true; clampCursor(); }
void Screen::cursorDown(int n)  { cy_ += std::max(1, n); wrapPending_ = false; clampCursor(); }
void Screen::cursorLeft(int n)  { cx_ -= std::max(1, n); wrapPending_ = false; clampCursor(); }
void Screen::cursorRight(int n) { cx_ += std::max(1, n); wrapPending_ = false; clampCursor(); }
void Screen::columnAbsolute(int col) { cx_ = col; wrapPending_ = false; clampCursor(); }
void Screen::rowAbsolute(int row) {
    if (row < cy_) repaint_ = true;
    clearStreak_ = 0; lastWasEL_ = false;
    cy_ = row; wrapPending_ = false; clampCursor();
}
void Screen::saveCursor()    { scx_ = cx_; scy_ = cy_; scolor_ = color_; }
void Screen::restoreCursor() { cx_ = scx_; cy_ = scy_; color_ = scolor_; wrapPending_ = false; clampCursor(); }

// ---- text -------------------------------------------------------------------
void Screen::putChar(char c) {
    if (wrapPending_) {           // deferred wrap from the previous glyph
        cx_ = 0;
        lineFeed();               // (also clears wrapPending_)
    }
    Cell& cell = cells_[cy_ * cols_ + cx_];
    cell.ch = c;
    cell.color = color_;
    ++produced_;                                  // one revealed glyph (for the pump)
    lastWasEL_ = false; clearStreak_ = 0;         // real text breaks a cls run
    if (cx_ + 1 >= cols_) wrapPending_ = true;    // at right margin: defer the wrap
    else ++cx_;
}

void Screen::carriageReturn() { cx_ = 0; wrapPending_ = false; }

void Screen::lineFeed() {
    wrapPending_ = false;
    // cls detection: a long run of (erase-line, line-feed) with no text between
    // is cmd's clear. On a near-full-screen run, drop the scroll-back so a later
    // scroll up doesn't show the pre-clear output.
    if (lastWasEL_) {
        if (++clearStreak_ >= rows_ * 3 / 4 && !altScreen_) {
            if (!history_.empty()) ++clearCount_;     // diag
            history_.clear();
            viewOffset_ = 0;
            clearStreak_ = 0;
        }
    } else {
        clearStreak_ = 0;
    }
    lastWasEL_ = false;
    if (cy_ >= scrollBot_) {
        scrollUp(1);              // at bottom of region -> scroll, cursor stays
    } else {
        ++cy_;
    }
    clampCursor();
}

void Screen::reverseIndex() {
    repaint_ = true;
    if (cy_ <= scrollTop_) scrollDown(1);   // at region top -> scroll down
    else { --cy_; clampCursor(); }
}

void Screen::backspace() { if (cx_ > 0) --cx_; wrapPending_ = false; }

void Screen::tab() {
    int next = (cx_ / 8 + 1) * 8;
    cx_ = std::min(next, cols_ - 1);
    wrapPending_ = false;
}

// ---- erase ------------------------------------------------------------------
void Screen::eraseInLine(int mode) {
    int from = 0, to = cols_ - 1;
    if (mode == 0) from = cx_;            // cursor..eol
    else if (mode == 1) to = cx_;         // bol..cursor
    // mode 2: whole line
    for (int c = from; c <= to && c < cols_; ++c) clearCell(cy_, c);
    lastWasEL_ = true;                    // part of a possible cls run (see lineFeed)
}

void Screen::eraseInDisplay(int mode) {
    if (mode == 3) {                      // clear scroll-back only (xterm ED 3)
        history_.clear();
        viewOffset_ = 0;
        return;
    }
    if (mode == 2) {                      // whole screen + home
        for (int i = 0; i < cols_ * rows_; ++i) { cells_[i].ch = ' '; cells_[i].color = kDefaultColor; }
        cx_ = cy_ = 0;
        wrapPending_ = false;
        // `clear`/`cls` should also drop the scroll-back so a later scroll up
        // doesn't reveal pre-clear output (the normal-screen buffer only).
        if (!altScreen_) { history_.clear(); viewOffset_ = 0; }
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

// ---- in-line edits (CSI X / @ / P / L / M) ----------------------------------
void Screen::eraseChars(int n) {                 // CSI X: blank n at cursor
    n = std::max(1, n);
    for (int c = cx_; c < cx_ + n && c < cols_; ++c) clearCell(cy_, c);
}

void Screen::insertChars(int n) {                // CSI @: shift right at cursor
    n = std::max(1, n);
    for (int c = cols_ - 1; c >= cx_; --c) {
        int src = c - n;
        cells_[cy_ * cols_ + c] = (src >= cx_) ? cells_[cy_ * cols_ + src] : Cell{};
    }
}

void Screen::deleteChars(int n) {                // CSI P: shift left at cursor
    n = std::max(1, n);
    for (int c = cx_; c < cols_; ++c) {
        int src = c + n;
        cells_[cy_ * cols_ + c] = (src < cols_) ? cells_[cy_ * cols_ + src] : Cell{};
    }
}

void Screen::insertLines(int n) {                // CSI L: open n lines at cursor
    if (cy_ < scrollTop_ || cy_ > scrollBot_) return;
    n = std::max(1, n);
    for (int k = 0; k < n; ++k) {
        for (int r = scrollBot_; r > cy_; --r)
            for (int c = 0; c < cols_; ++c)
                cells_[r * cols_ + c] = cells_[(r - 1) * cols_ + c];
        for (int c = 0; c < cols_; ++c) clearCell(cy_, c);
    }
}

void Screen::deleteLines(int n) {                // CSI M: remove n lines at cursor
    if (cy_ < scrollTop_ || cy_ > scrollBot_) return;
    n = std::max(1, n);
    for (int k = 0; k < n; ++k) {
        for (int r = cy_; r < scrollBot_; ++r)
            for (int c = 0; c < cols_; ++c)
                cells_[r * cols_ + c] = cells_[(r + 1) * cols_ + c];
        for (int c = 0; c < cols_; ++c) clearCell(scrollBot_, c);
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
    wrapPending_ = false;
}

void Screen::scrollUp(int n) {
    n = std::max(1, n);
    // Full-screen scroll of the main screen feeds the scroll-back history.
    bool feedHistory = (scrollTop_ == 0 && !altScreen_);
    for (int k = 0; k < n; ++k) {
        if (feedHistory) {
            std::vector<Cell> row(cells_.begin(), cells_.begin() + cols_);
            history_.push_back(std::move(row));
            if (history_.size() > kMaxHistory) history_.pop_front();
            if (history_.size() > historyMax_) historyMax_ = history_.size();  // diag
        }
        for (int r = scrollTop_; r < scrollBot_; ++r)
            for (int c = 0; c < cols_; ++c)
                cells_[r * cols_ + c] = cells_[(r + 1) * cols_ + c];
        for (int c = 0; c < cols_; ++c) clearCell(scrollBot_, c);
    }
}

void Screen::scrollDown(int n) {
    n = std::max(1, n);
    for (int k = 0; k < n; ++k) {
        for (int r = scrollBot_; r > scrollTop_; --r)
            for (int c = 0; c < cols_; ++c)
                cells_[r * cols_ + c] = cells_[(r - 1) * cols_ + c];
        for (int c = 0; c < cols_; ++c) clearCell(scrollTop_, c);
    }
}

// ---- alternate screen + scroll-back view ------------------------------------
void Screen::setAltScreen(bool on) {
    altScreen_ = on;
    viewOffset_ = 0;            // never show scroll-back over an alt-screen app
}

void Screen::scrollViewUp(int n) {
    viewOffset_ += std::max(1, n);
    int maxOff = static_cast<int>(history_.size());
    if (viewOffset_ > maxOff) viewOffset_ = maxOff;
}

void Screen::scrollViewDown(int n) {
    viewOffset_ -= std::max(1, n);
    if (viewOffset_ < 0) viewOffset_ = 0;
}

// Resolve a visible row (0..rows_-1) through the scroll-back: the live view
// (offset 0) is the current grid; a positive offset slides the window up into
// history_. Combined buffer = history_ (old..new) then the live grid rows.
const Cell& Screen::viewAt(int vrow, int col) const {
    static const Cell blank{};
    int H = static_cast<int>(history_.size());
    int combined = H - viewOffset_ + vrow;     // index into history++grid
    if (combined < 0) return blank;
    if (combined < H) {
        const std::vector<Cell>& row = history_[combined];
        return (col < static_cast<int>(row.size())) ? row[col] : blank;
    }
    int gr = combined - H;
    if (gr >= rows_) return blank;
    return cells_[gr * cols_ + col];
}

}  // namespace grid
