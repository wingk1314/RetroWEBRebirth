// rfbvice: a Browservice vice plugin that serves the browser view over the
// VNC (RFB) protocol using LibVNCServer.
//
// Compared to retrojsvice (which sends a full PNG/JPEG frame per HTTP request),
// rfbvice keeps one TCP connection per client and sends only what changed:
//
//   - each new frame is diffed against what the clients already have, in
//     32x32 tiles, and only changed tiles are sent;
//   - vertical scrolling is detected (row hashing) and sent as a CopyRect, so
//     a scroll costs one copy command plus the newly revealed strip;
//   - encoding (Tight with JPEG, ZRLE, Hextile...) is negotiated with the
//     client, and the client asks for updates at its own pace, which gives
//     natural flow control on slow machines;
//   - mouse cursor shapes are sent as cursor updates instead of being encoded
//     in the image size.
//
// Threading model:
//
//   - The program thread calls the vice plugin API functions and
//     vicePluginAPI_pumpEvents, and is the only thread that calls program
//     callbacks (except eventNotify).
//   - One background "VNC thread" runs the LibVNCServer event loop. It owns
//     the framebuffer: all diffing, CopyRect scheduling and framebuffer writes
//     happen there, so no encoder ever reads a half-written frame.
//   - The two threads exchange data through a mutex-protected mailbox: frames
//     and cursor/clipboard updates flow to the VNC thread, input events and
//     client clipboard text flow to the program thread (followed by
//     eventNotify).

#include "frame_diff.hpp"
#include "keys.hpp"

#include "../../../vice_plugin_api.h"

#include <rfb/rfb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define API_EXPORT __attribute__((visibility("default")))

namespace {

using namespace rfbvice;
using std::string;
using std::vector;
using Clock = std::chrono::steady_clock;

const char* RfbviceVersion = "0.1.0";
const uint64_t APIVersion = 2000000;

/*************
 * Logging *
 *************/

std::mutex logMutex;
std::function<void(VicePluginAPI_LogLevel, const char*, const char*)> logCallback;
std::function<void(const char*, const char*)> panicCallback;

void logMsg(VicePluginAPI_LogLevel level, const char* location, const string& msg) {
    std::function<void(VicePluginAPI_LogLevel, const char*, const char*)> cb;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        cb = logCallback;
    }
    if(cb) {
        cb(level, location, msg.c_str());
    } else {
        const char* levelStr =
            level == VICE_PLUGIN_API_LOG_LEVEL_ERROR ? "ERROR" :
            level == VICE_PLUGIN_API_LOG_LEVEL_WARNING ? "WARNING" : "INFO";
        fprintf(stderr, "[rfbvice %s %s] %s\n", levelStr, location, msg.c_str());
    }
}

[[noreturn]] void panicMsg(const char* location, const string& msg) {
    std::function<void(const char*, const char*)> cb;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        cb = panicCallback;
    }
    if(cb) {
        cb(location, msg.c_str());
    }
    fprintf(stderr, "[rfbvice PANIC %s] %s\n", location, msg.c_str());
    abort();
}

