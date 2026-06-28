#include "vtparse.h"

#include <cstdio>
#include <cstring>

namespace {
// ANSI SGR 30..37 map directly to our colour indices 0..7; bright is +8.
inline uint8_t ansiFg(int code) { return static_cast<uint8_t>(code - 30); }

// RGB -> nearest ANSI-8 (bit0=red, bit1=green, bit2=blue; matches the ANSI order
// black,red,green,yellow,blue,magenta,cyan,white).
inline uint8_t rgbToAnsi8(int r, int g, int b) {
    return static_cast<uint8_t>((r > 127 ? 1 : 0) | (g > 127 ? 2 : 0) | (b > 127 ? 4 : 0));
}

// xterm-256 index -> ANSI colour. 0..15 standard; 16..231 6x6x6 cube; 232..255
// grayscale (kept white so dim text stays visible on the black CRT).
inline uint8_t xterm256ToAnsi8(int n) {
    if (n < 16) return static_cast<uint8_t>(n & 0x0f);
    if (n >= 232) return 7;
    n -= 16;
    int r = (n / 36) % 6, g = (n / 6) % 6, b = n % 6;
    return rgbToAnsi8(r * 51, g * 51, b * 51);
}

// Map a non-ASCII Unicode codepoint to a single ASCII cell (BO1 font is ASCII).
// One cell per codepoint keeps grid alignment intact.
inline char mapCodepoint(uint32_t cp) {
    if (cp < 0x80) return static_cast<char>(cp);
    if (cp >= 0x2500 && cp <= 0x257f) {        // box drawing
        switch (cp) {
        case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250a:
        case 0x250b: case 0x254e: case 0x254f: case 0x2551: return '|';
        case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508:
        case 0x2509: case 0x254c: case 0x254d: return '-';
        case 0x2550: return '=';
        default: return '+';                    // corners / tees / crosses
        }
    }
    if (cp >= 0x2580 && cp <= 0x259f) {        // block elements / shades
        if (cp == 0x2591) return '.';
        if (cp == 0x2592) return ':';
        return '#';
    }
    if (cp >= 0x2800 && cp <= 0x28ff) return '*';   // braille (spinners)
    switch (cp) {
    case 0x2022: return '*';                    // bullet
    case 0x2026: return '.';                    // ellipsis
    case 0x2018: case 0x2019: return '\'';      // curly single quotes
    case 0x201c: case 0x201d: return '"';       // curly double quotes
    case 0x2013: case 0x2014: return '-';       // en/em dash
    case 0x2190: return '<'; case 0x2192: return '>';
    case 0x2191: return '^'; case 0x2193: return 'v';
    case 0x00a0: return ' ';                    // nbsp
    }
    return '?';                                 // unknown -> placeholder
}
}  // namespace

namespace vt {

void Parser::feed(const char* data, int len) {
    for (int i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        switch (state_) {
        case Ground:
            if (c == 0x1b)            { u8len_ = 0; state_ = Esc; }
            else if (c < 0x80)        { u8len_ = 0; groundByte(c); }
            else if ((c & 0xC0) == 0x80) {            // UTF-8 continuation byte
                if (u8len_ > 0) {
                    u8cp_ = (u8cp_ << 6) | (c & 0x3f);
                    if (--u8len_ == 0) putCodepoint(u8cp_);
                }
            } else {                                   // UTF-8 lead byte
                if ((c & 0xE0) == 0xC0)      { u8cp_ = c & 0x1f; u8len_ = 1; }
                else if ((c & 0xF0) == 0xE0) { u8cp_ = c & 0x0f; u8len_ = 2; }
                else if ((c & 0xF8) == 0xF0) { u8cp_ = c & 0x07; u8len_ = 3; }
                else                          { u8len_ = 0; }
            }
            break;

        case Esc:
            switch (c) {
            case '[': state_ = Csi;
                params_[0] = 0; pcount_ = 0; digits_ = false; priv_ = false;
                break;
            case ']': state_ = Osc; break;            // OSC (e.g. window title)
            case '(': case ')': case '*': case '+':
                state_ = EscCharset; break;           // charset select: eat next byte
            case '7': scr_.saveCursor();    state_ = Ground; break;   // DECSC
            case '8': scr_.restoreCursor(); state_ = Ground; break;   // DECRC
            case 'M': scr_.reverseIndex();  state_ = Ground; break;   // RI
            default:
                state_ = Ground; break;               // =, >, etc: ignore
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
        // bytes >= 0x80 are handled as UTF-8 in feed(); other control: ignore
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
    case 'd': scr_.rowAbsolute(arg(0, 1) - 1);    break;            // VPA (1-based)
    case 'H': case 'f':
        scr_.gotoXY(arg(0, 1) - 1, arg(1, 1) - 1); break;          // CUP (1-based)
    case 'J': scr_.eraseInDisplay(arg(0, 0)); break;
    case 'K': scr_.eraseInLine(arg(0, 0));    break;
    case 'X': scr_.eraseChars(arg(0, 1));     break;               // ECH
    case '@': scr_.insertChars(arg(0, 1));    break;               // ICH
    case 'P': scr_.deleteChars(arg(0, 1));    break;               // DCH
    case 'L': scr_.insertLines(arg(0, 1));    break;               // IL
    case 'M': scr_.deleteLines(arg(0, 1));    break;               // DL
    case 's': scr_.saveCursor();    break;                          // save cursor
    case 'u': scr_.restoreCursor(); break;                          // restore cursor
    case 'n':                                                       // DSR
        if (arg(0, 0) == 6) {                                      // cursor position report
            char buf[32];
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                     scr_.curRow() + 1, scr_.curCol() + 1);
            reply(buf);
        }
        break;
    case 'c':                                                       // DA -> "VT100 with AVO"
        if (!priv_) reply("\x1b[?1;2c");
        break;
    case 'm': applySgr(); break;
    case 'r':                                                       // DECSTBM
        if (nargs >= 2) scr_.setScrollRegion(arg(0, 1) - 1, arg(1, 1) - 1);
        else            scr_.setScrollRegion(0, scr_.rows() - 1);
        break;
    case 'h': case 'l':
        // ?1049 / ?47 / ?1047 = alternate screen buffer (vim/less). Track the
        // flag (disables scroll-back) and clear; the app redraws its screen.
        if (priv_ && (arg(0, 0) == 1049 || arg(0, 0) == 47 || arg(0, 0) == 1047)) {
            scr_.setAltScreen(final == 'h');
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
        else if (p == 38 || p == 48) {
            // Extended colour: 38/48 ;5;n (256) or ;2;r;g;b (truecolour).
            // Foreground (38) is mapped to nearest ANSI; background (48) is
            // consumed but ignored (engine cell stores foreground only).
            bool fg = (p == 38);
            if (i + 2 < nargs && params_[i + 1] == 5) {
                if (fg) scr_.setColor(xterm256ToAnsi8(params_[i + 2]));
                i += 2;
            } else if (i + 4 < nargs && params_[i + 1] == 2) {
                if (fg) scr_.setColor(rgbToAnsi8(params_[i + 2], params_[i + 3], params_[i + 4]));
                i += 4;
            } else if (i + 1 < nargs) {
                i += 1;     // malformed; skip the subtype param
            }
        }
        // basic background (40..47, 49, 100..107) ignored: foreground only.
    }
}

void Parser::putCodepoint(uint32_t cp) {
    scr_.putChar(mapCodepoint(cp));
}

void Parser::reply(const char* s) {
    if (respond_ && s) respond_(s, static_cast<int>(strlen(s)));
}

}  // namespace vt
