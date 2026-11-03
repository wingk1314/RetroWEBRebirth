#include "keys.hpp"

namespace rfbvice {

namespace {

// Negated Windows virtual key codes, matching VicePluginAPI_Key.
constexpr int Backspace = -8;
constexpr int Tab = -9;
constexpr int Enter = -13;
constexpr int Shift = -16;
constexpr int Control = -17;
constexpr int Alt = -18;
constexpr int CapsLock = -20;
constexpr int Esc = -27;
constexpr int PageUp = -33;
constexpr int PageDown = -34;
constexpr int End = -35;
constexpr int Home = -36;
constexpr int Left = -37;
constexpr int Up = -38;
constexpr int Right = -39;
constexpr int Down = -40;
constexpr int Insert = -45;
constexpr int Delete = -46;
constexpr int Win = -91;
constexpr int Menu = -93;
constexpr int F1 = -112;
constexpr int NumLock = -144;

}

int keysymToViceKey(uint32_t keysym, bool commandAsControl) {
    // Printable Latin-1: keysym equals the code point.
    if((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF)) {
        return (int)keysym;
    }
    // Direct Unicode keysyms.
    if(keysym >= 0x01000100 && keysym <= 0x0110FFFF) {
        uint32_t cp = keysym - 0x01000000;
        if(cp >= 0xD800 && cp <= 0xDFFF) {
            return 0;
        }
        return (int)cp;
    }

    switch(keysym) {
        case 0xFF08: return Backspace;
        case 0xFF09: return Tab;
        case 0xFE20: return Tab;        // ISO_Left_Tab (Shift-Tab)
        case 0xFF0D: return Enter;
        case 0xFF8D: return Enter;      // KP_Enter
        case 0xFF1B: return Esc;
        case 0xFFE1: case 0xFFE2: return Shift;
        case 0xFFE3: case 0xFFE4: return Control;
        case 0xFFE9: case 0xFFEA: return Alt;
        case 0xFF7E: return Alt;        // Mode_switch (Option on some viewers)
        case 0xFFE7: case 0xFFE8:       // Meta_L / Meta_R
        case 0xFFEB: case 0xFFEC:       // Super_L / Super_R
            return commandAsControl ? Control : Win;
        case 0xFFE5: return CapsLock;
        case 0xFF55: case 0xFF9A: return PageUp;
        case 0xFF56: case 0xFF9B: return PageDown;
        case 0xFF57: case 0xFF9C: return End;
        case 0xFF50: case 0xFF95: return Home;
        case 0xFF51: case 0xFF96: return Left;
        case 0xFF52: case 0xFF97: return Up;
        case 0xFF53: case 0xFF98: return Right;
        case 0xFF54: case 0xFF99: return Down;
        case 0xFF63: case 0xFF9E: return Insert;
        case 0xFFFF: case 0xFF9F: return Delete;
        case 0xFF67: return Menu;
        case 0xFF7F: return NumLock;
        // Keypad characters.
        case 0xFF80: return ' ';
        case 0xFFAA: return '*';
        case 0xFFAB: return '+';
        case 0xFFAC: return ',';
        case 0xFFAD: return '-';
        case 0xFFAE: return '.';
        case 0xFFAF: return '/';
        case 0xFFBD: return '=';
        default: break;
    }
    if(keysym >= 0xFFB0 && keysym <= 0xFFB9) {
        return '0' + (int)(keysym - 0xFFB0);
    }
    if(keysym >= 0xFFBE && keysym <= 0xFFC9) {
        return F1 - (int)(keysym - 0xFFBE);
    }
    return 0;
}

std::string latin1ToUTF8(const char* data, size_t length) {
    std::string out;
    out.reserve(length);
    for(size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        if(c == 0) {
            continue;
        }
        if(c < 0x80) {
            out.push_back((char)c);
        } else {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::string utf8ToLatin1(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while(i < utf8.size()) {
        unsigned char c = (unsigned char)utf8[i];
        uint32_t cp;
        size_t len;
        if(c < 0x80) {
            cp = c; len = 1;
        } else if((c & 0xE0) == 0xC0) {
            cp = c & 0x1F; len = 2;
        } else if((c & 0xF0) == 0xE0) {
            cp = c & 0x0F; len = 3;
        } else if((c & 0xF8) == 0xF0) {
            cp = c & 0x07; len = 4;
        } else {
            out.push_back('?');
            ++i;
            continue;
        }
        if(i + len > utf8.size()) {
            out.push_back('?');
            break;
        }
        bool valid = true;
        for(size_t j = 1; j < len; ++j) {
            unsigned char cc = (unsigned char)utf8[i + j];
            if((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if(!valid) {
            out.push_back('?');
            ++i;
            continue;
        }
        if(cp == 0) {
            // skip
        } else if(cp <= 0xFF) {
            out.push_back((char)cp);
        } else {
            out.push_back('?');
        }
        i += len;
    }
    return out;
}

}
