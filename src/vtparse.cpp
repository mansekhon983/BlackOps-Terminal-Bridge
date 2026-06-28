#include "vtparse.h"

namespace {
// ANSI SGR 30..37 map directly to our colour indices 0..7; bright is +8.
inline uint8_t ansiFg(int code) { return static_cast<uint8_t>(code - 30); }
}  // namespace

namespace vt {

void Parser::feed(const char* data, int len) {
    for (int i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        switch (state_) {
        case Ground:
            if (c == 0x1b) state_ = Esc;
            else groundByte(c);
            break;

        case Esc:
            switch (c) {
            case '[': state_ = Csi;
                params_[0] = 0; pcount_ = 0; digits_ = false; priv_ = false;
                break;
            case ']': state_ = Osc; break;            // OSC (e.g. window title)
            case '(': case ')': case '*': case '+':
                state_ = EscCharset; break;           // charset select: eat next byte
            case 'M':                                  // reverse index: scroll down at top
                // approximate: move cursor up (vim/less rarely depend on full RI)
                scr_.cursorUp(1);
                state_ = Ground; break;
            default:
                state_ = Ground; break;               // =, >, 7, 8, etc: ignore
            }
            break;

        case EscCharset:
            state_ = Ground;                           // consume the charset id byte
            break;

        case Csi:
            if (c == '?') { priv_ = true; break; }
            if (c >= '0' && c <= '9') {
                params_[pcount_] = params_[pcount_] * 10 + (c - '0');
                digits_ = true;
                break;
            }
            if (c == ';') {
                if (pcount_ < 15) { ++pcount_; params_[pcount_] = 0; }
                digits_ = true;
                break;
            }
            if (c >= 0x40 && c <= 0x7e) {              // final byte
                dispatchCsi(static_cast<char>(c));
                state_ = Ground;
                break;
            }
            // intermediate bytes (0x20..0x2f): ignore, stay in CSI
            break;

        case Osc:
            if (c == 0x07) state_ = Ground;            // BEL terminates OSC
            else if (c == 0x1b) state_ = OscEsc;       // maybe ST (ESC \)
            break;
        case OscEsc:
            state_ = (c == '\\') ? Ground : Osc;       // ESC \ = ST
            break;
        }
    }
}

void Parser::groundByte(unsigned char c) {
    switch (c) {
    case 0x07: break;                  // BEL
    case 0x08: scr_.backspace(); break;
    case 0x09: scr_.tab(); break;
    case 0x0a: scr_.lineFeed(); break; // LF
    case 0x0b: case 0x0c: scr_.lineFeed(); break;  // VT/FF ~ LF
    case 0x0d: scr_.carriageReturn(); break;       // CR
    default:
        if (c >= 0x20 && c < 0x7f) scr_.putChar(static_cast<char>(c));
        // other control / high bytes: ignore (no UTF-8 decode yet)
        break;
    }
}

void Parser::dispatchCsi(char final) {
    int nargs = (digits_ || pcount_ > 0) ? pcount_ + 1 : 0;
    auto arg = [&](int i, int def) -> int {
        if (i >= nargs) return def;
        return params_[i] != 0 ? params_[i] : def;
    };

    switch (final) {
    case 'A': scr_.cursorUp(arg(0, 1));    break;
    case 'B': scr_.cursorDown(arg(0, 1));  break;
    case 'C': scr_.cursorRight(arg(0, 1)); break;
    case 'D': scr_.cursorLeft(arg(0, 1));  break;
    case 'G': scr_.columnAbsolute(arg(0, 1) - 1); break;            // CHA (1-based)
    case 'H': case 'f':
        scr_.gotoXY(arg(0, 1) - 1, arg(1, 1) - 1); break;          // CUP (1-based)
    case 'J': scr_.eraseInDisplay(arg(0, 0)); break;
    case 'K': scr_.eraseInLine(arg(0, 0));    break;
    case 'm': applySgr(); break;
    case 'r':                                                       // DECSTBM
        if (nargs >= 2) scr_.setScrollRegion(arg(0, 1) - 1, arg(1, 1) - 1);
        else            scr_.setScrollRegion(0, scr_.rows() - 1);
        break;
    case 'h': case 'l':
        // Private modes. ?1049 / ?47 = alternate screen buffer (vim/less): on
        // enter OR exit we just clear -- the app redraws; we don't preserve the
        // shell's previous screen for E2.
        if (priv_ && (arg(0, 0) == 1049 || arg(0, 0) == 47 || arg(0, 0) == 1047)) {
            scr_.eraseInDisplay(2);
        }
        break;
    default:
        break;  // unhandled CSI: ignore
    }
}

void Parser::applySgr() {
    int nargs = (digits_ || pcount_ > 0) ? pcount_ + 1 : 0;
    if (nargs == 0) { scr_.resetColor(); return; }   // bare CSI m = reset

    bool bright = false;
    for (int i = 0; i < nargs; ++i) {
        int p = params_[i];
        if (p == 0)                 { scr_.resetColor(); bright = false; }
        else if (p == 1)            { bright = true; }
        else if (p == 22)           { bright = false; }
        else if (p == 39)           { scr_.resetColor(); }
        else if (p >= 30 && p <= 37){ scr_.setColor(ansiFg(p) + (bright ? 8 : 0)); }
        else if (p >= 90 && p <= 97){ scr_.setColor(static_cast<uint8_t>(p - 90 + 8)); }
        else if (p == 38) {
            // 38;5;n (256) or 38;2;r;g;b (truecolor): consume the extra params,
            // map coarsely to the 8-colour set later; for now skip to default.
            if (i + 1 < nargs && params_[i + 1] == 5) i += 2;
            else if (i + 1 < nargs && params_[i + 1] == 2) i += 4;
        }
        // background (40..47, 49, 100..107) ignored: we render foreground only.
    }
}

}  // namespace vt
