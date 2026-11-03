#pragma once

#include <cstdint>
#include <string>

namespace rfbvice {

// Translates an X11 keysym (as sent by VNC clients) to a vice plugin API key
// code: positive values are Unicode code points of character keys, negative
// values are negated Windows virtual key codes of non-character keys. Returns
// 0 for keysyms that should be ignored.
//
// If commandAsControl is true, the Meta and Super keysyms (sent for the Mac
// Command key by most Mac VNC viewers) are mapped to Control so that
// Command-C/V/X/A/F behave as Mac users expect in the remote browser.
int keysymToViceKey(uint32_t keysym, bool commandAsControl);

// Converts ISO-8859-1 (the encoding of RFB cut text) to UTF-8 and back. The
// reverse conversion replaces characters outside Latin-1 with '?'.
std::string latin1ToUTF8(const char* data, size_t length);
std::string utf8ToLatin1(const std::string& utf8);

}
