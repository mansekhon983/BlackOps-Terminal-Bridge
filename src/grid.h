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

    // Monotonic count of glyph cells written via putChar. The frame pump meters
    // the pty stream against this so output reveals at the native ~1 char/ms rate
    // (left-to-right, top-to-bottom) instead of appearing all at once.
    uint64_t produced() const { return produced_; }

    // Set whenever the stream moves the cursor *up* (CUU / reverse-index) -- the
    // hallmark of an app repainting in place (vim, htop, Claude Code) as opposed
    // to plain append output (dir, cat). The pump uses it to switch a TUI to
    // instant rendering instead of typewriter-throttling it.
    bool repaint() const { return repaint_; }
    void clearRepaint()  { repaint_ = false; }
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

    // diagnostics (offline replay): scroll-back depth + how many times the cls
    // detector wiped history.
    size_t historySize() const { return history_.size(); }
    int    clearCount()  const { return clearCount_; }
    size_t historyMax()  const { return historyMax_; }
    const std::deque<std::vector<Cell>>& historyBuf() const { return history_; }

    // Tall viewport: the engine only shows `displayRows` rows, but the grid (and
    // PTY) can be taller. The visible window is `displayRows` consecutive rows of
    // the combined (history ++ grid) buffer, anchored to keep the cursor visible
    // (live) or slid up by viewOffset. Lets PgUp page through a repaint-style TUI
    // (Claude) whose layout is taller than the 27 visible rows. Default = rows().
    void setDisplayRows(int d) { displayRows_ = (d > 0 && d <= rows_) ? d : rows_; }
    int  displayRows() const { return displayRows_; }
    // Rebuild the compacted view sequence (call once per blit, under mtx). The display
    // is a window over `history_ ++ grid` with long runs of blank rows collapsed to a
    // single blank, so a repaint TUI (Claude) that pins its input far below its content
    // (leaving a huge blank gap) shows content-tail + input together with no gap, and
    // cmd (no gaps) is unaffected. Anchors to the bottom when content overflows.
    void updateViewAnchor();              // == buildView(); name kept for call sites
    int  viewTop() const { return viewTop_; }  // top index into the compacted sequence
    int  viewSeqLen() const { return static_cast<int>(viewRows_.size()); }  // compacted length
    int  viewLiveTop() const { return liveTop_; }   // live (bottom-anchored) top index
    int  cursorVisibleRow() const;        // cursor's display row, or -1 if off-window

    // Whether a repaint TUI is currently active (set each frame by the host from the
    // instant-mode signal). Gates the ED2 scroll-back clear: a clear is a real `cls`
    // only when NOT in a TUI.
    void setTuiActive(bool b) { tuiActive_ = b; }

    // scroll-back view (call under mtx). viewOffset 0 = live bottom.
    void scrollViewUp(int n);
    void scrollViewDown(int n);
    void scrollToLive() { viewOffset_ = 0; }
    int  viewOffset() const { return viewOffset_; }
    const Cell& viewAt(int dRow, int col) const;   // resolve a visible display row

    std::mutex mtx;

private:
    void clearCell(int row, int col);
    void clampCursor();

    int cols_ = 80, rows_ = 20;
    int displayRows_ = 20;             // visible window height (<= rows_); see setDisplayRows
    int cx_ = 0, cy_ = 0;
    bool wrapPending_ = false;          // deferred wrap at right margin (DECAWM)
    int scrollTop_ = 0, scrollBot_ = 19;
    int scx_ = 0, scy_ = 0;             // saved cursor
    uint8_t color_ = kDefaultColor;
    uint8_t scolor_ = kDefaultColor;    // saved colour
    bool altScreen_ = false;
    int viewOffset_ = 0;
    uint64_t produced_ = 0;             // glyph cells written (see produced())
    bool repaint_ = false;             // cursor moved up since last clearRepaint()
    int  clearCount_ = 0;              // diag: times a clear dropped history
    size_t historyMax_ = 0;           // diag: peak history depth
    std::vector<Cell> cells_;
    std::deque<std::vector<Cell>> history_;   // rows scrolled off the top (cmd path)
    static const size_t kMaxHistory = 8000;

    // Compacted view sequence, rebuilt each blit by updateViewAnchor()/buildView().
    // Each entry points to a cols_-wide row (a grid row, a history row, or blankRow_).
    std::vector<const Cell*> viewRows_;
    std::vector<Cell> blankRow_;              // one blank row, referenced for gap fillers
    int cursorSeqPos_ = -1;                   // cursor's index in viewRows_ (-1 = collapsed)
    int viewTop_ = 0;                         // top compacted-seq index of the display
    int liveTop_ = 0;                         // compacted-seq index of the live (bottom) top

    bool tuiActive_ = false;                  // repaint TUI active (gates the ED2 clear)
};

}  // namespace grid
