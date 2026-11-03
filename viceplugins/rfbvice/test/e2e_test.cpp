// End-to-end test for rfbvice: loads the plugin like Browservice does, plays
// the program's side of the vice plugin API with a synthetic scrolling page,
// and connects a real VNC client (libvncclient) to check what it receives.
//
// Usage: e2e_test path/to/rfbvice.so

#include "../../../vice_plugin_api.h"

#include <rfb/rfbclient.h>

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;

static int failures = 0;
#define CHECK(cond) do { \
    if(!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while(0)

const int W = 640, H = 480, Header = 40, DocH = 5000;
const int Port = 59123;

/*********** API functions loaded from the plugin ***********/

#define API_FUNCS(X) \
    X(isAPIVersionSupported) X(initContext) X(destroyContext) X(start) X(shutdown) \
    X(pumpEvents) X(createPopupWindow) X(closeWindow) X(notifyWindowViewChanged) \
    X(setWindowCursor) X(putClipboardContent) X(setGlobalLogCallback) X(free) \
    X(isExtensionSupported) X(PluginNavigationControlSupportQuery_query)
#define DECL(name) decltype(&vicePluginAPI_##name) api_##name;
API_FUNCS(DECL)

/*********** Host ("program") state; touched only on the main thread except notifyFlag ***********/

mutex notifyMutex;
condition_variable notifyCv;
bool notifyFlag = false;

vector<uint32_t> doc;
int scrollOffset = 0;
uint32_t popupColor = 0x00336699u;
vector<uint64_t> openWindows;
uint64_t nextWindow = 1;
vector<string> hostLog;
bool shutdownCompleted = false;
vector<pair<uint64_t, pair<size_t, size_t>>> resizes;

vector<uint32_t> hostImage(uint64_t window) {
    vector<uint32_t> img((size_t)W * H);
    for(int y = 0; y < H; ++y) {
        for(int x = 0; x < W; ++x) {
            uint32_t v;
            if(window != 1) {
                v = popupColor;
            } else if(y < Header) {
                v = 0x00C0C0C0u ^ (uint32_t)((x * 7 + y * 13) & 0x3F);
            } else {
                v = doc[(size_t)(y - Header + scrollOffset) * W + (size_t)x];
            }
            img[(size_t)y * W + (size_t)x] = v;
        }
    }
    return img;
}

void cbEventNotify(void*) {
    lock_guard<mutex> lock(notifyMutex);
    notifyFlag = true;
    notifyCv.notify_one();
}
void cbShutdownComplete(void*) { shutdownCompleted = true; hostLog.push_back("shutdownComplete"); }
uint64_t cbCreateWindow(void*, char**) {
    uint64_t h = nextWindow++;
    openWindows.push_back(h);
    hostLog.push_back("createWindow " + to_string(h));
    return h;
}
void cbCloseWindow(void*, uint64_t w) { hostLog.push_back("closeWindow " + to_string(w)); }
void cbResizeWindow(void*, uint64_t w, size_t width, size_t height) {
    resizes.push_back({w, {width, height}});
}
void cbFetchWindowImage(void*, uint64_t window,
    void (*put)(void*, const uint8_t*, size_t, size_t, size_t), void* data) {
    vector<uint32_t> img = hostImage(window);
    // Garbage in the unused byte must not matter.
    for(auto& p : img) p |= 0xAB000000u;
    put(data, (const uint8_t*)img.data(), W, H, W);
}
void cbMouseDown(void*, uint64_t w, int x, int y, int b) { hostLog.push_back("down " + to_string(w) + " " + to_string(x) + " " + to_string(y) + " " + to_string(b)); }
void cbMouseUp(void*, uint64_t w, int x, int y, int b) { hostLog.push_back("up " + to_string(w) + " " + to_string(x) + " " + to_string(y) + " " + to_string(b)); }
void cbMouseMove(void*, uint64_t, int x, int y) { hostLog.push_back("move " + to_string(x) + " " + to_string(y)); }
void cbMouseDoubleClick(void*, uint64_t, int x, int y, int b) { hostLog.push_back("dbl " + to_string(x) + " " + to_string(y) + " " + to_string(b)); }
void cbMouseWheel(void*, uint64_t, int, int, int dx, int dy) { hostLog.push_back("wheel " + to_string(dx) + " " + to_string(dy)); }
void cbMouseLeave(void*, uint64_t, int, int) {}
void cbKeyDown(void*, uint64_t, int key) { hostLog.push_back("kdn " + to_string(key)); }
void cbKeyUp(void*, uint64_t, int key) { hostLog.push_back("kup " + to_string(key)); }
void cbLoseFocus(void*, uint64_t) {}
void cbNavigate(void*, uint64_t, int) {}
void cbCopyToClipboard(void*, const char* text) { hostLog.push_back(string("copy ") + text); }
int cbRequestClipboard(void*) { hostLog.push_back("requestClipboard"); return 1; }
void cbUploadFile(void*, uint64_t, const char*, const char*, void (*)(void*), void*) {}
void cbCancelFileUpload(void*, uint64_t) {}

/*********** VNC client (runs on its own thread) ***********/

mutex clientMutex;
vector<uint32_t> clientSnapshot;
int copyRectCount = 0;
long rawPixelsUpdated = 0;     // pixels received in non-CopyRect rectangles
long copyPixels = 0;           // pixels covered by CopyRect rectangles
int cursorShapes = 0;
int lastCursorWidth = 0;
string lastServerCutText;
deque<function<void(rfbClient*)>> clientCommands;
atomic<bool> clientStop{false};
atomic<bool> clientReady{false};
GotCopyRectProc defaultCopyRect = nullptr;

void onCopyRect(rfbClient* c, int sx, int sy, int w, int h, int dx, int dy) {
    { lock_guard<mutex> lock(clientMutex); ++copyRectCount; copyPixels += (long)w * h; }
    defaultCopyRect(c, sx, sy, w, h, dx, dy);
}
// Called for every rectangle, CopyRect included; CopyRect area is subtracted
// when reporting.
void onUpdate(rfbClient* c, int x, int y, int w, int h) {
    (void)c; (void)x; (void)y;
    lock_guard<mutex> lock(clientMutex);
    rawPixelsUpdated += (long)w * h;
}
void onFinished(rfbClient* c) {
    lock_guard<mutex> lock(clientMutex);
    clientSnapshot.assign((uint32_t*)c->frameBuffer, (uint32_t*)c->frameBuffer + (size_t)W * H);
}
void onCursor(rfbClient*, int, int, int width, int, int) {
    lock_guard<mutex> lock(clientMutex);
    ++cursorShapes;
    lastCursorWidth = width;
}
void onCutText(rfbClient*, const char* text, int len) {
    lock_guard<mutex> lock(clientMutex);
    lastServerCutText.assign(text, (size_t)len);
}

void clientThread() {
    rfbClient* c = rfbGetClient(8, 3, 4);
    c->format.redShift = 16;
    c->format.greenShift = 8;
    c->format.blueShift = 0;
    c->appData.encodingsString = "copyrect tight zrle hextile raw";
    c->appData.enableJPEG = FALSE; // lossless, so frames can be compared exactly
    c->appData.useRemoteCursor = TRUE;
    c->serverHost = strdup("127.0.0.1");
    c->serverPort = Port;
    c->canHandleNewFBSize = FALSE;
    defaultCopyRect = c->GotCopyRect;
    c->GotCopyRect = onCopyRect;
    c->GotFrameBufferUpdate = onUpdate;
    c->FinishedFrameBufferUpdate = onFinished;
    c->GotCursorShape = onCursor;
    c->GotXCutText = onCutText;
    int argc = 0;
    if(!rfbInitClient(c, &argc, nullptr)) {
        fprintf(stderr, "client: could not connect\n");
        ++failures;
        clientReady = true;
        return;
    }
    CHECK(c->width == W && c->height == H);
    clientReady = true;
    while(!clientStop) {
        deque<function<void(rfbClient*)>> cmds;
        { lock_guard<mutex> lock(clientMutex); cmds.swap(clientCommands); }
        for(auto& f : cmds) f(c);
        int n = WaitForMessage(c, 5000);
        if(n < 0) break;
        if(n > 0 && !HandleRFBServerMessage(c)) break;
    }
    rfbClientCleanup(c);
}

void clientDo(function<void(rfbClient*)> f) {
    lock_guard<mutex> lock(clientMutex);
    clientCommands.push_back(move(f));
}

/*********** Test driver (main thread) ***********/

VicePluginAPI_Context* ctx = nullptr;

void pumpFor(chrono::milliseconds dur) {
    auto end = Clock::now() + dur;
    while(Clock::now() < end) {
        {
            unique_lock<mutex> lock(notifyMutex);
            notifyCv.wait_for(lock, chrono::milliseconds(5), [] { return notifyFlag; });
            notifyFlag = false;
        }
        api_pumpEvents(ctx);
    }
}

bool pumpUntil(function<bool()> cond, int timeoutMs = 3000) {
    auto end = Clock::now() + chrono::milliseconds(timeoutMs);
    while(Clock::now() < end) {
        pumpFor(chrono::milliseconds(10));
        if(cond()) return true;
    }
    return false;
}

bool clientMatches(uint64_t window) {
    vector<uint32_t> expected = hostImage(window);
    lock_guard<mutex> lock(clientMutex);
    if(clientSnapshot.size() != expected.size()) return false;
    for(size_t i = 0; i < expected.size(); ++i) {
        if((clientSnapshot[i] ^ expected[i]) & 0x00FFFFFFu) return false;
    }
    return true;
}

bool logHas(const string& entry) {
    for(const string& s : hostLog) if(s == entry) return true;
    return false;
}

void dumpLog() {
    for(const string& s : hostLog) fprintf(stderr, "  log: %s\n", s.c_str());
}

int main(int argc, char** argv) {
    if(argc != 2) {
        fprintf(stderr, "usage: %s rfbvice.so\n", argv[0]);
        return 2;
    }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if(!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
#define LOAD(name) api_##name = (decltype(api_##name))dlsym(lib, "vicePluginAPI_" #name); \
    if(!api_##name) { fprintf(stderr, "missing symbol " #name "\n"); return 2; }
    API_FUNCS(LOAD)

    CHECK(api_isAPIVersionSupported(2000000) == 1);
    CHECK(api_isExtensionSupported(2000000, "PluginNavigationControlSupportQuery") == 1);

    // Synthetic article: random "text" lines.
    mt19937 rng(42);
    doc.assign((size_t)W * DocH, 0x00FFFFFFu);
    for(int y = 0; y < DocH; ++y) {
        if(y % 18 >= 12) continue;
        for(int x = 8; x < W - 8; ++x) if(rng() % 3 == 0) doc[(size_t)y * W + (size_t)x] = 0x00202020u + (rng() % 64);
    }

    // Bad options are rejected with a message.
    {
        const char* names[] = {"screen-size"};
        const char* values[] = {"huge"};
        char* err = nullptr;
        CHECK(api_initContext(2000000, names, values, 1, "Test", &err) == nullptr);
        CHECK(err != nullptr && strstr(err, "screen-size") != nullptr);
        api_free(err);
    }

    string listen = "127.0.0.1:" + to_string(Port);
    const char* names[] = {"vnc-listen-addr", "screen-size"};
    const char* values[] = {listen.c_str(), "640x480"};
    char* err = nullptr;
    ctx = api_initContext(2000000, names, values, 2, "RetroWEBRebirth", &err);
    if(!ctx) { fprintf(stderr, "initContext: %s\n", err ? err : "?"); return 1; }
    CHECK(api_PluginNavigationControlSupportQuery_query(ctx) == 0);

    VicePluginAPI_Callbacks cb = {};
    cb.eventNotify = cbEventNotify; cb.shutdownComplete = cbShutdownComplete;
    cb.createWindow = cbCreateWindow; cb.closeWindow = cbCloseWindow;
    cb.resizeWindow = cbResizeWindow; cb.fetchWindowImage = cbFetchWindowImage;
    cb.mouseDown = cbMouseDown; cb.mouseUp = cbMouseUp; cb.mouseMove = cbMouseMove;
    cb.mouseDoubleClick = cbMouseDoubleClick; cb.mouseWheel = cbMouseWheel;
    cb.mouseLeave = cbMouseLeave; cb.keyDown = cbKeyDown; cb.keyUp = cbKeyUp;
    cb.loseFocus = cbLoseFocus; cb.navigate = cbNavigate;
    cb.copyToClipboard = cbCopyToClipboard; cb.requestClipboardContent = cbRequestClipboard;
    cb.uploadFile = cbUploadFile; cb.cancelFileUpload = cbCancelFileUpload;
    api_start(ctx, cb, nullptr);

    // 1. The plugin creates a window and asks for the screen size.
    CHECK(pumpUntil([] { return !openWindows.empty(); }));
    CHECK(!resizes.empty() && resizes[0].second.first == (size_t)W && resizes[0].second.second == (size_t)H);

    thread ct(clientThread);
    while(!clientReady) pumpFor(chrono::milliseconds(10));

    // 2. The client receives the initial page.
    CHECK(pumpUntil([] { return clientMatches(1); }));
    printf("initial frame received\n");

    // 3. Scrolling is sent as CopyRect plus a small strip.
    for(int step : {30, 30, 90, -60}) {
        int copiesBefore; long pixelsBefore;
        { lock_guard<mutex> lock(clientMutex); copiesBefore = copyRectCount; pixelsBefore = rawPixelsUpdated - copyPixels; }
        scrollOffset += step;
        api_notifyWindowViewChanged(ctx, 1);
        bool ok = pumpUntil([] { return clientMatches(1); });
        CHECK(ok);
        pumpFor(chrono::milliseconds(50));
        int copies; long pixels;
        { lock_guard<mutex> lock(clientMutex); copies = copyRectCount - copiesBefore; pixels = rawPixelsUpdated - copyPixels - pixelsBefore; }
        printf("scroll %+d px: %d CopyRect, %ld new pixels (%.1f%% of screen)\n",
               step, copies, pixels, 100.0 * (double)pixels / (W * H));
        CHECK(copies >= 1);
        CHECK(pixels <= (long)(abs(step) + 96) * W);
    }

    // 4. Pointer input: move, click, wheel, double click.
    clientDo([](rfbClient* c) {
        SendPointerEvent(c, 100, 200, 0);
        SendPointerEvent(c, 100, 200, 1);
        SendPointerEvent(c, 100, 200, 0);
        SendPointerEvent(c, 100, 200, 16);
        SendPointerEvent(c, 100, 200, 0);
        SendPointerEvent(c, 100, 200, 8);
        SendPointerEvent(c, 100, 200, 0);
    });
    CHECK(pumpUntil([] { return logHas("wheel 0 -120"); }));
    CHECK(logHas("move 100 200"));
    CHECK(logHas("down 1 100 200 0"));
    CHECK(logHas("up 1 100 200 0"));
    CHECK(logHas("wheel 0 120"));
    CHECK(!logHas("dbl 100 200 0"));
    clientDo([](rfbClient* c) {
        SendPointerEvent(c, 300, 100, 1);
        SendPointerEvent(c, 300, 100, 0);
        SendPointerEvent(c, 301, 101, 1);
        SendPointerEvent(c, 301, 101, 0);
    });
    CHECK(pumpUntil([] { return logHas("dbl 301 101 0"); }));
    printf("pointer events OK\n");

    // 5. Keyboard: characters, special keys, Command mapped to Control.
    clientDo([](rfbClient* c) {
        SendKeyEvent(c, 'a', TRUE); SendKeyEvent(c, 'a', FALSE);
        SendKeyEvent(c, 0xFF0D, TRUE); SendKeyEvent(c, 0xFF0D, FALSE);   // Return
        SendKeyEvent(c, 0xFFE7, TRUE);                                    // Meta_L (Mac Command)
        SendKeyEvent(c, 'c', TRUE); SendKeyEvent(c, 'c', FALSE);
        SendKeyEvent(c, 0xFFE7, FALSE);
        SendKeyEvent(c, 0x01000416, TRUE); SendKeyEvent(c, 0x01000416, FALSE); // Unicode keysym
    });
    CHECK(pumpUntil([] { return logHas("kup 1046"); }));
    CHECK(logHas("kdn 97") && logHas("kup 97"));
    CHECK(logHas("kdn -13") && logHas("kup -13"));
    CHECK(logHas("kdn -17") && logHas("kup -17"));
    // Command-C asks the program for its clipboard shortly afterwards.
    CHECK(pumpUntil([] { return logHas("requestClipboard"); }));
    printf("keyboard events OK\n");

    // 6. Clipboard in both directions (Latin-1 on the wire, UTF-8 in the API).
    clientDo([](rfbClient* c) { char t[] = "caf\xe9"; SendClientCutText(c, t, 4); });
    CHECK(pumpUntil([] { return logHas("copy caf\xc3\xa9"); }));
    api_putClipboardContent(ctx, "na\xc3\xafve \xe2\x82\xac");
    CHECK(pumpUntil([] { lock_guard<mutex> lock(clientMutex); return lastServerCutText == "na\xefve ?"; }));
    printf("clipboard OK\n");

    // 7. Cursor shape updates.
    int shapesBefore;
    { lock_guard<mutex> lock(clientMutex); shapesBefore = cursorShapes; }
    api_setWindowCursor(ctx, 1, VICE_PLUGIN_API_MOUSE_CURSOR_HAND);
    clientDo([](rfbClient* c) { SendPointerEvent(c, 50, 60, 0); }); // nudge an update
    CHECK(pumpUntil([&] { lock_guard<mutex> lock(clientMutex); return cursorShapes > shapesBefore && lastCursorWidth == 16; }));
    printf("cursor shape OK\n");

    // 8. Popups are shown on top, and closing them returns to the parent.
    char* msg = nullptr;
    CHECK(api_createPopupWindow(ctx, 1, 100, &msg) == 1);
    CHECK(pumpUntil([] { return clientMatches(100); }));
    api_closeWindow(ctx, 100);
    CHECK(pumpUntil([] { return clientMatches(1); }));
    printf("popup OK\n");

    // 9. Changes to a window that is not shown are ignored, then shown later.
    api_createPopupWindow(ctx, 1, 101, &msg);
    CHECK(pumpUntil([] { return clientMatches(101); }));
    scrollOffset += 45;
    api_notifyWindowViewChanged(ctx, 1);
    pumpFor(chrono::milliseconds(100));
    CHECK(clientMatches(101));
    api_closeWindow(ctx, 101);
    CHECK(pumpUntil([] { return clientMatches(1); }));

    // 10. Shutdown closes the remaining window and completes.
    clientStop = true;
    api_shutdown(ctx);
    CHECK(pumpUntil([] { return shutdownCompleted; }));
    CHECK(logHas("closeWindow 1"));
    ct.join();
    api_destroyContext(ctx);

    if(failures) {
        dumpLog();
        fprintf(stderr, "e2e_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("e2e_test: all tests passed\n");
    return 0;
}