#define STR2(x) #x
#define STR(x) STR2(x)
#define LOCATION (__FILE__ ":" STR(__LINE__))
#define LOG_INFO(msg) logMsg(VICE_PLUGIN_API_LOG_LEVEL_INFO, LOCATION, (msg))
#define LOG_WARNING(msg) logMsg(VICE_PLUGIN_API_LOG_LEVEL_WARNING, LOCATION, (msg))
#define LOG_ERROR(msg) logMsg(VICE_PLUGIN_API_LOG_LEVEL_ERROR, LOCATION, (msg))
#define REQUIRE(cond) \
    do { if(!(cond)) panicMsg(LOCATION, "Requirement '" #cond "' failed"); } while(0)

void libvncLog(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    string msg = buf;
    while(!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
        msg.pop_back();
    }
    if(!msg.empty()) {
        logMsg(VICE_PLUGIN_API_LOG_LEVEL_INFO, "libvncserver", "LibVNCServer: " + msg);
    }
}

char* createMallocString(const string& str) {
    char* ret = (char*)malloc(str.size() + 1);
    REQUIRE(ret != nullptr);
    memcpy(ret, str.c_str(), str.size() + 1);
    return ret;
}

/*************
 * Options *
 *************/

struct Options {
    string listenIP = "127.0.0.1";
    int listenPort = 5900;
    int width = 1024;
    int height = 768;
    string password;
    bool scrollDetection = true;
    int wheelStep = 120;
    bool commandAsControl = true;
};

const vector<std::array<const char*, 4>> optionDocs = {{
    {"vnc-listen-addr", "IP:PORT", "bind address and port for the VNC server", "default: 127.0.0.1:5900"},
    {"screen-size", "WIDTHxHEIGHT", "size of the VNC screen (set it to the client's screen size, or a bit smaller to leave room for the viewer's title bar)", "default: 1024x768"},
    {"vnc-password", "PASSWORD", "require this VNC password (classic VNC authentication uses at most 8 characters; it is not encryption, so use it only on trusted networks)", "default: no password"},
    {"scroll-detection", "YES/NO", "detect vertical scrolling and send it as a cheap CopyRect instead of new pixels", "default: yes"},
    {"wheel-step", "N", "mouse wheel delta sent to the browser per wheel notch (120 is one notch in most browsers)", "default: 120"},
    {"command-key-as-control", "YES/NO", "map the Mac Command key (sent as Meta/Super by VNC viewers) to Control, so that Command-C/V/X/A work in the browser", "default: yes"},
}};

bool parseYesNo(const string& value, bool& out) {
    string v = value;
    for(char& c : v) c = (char)tolower((unsigned char)c);
    if(v == "yes" || v == "true" || v == "1" || v == "on") { out = true; return true; }
    if(v == "no" || v == "false" || v == "0" || v == "off") { out = false; return true; }
    return false;
}

bool parseInt(const string& value, int minVal, int maxVal, int& out) {
    if(value.empty() || value.size() > 9) return false;
    for(char c : value) if(c < '0' || c > '9') return false;
    int v = atoi(value.c_str());
    if(v < minVal || v > maxVal) return false;
    out = v;
    return true;
}

string parseOptions(const vector<std::pair<string, string>>& opts, Options& out) {
    for(const auto& kv : opts) {
        const string& name = kv.first;
        const string& value = kv.second;
        if(name == "vnc-listen-addr") {
            size_t colon = value.rfind(':');
            in_addr_t addr;
            if(
                colon == string::npos ||
                !parseInt(value.substr(colon + 1), 1, 65535, out.listenPort) ||
                !rfbStringToAddr((char*)value.substr(0, colon).c_str(), &addr)
            ) {
                return "Invalid value '" + value + "' for option vnc-listen-addr (expected IP:PORT)";
            }
            out.listenIP = value.substr(0, colon);
        } else if(name == "screen-size") {
            size_t x = value.find('x');
            if(
                x == string::npos ||
                !parseInt(value.substr(0, x), 64, 4096, out.width) ||
                !parseInt(value.substr(x + 1), 64, 4096, out.height)
            ) {
                return "Invalid value '" + value + "' for option screen-size (expected e.g. 1024x768)";
            }
        } else if(name == "vnc-password") {
            if(value.size() > 8) {
                LOG_WARNING("VNC passwords are limited to 8 characters; only the first 8 are used");
            }
            out.password = value;
        } else if(name == "scroll-detection") {
            if(!parseYesNo(value, out.scrollDetection)) {
                return "Invalid value '" + value + "' for option scroll-detection (expected yes or no)";
            }
        } else if(name == "wheel-step") {
            if(!parseInt(value, 1, 1000, out.wheelStep)) {
                return "Invalid value '" + value + "' for option wheel-step (expected 1..1000)";
            }
        } else if(name == "command-key-as-control") {
            if(!parseYesNo(value, out.commandAsControl)) {
                return "Invalid value '" + value + "' for option command-key-as-control (expected yes or no)";
            }
        } else {
            return "Unknown option '" + name + "' (rfbvice options: vnc-listen-addr, screen-size, "
                "vnc-password, scroll-detection, wheel-step, command-key-as-control)";
        }
    }
    return "";
}

/*************
 * Cursors *
 *************/

// '#' = black, '.' = white, ' ' = transparent.
struct CursorArt {
    int width, height, hotX, hotY;
    vector<const char*> rows;
};

const CursorArt arrowArt = {12, 19, 0, 0, {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#......#####",
    "#...#..#    ",
    "#..# #..#   ",
    "#.#  #..#   ",
    "##    #..#  ",
    "#     #..#  ",
    "       #..# ",
    "        ##  ",
}};

const CursorArt handArt = {16, 17, 5, 0, {
    "     ##         ",
    "    #..#        ",
    "    #..#        ",
    "    #..#        ",
    "    #..###      ",
    "    #..#..###   ",
    "    #..#..#..## ",
    " ## #..#..#..#.#",
    "#..##........#.#",
    "#...#..........#",
    " #.............#",
    "  #............#",
    "  #...........# ",
    "   #..........# ",
    "    #........#  ",
    "    #........#  ",
    "    ##########  ",
}};

const CursorArt textArt = {9, 18, 4, 9, {
    ".... ....",
    ".###.###.",
    " ...#... ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    "   .#.   ",
    " ...#... ",
    ".###.###.",
    ".... ....",
}};

rfbCursorPtr makeCursor(const CursorArt& art) {
    string source, mask;
    for(const char* row : art.rows) {
        for(int x = 0; x < art.width; ++x) {
            char c = row[x];
            source.push_back(c == '#' ? 'x' : ' ');
            mask.push_back(c == ' ' ? ' ' : 'x');
        }
    }
    rfbCursorPtr cursor = rfbMakeXCursor(
        art.width, art.height, (char*)source.c_str(), (char*)mask.c_str()
    );
    REQUIRE(cursor != nullptr);
    cursor->xhot = (unsigned short)art.hotX;
    cursor->yhot = (unsigned short)art.hotY;
    cursor->foreRed = cursor->foreGreen = cursor->foreBlue = 0;
    cursor->backRed = cursor->backGreen = cursor->backBlue = 0xFFFF;
    return cursor;
}

/*************
 * Context *
 *************/

struct InputEvent {
    enum Type { Pointer, Key } type;
    int buttonMask;
    int x, y;
    bool down;
    uint32_t keysym;
    Clock::time_point time;
};

class Context {
public:
    explicit Context(Options options, string programName)
        : opt_(std::move(options)), programName_(std::move(programName))
    {}

    ~Context() {
        stopVNCThread_();
        if(screen_ != nullptr) {
            rfbShutdownServer(screen_, TRUE);
            screen_->cursor = nullptr;
            char* fb = screen_->frameBuffer;
            rfbScreenCleanup(screen_);
            free(fb);
            screen_ = nullptr;
        }
        for(rfbCursorPtr& c : cursors_) {
            if(c != nullptr) {
                rfbFreeCursor(c);
                c = nullptr;
            }
        }
        for(char*& p : passwords_) {
            free(p);
            p = nullptr;
        }
    }

    // Creates the VNC server and starts listening. Returns an error message on
    // failure.
    string init() {
        int argc = 1;
        char arg0[] = "rfbvice";
        char* argv[] = {arg0, nullptr};
        screen_ = rfbGetScreen(&argc, argv, opt_.width, opt_.height, 8, 3, 4);
        if(screen_ == nullptr) {
            return "Creating the VNC screen failed";
        }

        // Browservice delivers B, G, R, X bytes; describe that layout so rows
        // can be copied without conversion.
        screen_->serverFormat.redShift = 16;
        screen_->serverFormat.greenShift = 8;
        screen_->serverFormat.blueShift = 0;

        size_t pixelCount = (size_t)opt_.width * (size_t)opt_.height;
        screen_->frameBuffer = (char*)malloc(pixelCount * 4);
        if(screen_->frameBuffer == nullptr) {
            return "Allocating the framebuffer failed";
        }
        // Start white, like an empty browser page.
        shadow_.assign(pixelCount, 0x00FFFFFFu);
        work_.assign(pixelCount, 0x00FFFFFFu);
        incoming_.assign(pixelCount, 0x00FFFFFFu);
        memcpy(screen_->frameBuffer, shadow_.data(), pixelCount * 4);

        screen_->desktopName = programName_.c_str();
        screen_->alwaysShared = TRUE;
        screen_->handleEventsEagerly = TRUE;
        screen_->deferUpdateTime = 5;
        screen_->screenData = this;
        screen_->ptrAddEvent = ptrAddEvent_;
        screen_->kbdAddEvent = kbdAddEvent_;
        screen_->setXCutText = setXCutText_;
        screen_->newClientHook = newClientHook_;

        screen_->autoPort = FALSE;
        screen_->port = opt_.listenPort;
        if(!rfbStringToAddr((char*)opt_.listenIP.c_str(), &screen_->listenInterface)) {
            return "Invalid listen address " + opt_.listenIP;
        }
        // Mirror the IPv4 choice for IPv6 so that a loopback-only server does
        // not accidentally listen on all IPv6 interfaces.
        if(opt_.listenIP == "127.0.0.1") {
            screen_->listen6Interface = (char*)"::1";
            screen_->ipv6port = opt_.listenPort;
        } else if(opt_.listenIP == "0.0.0.0") {
            screen_->ipv6port = opt_.listenPort;
        } else {
            screen_->ipv6port = 0;
        }

        if(!opt_.password.empty()) {
            passwords_[0] = createMallocString(opt_.password.substr(0, 8));
            passwords_[1] = nullptr;
            screen_->authPasswdData = (void*)passwords_;
            screen_->passwordCheck = rfbCheckPasswordByList;
        }

        cursors_[VICE_PLUGIN_API_MOUSE_CURSOR_NORMAL] = makeCursor(arrowArt);
        cursors_[VICE_PLUGIN_API_MOUSE_CURSOR_HAND] = makeCursor(handArt);
        cursors_[VICE_PLUGIN_API_MOUSE_CURSOR_TEXT] = makeCursor(textArt);
        for(rfbCursorPtr c : cursors_) {
            // We free the cursors ourselves in the destructor.
            c->cleanup = FALSE;
        }
        rfbSetCursor(screen_, cursors_[VICE_PLUGIN_API_MOUSE_CURSOR_NORMAL]);

        rfbInitServer(screen_);
        if(screen_->listenSock == RFB_INVALID_SOCKET) {
            return "Could not listen on " + opt_.listenIP + ":" +
                std::to_string(opt_.listenPort) + " (is the port already in use?)";
        }
        LOG_INFO(
            "VNC server listening on " + opt_.listenIP + ":" + std::to_string(opt_.listenPort) +
            ", screen " + std::to_string(opt_.width) + "x" + std::to_string(opt_.height) +
            (opt_.password.empty() ? ", no password" : ", password required")
        );
        return "";
    }

    void start(VicePluginAPI_Callbacks callbacks, void* callbackData) {
        REQUIRE(state_ == Created);
        callbacks_ = callbacks;
        callbackData_ = callbackData;
        state_ = Running;
        notifyAllowed_.store(true);
        stopVNC_.store(false);
        vncThread_ = std::thread([this]() { vncThreadMain_(); });
        notify_();
    }

    void shutdown() {
        REQUIRE(state_ == Running);
        state_ = ShutdownPending;
        notify_();
    }

    void pumpEvents() {
        if(state_ == ShutdownPending) {
            stopVNCThread_();
            vector<uint64_t> windows = windows_;
            windows_.clear();
            for(uint64_t w : windows) {
                callbacks_.closeWindow(callbackData_, w);
            }
            state_ = ShutDown;
            notifyAllowed_.store(false);
            callbacks_.shutdownComplete(callbackData_);
            return;
        }
        if(state_ != Running) {
            return;
        }

        if(windows_.empty()) {
            char* msg = nullptr;
            uint64_t handle = callbacks_.createWindow(callbackData_, &msg);
            if(handle != 0) {
                windows_.push_back(handle);
                windowCursors_[handle] = VICE_PLUGIN_API_MOUSE_CURSOR_NORMAL;
                needResize_.insert(handle);
                activeChanged_();
            } else {
                LOG_WARNING(
                    string("Creating a browser window was denied: ") +
                    (msg != nullptr ? msg : "(no reason given)") +
                    "; retrying when a VNC client connects"
                );
                free(msg);
            }
        }

        for(uint64_t w : needResize_) {
            callbacks_.resizeWindow(
                callbackData_, w, (size_t)opt_.width, (size_t)opt_.height
            );
        }
        needResize_.clear();

        std::deque<InputEvent> inputs;
        vector<string> clientCutTexts;
        bool clipboardDue = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inputs.swap(inputQueue_);
            clientCutTexts.swap(clientCutTexts_);
            clipboardDue = clipboardRequestDue_;
            clipboardRequestDue_ = false;
        }

        if(!windows_.empty()) {
            uint64_t active = windows_.back();
            for(const InputEvent& event : inputs) {
                if(event.type == InputEvent::Pointer) {
                    handlePointer_(active, event);
                } else {
                    handleKey_(active, event);
                }
            }
        }
        for(const string& text : clientCutTexts) {
            callbacks_.copyToClipboard(callbackData_, text.c_str());
        }
        if(clipboardDue) {
            callbacks_.requestClipboardContent(callbackData_);
        }

        if(viewDirty_ && !windows_.empty()) {
            viewDirty_ = false;
            callbacks_.fetchWindowImage(
                callbackData_, windows_.back(), putImage_, this
            );
        }
    }

    int createPopupWindow(uint64_t parent, uint64_t popup, char** msg) {
        (void)parent;
        if(state_ != Running) {
            if(msg != nullptr) *msg = createMallocString("Plugin is shutting down");
            return 0;
        }
        // VNC has a single screen, so popups (target=_blank links, window.open)
        // are shown on top of their parent; closing one returns to the window
        // below it, much like tabs.
        windows_.push_back(popup);
        windowCursors_[popup] = VICE_PLUGIN_API_MOUSE_CURSOR_NORMAL;
        needResize_.insert(popup);
        activeChanged_();
        return 1;
    }

    void closeWindow(uint64_t window) {
        auto it = std::find(windows_.begin(), windows_.end(), window);
        REQUIRE(it != windows_.end());
        bool wasActive = (window == windows_.back());
        windows_.erase(it);
        windowCursors_.erase(window);
        needResize_.erase(window);
        if(wasActive) {
            activeChanged_();
        }
    }

    void notifyWindowViewChanged(uint64_t window) {
        if(!windows_.empty() && windows_.back() == window) {
            viewDirty_ = true;
            notify_();
        }
    }

    void setWindowCursor(uint64_t window, VicePluginAPI_MouseCursor cursor) {
        int c = (int)cursor;
        if(c < 0 || c > 2) c = 0;
        windowCursors_[window] = c;
        if(!windows_.empty() && windows_.back() == window) {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingCursor_ = c;
        }
    }

    void putClipboardContent(const char* text) {
        string latin1 = utf8ToLatin1(text != nullptr ? text : "");
        std::lock_guard<std::mutex> lock(mutex_);
        serverCutTexts_.push_back(std::move(latin1));
    }

private:
    enum State { Created, Running, ShutdownPending, ShutDown };

    static Context* fromClient_(rfbClientPtr cl) {
        return (Context*)cl->screen->screenData;
    }

    void notify_() {
        if(notifyAllowed_.load()) {
            callbacks_.eventNotify(callbackData_);
        }
    }

    // Called on the program thread whenever the topmost window changes.
    void activeChanged_() {
        viewDirty_ = true;
        keysDown_.clear();
        controlDown_ = 0;
        if(!windows_.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingCursor_ = windowCursors_[windows_.back()];
        }
        notify_();
    }

    static void putImage_(
        void* data,
        const uint8_t* image,
        size_t width,
        size_t height,
        size_t pitch
    ) {
        Context* self = (Context*)data;
        size_t W = (size_t)self->opt_.width;
        size_t H = (size_t)self->opt_.height;
        size_t copyW = std::min(width, W);
        size_t copyH = std::min(height, H);

        std::lock_guard<std::mutex> lock(self->mutex_);
        uint32_t* dst = self->incoming_.data();
        for(size_t y = 0; y < H; ++y) {
            uint32_t* row = dst + y * W;
            if(y < copyH) {
                memcpy(row, image + 4 * pitch * y, 4 * copyW);
                std::fill(row + copyW, row + W, 0x00FFFFFFu);
            } else {
                std::fill(row, row + W, 0x00FFFFFFu);
            }
        }
        self->hasIncoming_ = true;
    }

    void handlePointer_(uint64_t window, const InputEvent& e) {
        int x = std::max(0, std::min(opt_.width - 1, e.x));
        int y = std::max(0, std::min(opt_.height - 1, e.y));

        if(x != lastX_ || y != lastY_) {
            callbacks_.mouseMove(callbackData_, window, x, y);
            lastX_ = x;
            lastY_ = y;
        }

        int changed = e.buttonMask ^ prevMask_;
        int pressed = changed & e.buttonMask;
        int released = changed & prevMask_;
        prevMask_ = e.buttonMask;

        // RFB button bits: 1 = left, 2 = middle, 4 = right; vice buttons:
        // 0 = left, 1 = middle, 2 = right.
        for(int b = 0; b < 3; ++b) {
            int bit = 1 << b;
            if(pressed & bit) {
                if(b == 0) {
                    auto sinceLast = e.time - lastLeftDownTime_;
                    if(
                        sinceLast < std::chrono::milliseconds(500) &&
                        abs(x - lastLeftDownX_) <= 4 &&
                        abs(y - lastLeftDownY_) <= 4
                    ) {
                        doubleClickPending_ = true;
                    }
                    lastLeftDownTime_ = e.time;
                    lastLeftDownX_ = x;
                    lastLeftDownY_ = y;
                }
                callbacks_.mouseDown(callbackData_, window, x, y, b);
            }
            if(released & bit) {
                callbacks_.mouseUp(callbackData_, window, x, y, b);
                if(b == 0 && doubleClickPending_) {
                    doubleClickPending_ = false;
                    // Do not let a third click count as another double click.
                    lastLeftDownTime_ = Clock::time_point();
                    callbacks_.mouseDoubleClick(callbackData_, window, x, y, 0);
                }
            }
        }

        // Wheel: 8 = up, 16 = down, 32 = left, 64 = right (press = one notch).
        // Vice convention: positive delta scrolls down/right.
        if(pressed & 8) callbacks_.mouseWheel(callbackData_, window, x, y, 0, -opt_.wheelStep);
        if(pressed & 16) callbacks_.mouseWheel(callbackData_, window, x, y, 0, opt_.wheelStep);
        if(pressed & 32) callbacks_.mouseWheel(callbackData_, window, x, y, -opt_.wheelStep, 0);
        if(pressed & 64) callbacks_.mouseWheel(callbackData_, window, x, y, opt_.wheelStep, 0);
    }

    void handleKey_(uint64_t window, const InputEvent& e) {
        if(e.down) {
            int key = keysymToViceKey(e.keysym, opt_.commandAsControl);
            if(key == 0) {
                return;
            }
            keysDown_[e.keysym] = key;
            if(key == -17) {
                ++controlDown_;
            }
            callbacks_.keyDown(callbackData_, window, key);

            // After Control-C / Control-X, fetch the program clipboard so
            // that the copied text reaches the VNC client's clipboard.
            if(controlDown_ > 0 && (key == 'c' || key == 'C' || key == 'x' || key == 'X')) {
                std::lock_guard<std::mutex> lock(mutex_);
                clipboardRequestAt_ = Clock::now() + std::chrono::milliseconds(300);
                clipboardRequestScheduled_ = true;
            }
        } else {
            auto it = keysDown_.find(e.keysym);
            if(it == keysDown_.end()) {
                return;
            }
            int key = it->second;
            keysDown_.erase(it);
            if(key == -17 && controlDown_ > 0) {
                --controlDown_;
            }
            callbacks_.keyUp(callbackData_, window, key);
        }
    }

    /******************************************
     * LibVNCServer callbacks (VNC thread) *
     ******************************************/

    static void ptrAddEvent_(int buttonMask, int x, int y, rfbClientPtr cl) {
        Context* self = fromClient_(cl);
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            InputEvent e = {};
            e.type = InputEvent::Pointer;
            e.buttonMask = buttonMask;
            e.x = x;
            e.y = y;
            e.time = Clock::now();
            self->inputQueue_.push_back(e);
        }
        self->notify_();
        rfbDefaultPtrAddEvent(buttonMask, x, y, cl);
    }

    static void kbdAddEvent_(rfbBool down, rfbKeySym keysym, rfbClientPtr cl) {
        Context* self = fromClient_(cl);
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            InputEvent e = {};
            e.type = InputEvent::Key;
            e.down = (bool)down;
            e.keysym = (uint32_t)keysym;
            e.time = Clock::now();
            self->inputQueue_.push_back(e);
        }
        self->notify_();
    }

    static void setXCutText_(char* str, int len, rfbClientPtr cl) {
        Context* self = fromClient_(cl);
        if(str == nullptr || len <= 0) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->clientCutTexts_.push_back(latin1ToUTF8(str, (size_t)len));
        }
        self->notify_();
    }

    static void clientGone_(rfbClientPtr cl) {
        Context* self = fromClient_(cl);
        LOG_INFO(string("VNC client disconnected: ") + (cl->host != nullptr ? cl->host : "?"));
        (void)self;
    }

    static enum rfbNewClientAction newClientHook_(rfbClientPtr cl) {
        Context* self = fromClient_(cl);
        cl->clientGoneHook = clientGone_;
        LOG_INFO(string("VNC client connected: ") + (cl->host != nullptr ? cl->host : "?"));
        // Gives the program thread a chance to recreate the browser window if
        // it was closed.
        self->notify_();
        return RFB_CLIENT_ACCEPT;
    }

    /*****************
     * VNC thread *
     *****************/

    void vncThreadMain_() {
        while(!stopVNC_.load()) {
            processMailbox_();
            rfbProcessEvents(screen_, 2000);
        }
    }

    void stopVNCThread_() {
        if(vncThread_.joinable()) {
            stopVNC_.store(true);
            vncThread_.join();
        }
    }

    void processMailbox_() {
        bool haveFrame = false;
        int cursor = -1;
        vector<string> cutTexts;
        bool clipboardDue = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(hasIncoming_) {
                work_.swap(incoming_);
                hasIncoming_ = false;
                haveFrame = true;
            }
            cursor = pendingCursor_;
            pendingCursor_ = -1;
            cutTexts.swap(serverCutTexts_);
            if(clipboardRequestScheduled_ && Clock::now() >= clipboardRequestAt_) {
                clipboardRequestScheduled_ = false;
                clipboardRequestDue_ = true;
                clipboardDue = true;
            }
        }
        if(clipboardDue) {
            notify_();
        }
        if(cursor >= 0 && cursor != currentCursor_) {
            currentCursor_ = cursor;
            rfbSetCursor(screen_, cursors_[cursor]);
        }
        for(string& text : cutTexts) {
            if(!text.empty()) {
                rfbSendServerCutText(screen_, &text[0], (int)text.size());
            }
        }
        if(haveFrame) {
            applyFrame_();
        }
    }

    // Brings the framebuffer (and our shadow copy of it) up to date with
    // work_, telling LibVNCServer which regions changed.
    void applyFrame_() {
        int W = opt_.width;
        int H = opt_.height;

        if(opt_.scrollDetection) {
            // Ignore the rightmost columns where a scrollbar thumb may move.
            ScrollMatch m = detectVerticalScroll(
                shadow_.data(), work_.data(), W, H, W, 24
            );
            if(m.found) {
                // Destination rect [x1,x2) x [y1,y2), source shifted by -dy.
                rfbDoCopyRect(screen_, m.x1, m.y1, m.x2, m.y2, 0, m.dy);
                applyScroll(shadow_.data(), W, m);
            }
        }

        tiles_.clear();
        findChangedTiles(shadow_.data(), work_.data(), W, H, W, 32, tiles_);
        uint32_t* fb = (uint32_t*)screen_->frameBuffer;
        for(const Rect& r : tiles_) {
            size_t count = (size_t)(r.x2 - r.x1);
            for(int y = r.y1; y < r.y2; ++y) {
                size_t off = (size_t)y * (size_t)W + (size_t)r.x1;
                memcpy(&shadow_[off], &work_[off], count * 4);
                memcpy(fb + off, &work_[off], count * 4);
            }
            rfbMarkRectAsModified(screen_, r.x1, r.y1, r.x2, r.y2);
        }
    }

    Options opt_;
    string programName_;
    VicePluginAPI_Callbacks callbacks_ = {};
    void* callbackData_ = nullptr;
    State state_ = Created;
    std::atomic<bool> notifyAllowed_{false};

    // Program thread state.
    vector<uint64_t> windows_; // stack; back() is shown
    std::map<uint64_t, int> windowCursors_;
    std::set<uint64_t> needResize_;
    bool viewDirty_ = false;
    int prevMask_ = 0;
    int lastX_ = -1, lastY_ = -1;
    Clock::time_point lastLeftDownTime_;
    int lastLeftDownX_ = 0, lastLeftDownY_ = 0;
    bool doubleClickPending_ = false;
    std::map<uint32_t, int> keysDown_;
    int controlDown_ = 0;

    // Shared state, protected by mutex_.
    std::mutex mutex_;
    std::deque<InputEvent> inputQueue_;
    vector<string> clientCutTexts_;
    vector<string> serverCutTexts_;
    vector<uint32_t> incoming_;
    bool hasIncoming_ = false;
    int pendingCursor_ = -1;
    Clock::time_point clipboardRequestAt_;
    bool clipboardRequestScheduled_ = false;
    bool clipboardRequestDue_ = false;

    // VNC thread state.
    std::thread vncThread_;
    std::atomic<bool> stopVNC_{false};
    rfbScreenInfoPtr screen_ = nullptr;
    vector<uint32_t> work_;
    vector<uint32_t> shadow_;
    vector<Rect> tiles_;
    rfbCursorPtr cursors_[3] = {nullptr, nullptr, nullptr};
    int currentCursor_ = VICE_PLUGIN_API_MOUSE_CURSOR_NORMAL;
    char* passwords_[2] = {nullptr, nullptr};
};

