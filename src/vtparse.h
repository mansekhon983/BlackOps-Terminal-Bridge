#pragma once
// Minimal xterm-subset VT parser (emulator milestone E2). Consumes the ConPTY
// byte stream and drives a grid::Screen. Covers what vim/htop/less/python/bash
// line-editing need: printable text; CR/LF/BS/TAB/BEL; CSI cursor moves, erase,
// SGR colour, scroll region; alt-screen enter/exit; OSC (title) swallowed.
// Unknown sequences are consumed and ignored -- never throws, never crashes.
//
// feed() mutates the screen; call it holding the screen's mutex.

#include "grid.h"
#include <cstdint>

namespace vt {

class Parser {
public:
    explicit Parser(grid::Screen& screen) : scr_(screen) {}
    void feed(const char* data, int len);

private:
    void groundByte(unsigned char c);
    void dispatchCsi(char final);
    void applySgr();

    grid::Screen& scr_;

    enum State { Ground, Esc, EscCharset, Csi, Osc, OscEsc } state_ = Ground;

    // CSI parameter accumulation
    int  params_[16] = {0};
    int  pcount_ = 0;      // index of the current param slot
    bool digits_ = false;  // any digit/separator seen this sequence
    bool priv_   = false;  // '?' private-mode marker
};

}  // namespace vt