bool contextExists = false;
std::mutex contextExistsMutex;

#define API_FUNC_START try {
#define API_FUNC_END \
    } catch(const std::exception& e) { \
        panicMsg(LOCATION, string("Unhandled exception: ") + e.what()); \
    } catch(...) { \
        panicMsg(LOCATION, "Unhandled exception"); \
    }

}

extern "C" {

struct VicePluginAPI_Context {
    std::unique_ptr<Context> impl;
};

API_EXPORT int vicePluginAPI_isAPIVersionSupported(uint64_t apiVersion) {
    return apiVersion == APIVersion ? 1 : 0;
}

API_EXPORT char* vicePluginAPI_createVersionString() {
API_FUNC_START
    return createMallocString(string("Rfbvice ") + RfbviceVersion);
API_FUNC_END
}

API_EXPORT char* vicePluginAPI_createCreditsString() {
API_FUNC_START
    return createMallocString(
        string("Rfbvice ") + RfbviceVersion + ": VNC (RFB) vice plugin for RetroWEBRebirth.\n"
        "Uses LibVNCServer (https://libvnc.github.io/), licensed under the GNU GPL v2 or later.\n"
    );
API_FUNC_END
}

API_EXPORT void* vicePluginAPI_malloc(size_t size) {
    return malloc(size);
}

API_EXPORT void vicePluginAPI_free(void* ptr) {
    free(ptr);
}

API_EXPORT VicePluginAPI_Context* vicePluginAPI_initContext(
    uint64_t apiVersion,
    const char** optionNames,
    const char** optionValues,
    size_t optionCount,
    const char* programName,
    char** initErrorMsgOut
) {
API_FUNC_START
    REQUIRE(apiVersion == APIVersion);

    auto fail = [&](const string& msg) -> VicePluginAPI_Context* {
        if(initErrorMsgOut != nullptr) {
            *initErrorMsgOut = createMallocString(msg);
        }
        return nullptr;
    };

    {
        std::lock_guard<std::mutex> lock(contextExistsMutex);
        if(contextExists) {
            return fail("rfbvice supports only one context at a time");
        }
    }

    vector<std::pair<string, string>> opts;
    for(size_t i = 0; i < optionCount; ++i) {
        REQUIRE(optionNames != nullptr && optionValues != nullptr);
        REQUIRE(optionNames[i] != nullptr && optionValues[i] != nullptr);
        opts.emplace_back(optionNames[i], optionValues[i]);
    }
    Options options;
    string err = parseOptions(opts, options);
    if(!err.empty()) {
        return fail(err);
    }

    rfbLog = libvncLog;
    rfbErr = libvncLog;

    string name = programName != nullptr ? programName : "Browser";
    std::unique_ptr<Context> impl(new Context(options, name));
    err = impl->init();
    if(!err.empty()) {
        return fail(err);
    }

    {
        std::lock_guard<std::mutex> lock(contextExistsMutex);
        contextExists = true;
    }
    VicePluginAPI_Context* ctx = new VicePluginAPI_Context;
    ctx->impl = std::move(impl);
    return ctx;
API_FUNC_END
}

API_EXPORT void vicePluginAPI_destroyContext(VicePluginAPI_Context* ctx) {
API_FUNC_START
    REQUIRE(ctx != nullptr);
    delete ctx;
    std::lock_guard<std::mutex> lock(contextExistsMutex);
    contextExists = false;
API_FUNC_END
}

API_EXPORT void vicePluginAPI_start(
    VicePluginAPI_Context* ctx,
    VicePluginAPI_Callbacks callbacks,
    void* callbackData
) {
API_FUNC_START
    ctx->impl->start(callbacks, callbackData);
API_FUNC_END
}

API_EXPORT void vicePluginAPI_shutdown(VicePluginAPI_Context* ctx) {
API_FUNC_START
    ctx->impl->shutdown();
API_FUNC_END
}

API_EXPORT void vicePluginAPI_pumpEvents(VicePluginAPI_Context* ctx) {
API_FUNC_START
    ctx->impl->pumpEvents();
API_FUNC_END
}

API_EXPORT int vicePluginAPI_createPopupWindow(
    VicePluginAPI_Context* ctx,
    uint64_t parentWindow,
    uint64_t popupWindow,
    char** msg
) {
API_FUNC_START
    return ctx->impl->createPopupWindow(parentWindow, popupWindow, msg);
API_FUNC_END
}

API_EXPORT void vicePluginAPI_closeWindow(VicePluginAPI_Context* ctx, uint64_t window) {
API_FUNC_START
    ctx->impl->closeWindow(window);
API_FUNC_END
}

API_EXPORT void vicePluginAPI_notifyWindowViewChanged(
    VicePluginAPI_Context* ctx,
    uint64_t window
) {
API_FUNC_START
    ctx->impl->notifyWindowViewChanged(window);
API_FUNC_END
}

API_EXPORT void vicePluginAPI_setWindowCursor(
    VicePluginAPI_Context* ctx,
    uint64_t window,
    VicePluginAPI_MouseCursor cursor
) {
API_FUNC_START
    ctx->impl->setWindowCursor(window, cursor);
API_FUNC_END
}

API_EXPORT int vicePluginAPI_windowQualitySelectorQuery(
    VicePluginAPI_Context*, uint64_t, char**, size_t*
) {
    // Image quality is chosen by the VNC client (e.g. its Tight JPEG setting).
    return 0;
}

API_EXPORT void vicePluginAPI_windowQualityChanged(VicePluginAPI_Context*, uint64_t, size_t) {}

API_EXPORT int vicePluginAPI_windowNeedsClipboardButtonQuery(VicePluginAPI_Context*, uint64_t) {
    // Clipboard text travels through the VNC protocol automatically.
    return 0;
}

API_EXPORT void vicePluginAPI_windowClipboardButtonPressed(VicePluginAPI_Context*, uint64_t) {}

API_EXPORT void vicePluginAPI_putClipboardContent(VicePluginAPI_Context* ctx, const char* text) {
API_FUNC_START
    ctx->impl->putClipboardContent(text);
API_FUNC_END
}

API_EXPORT void vicePluginAPI_putFileDownload(
    VicePluginAPI_Context*,
    uint64_t,
    const char* name,
    const char*,
    void (*cleanup)(void*),
    void* cleanupData
) {
API_FUNC_START
    LOG_WARNING(
        string("File downloads are not supported over VNC; ignoring download '") +
        (name != nullptr ? name : "") + "'"
    );
    cleanup(cleanupData);
API_FUNC_END
}

API_EXPORT int vicePluginAPI_startFileUpload(VicePluginAPI_Context*, uint64_t) {
    // VNC has no file transfer in the base protocol.
    return 0;
}

API_EXPORT void vicePluginAPI_cancelFileUpload(VicePluginAPI_Context*, uint64_t) {}

API_EXPORT void vicePluginAPI_getOptionDocs(
    uint64_t apiVersion,
    void (*callback)(void*, const char*, const char*, const char*, const char*),
    void* data
) {
API_FUNC_START
    REQUIRE(apiVersion == APIVersion);
    for(const auto& doc : optionDocs) {
        callback(data, doc[0], doc[1], doc[2], doc[3]);
    }
API_FUNC_END
}

API_EXPORT void vicePluginAPI_setGlobalLogCallback(
    uint64_t apiVersion,
    void (*callback)(void*, VicePluginAPI_LogLevel, const char*, const char*),
    void* data,
    void (*destructorCallback)(void*)
) {
API_FUNC_START
    REQUIRE(apiVersion == APIVersion);
    std::lock_guard<std::mutex> lock(logMutex);
    if(callback == nullptr) {
        logCallback = nullptr;
    } else {
        std::shared_ptr<void> holder(data, [destructorCallback](void* d) {
            if(destructorCallback != nullptr) destructorCallback(d);
        });
        logCallback = [callback, holder](
            VicePluginAPI_LogLevel level, const char* location, const char* msg
        ) {
            callback(holder.get(), level, location, msg);
        };
    }
API_FUNC_END
}

API_EXPORT void vicePluginAPI_setGlobalPanicCallback(
    uint64_t apiVersion,
    void (*callback)(void*, const char*, const char*),
    void* data,
    void (*destructorCallback)(void*)
) {
API_FUNC_START
    REQUIRE(apiVersion == APIVersion);
    std::lock_guard<std::mutex> lock(logMutex);
    if(callback == nullptr) {
        panicCallback = nullptr;
    } else {
        std::shared_ptr<void> holder(data, [destructorCallback](void* d) {
            if(destructorCallback != nullptr) destructorCallback(d);
        });
        panicCallback = [callback, holder](const char* location, const char* msg) {
            callback(holder.get(), location, msg);
        };
    }
API_FUNC_END
}

API_EXPORT int vicePluginAPI_isExtensionSupported(uint64_t apiVersion, const char* name) {
    if(apiVersion != APIVersion || name == nullptr) {
        return 0;
    }
    return strcmp(name, "PluginNavigationControlSupportQuery") == 0 ? 1 : 0;
}

API_EXPORT int vicePluginAPI_PluginNavigationControlSupportQuery_query(
    VicePluginAPI_Context*
) {
    // The plugin has no navigation buttons of its own, so Browservice shows
    // back/forward/refresh in its control bar.
    return 0;
}

}
