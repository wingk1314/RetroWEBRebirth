# Modding Browservice for VintageStreamProxy Integration

This document describes how to modify Browservice's C++ code to automatically detect YouTube video URLs and trigger VLC playback via VintageStreamProxy.

## Architecture Overview

### Current Flow

```
User clicks link → CEF OnBeforeBrowse() → Navigation proceeds → Page renders
```

### Desired Flow

```
User clicks YouTube link → CEF OnBeforeBrowse() → Detect video ID → 
  → Send command to client → Client opens VLC → Block navigation
```

## Key Files to Modify

### 1. `src/window.cpp` — Navigation Interception

The `OnBeforeBrowse` function (line 431) is called before every navigation. Currently it only blocks local file requests:

```cpp
virtual bool OnBeforeBrowse(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    bool userGesture,
    bool isRedirect
) override {
    REQUIRE_UI_THREAD();
    REQUIRE(window_->state_ == Open);
    REQUIRE(browser);
    REQUIRE(window_->browser_);
    REQUIRE(window_->browser_->IsSame(browser));

    // Current: Block local files
    REQUIRE(request);
    return globals->config->blockFileScheme && isLocalFileRequestURL(request->GetURL());
}
```

**Modification needed:**

```cpp
virtual bool OnBeforeBrowse(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request,
    bool userGesture,
    bool isRedirect
) override {
    REQUIRE_UI_THREAD();
    REQUIRE(window_->state_ == Open);
    REQUIRE(browser);
    REQUIRE(window_->browser_);
    REQUIRE(window_->browser_->IsSame(browser));

    REQUIRE(request);
    string url = string(request->GetURL());
    
    // NEW: Check for YouTube video URLs
    if (isYouTubeVideoURL(url)) {
        string videoId = extractYouTubeVideoId(url);
        if (!videoId.empty()) {
            // Send command to client to open VLC
            window_->sendVLCCommand(videoId);
            
            // Show notification in browser
            window_->showNotification("Opening in VLC: " + videoId);
            
            // Block the navigation
            return true;
        }
    }
    
    // Existing: Block local files
    return globals->config->blockFileScheme && isLocalFileRequestURL(url);
}
```

### 2. `src/window.hpp` — New Methods

Add methods to handle YouTube detection and VLC commands:

```cpp
class Window {
    // ... existing code ...
    
    // NEW: YouTube/VLC integration
    bool isYouTubeVideoURL(const string& url);
    string extractYouTubeVideoId(const string& url);
    void sendVLCCommand(const string& videoId);
    void showNotification(const string& message);
    
    // NEW: VintageStreamProxy configuration
    string vspServer_;
    string vspProfile_;
};
```

### 3. `src/window.cpp` — Implementation

```cpp
bool Window::isYouTubeVideoURL(const string& url) {
    // Check for YouTube video URL patterns
    return (url.find("youtube.com/watch") != string::npos ||
            url.find("youtu.be/") != string::npos ||
            url.find("youtube.com/embed/") != string::npos);
}

string Window::extractYouTubeVideoId(const string& url) {
    // Extract video ID from various URL formats
    size_t pos;
    
    // youtube.com/watch?v=VIDEO_ID
    pos = url.find("v=");
    if (pos != string::npos) {
        string id = url.substr(pos + 2, 11);
        if (id.length() == 11) return id;
    }
    
    // youtu.be/VIDEO_ID
    pos = url.find("youtu.be/");
    if (pos != string::npos) {
        string id = url.substr(pos + 9, 11);
        if (id.length() == 11) return id;
    }
    
    // youtube.com/embed/VIDEO_ID
    pos = url.find("youtube.com/embed/");
    if (pos != string::npos) {
        string id = url.substr(pos + 18, 11);
        if (id.length() == 11) return id;
    }
    
    return "";
}

void Window::sendVLCCommand(const string& videoId) {
    // Option 1: Send via WebSocket to client
    // Option 2: Trigger HTTP request to local VLC
    // Option 3: Use OS command to open VLC
    
    string streamUrl = vspServer_ + "/" + vspProfile_ + "?v=" + videoId;
    
    // For now, log the command (actual implementation depends on client capabilities)
    INFO_LOG("VLC command: vlc \"", streamUrl, "\"");
    
    // TODO: Implement actual VLC launching
    // This could be:
    // 1. A WebSocket message to the client
    // 2. An HTTP request to a local helper service
    // 3. An OS-level command (system("vlc ..."))
}

void Window::showNotification(const string& message) {
    // Inject JavaScript to show notification in the browser
    if (browser_ && browser_->GetMainFrame()) {
        string js = "document.title = '📺 " + message + "';";
        browser_->GetMainFrame()->ExecuteJavaScript(js, "about:blank", 0);
    }
}
```

### 4. `src/config.cpp` — Add Configuration Options

```cpp
// Add to config parsing
if (key == "vsp-server") {
    globals->config->vspServer = value;
} else if (key == "vsp-profile") {
    globals->config->vspProfile = value;
}
```

### 5. `src/config_defs.hpp` — Define Config Structure

```cpp
struct Config {
    // ... existing config ...
    
    // NEW: VintageStreamProxy settings
    string vspServer = "http://localhost:33000";
    string vspProfile = "default";
    bool vspAutoOpen = true;
};
```

## Communication Challenges

### Server → Client Communication

The main challenge is sending commands from the Browservice server to the iMac G4 client. Options:

#### Option 1: WebSocket (Recommended)

Add WebSocket support to both Browservice and the client:

```cpp
// In window.cpp
void Window::sendVLCCommand(const string& videoId) {
    if (websocketConnection_) {
        json msg = {
            {"type", "vlc_open"},
            {"videoId", videoId},
            {"streamUrl", vspServer_ + "/" + vspProfile_ + "?v=" + videoId}
        };
        websocketConnection_->send(msg.dump());
    }
}
```

Client-side JavaScript (in main.html):

```javascript
// Add WebSocket connection
const ws = new WebSocket('ws://server:port');
ws.onmessage = function(event) {
    const msg = JSON.parse(event.data);
    if (msg.type === 'vlc_open') {
        // Open VLC with the stream URL
        window.location.href = 'vlc://' + msg.streamUrl;
    }
};
```

#### Option 2: HTTP Callback

Browservice makes HTTP request to a local service on the iMac G4:

```cpp
void Window::sendVLCCommand(const string& videoId) {
    string callbackUrl = "http://localhost:8080/vlc?videoId=" + videoId;
    
    // Make HTTP request (using Poco library)
    try {
        Poco::Net::HTTPClientSession session("localhost", 8080);
        Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, 
                                       "/vlc?videoId=" + videoId);
        session.sendRequest(request);
    } catch (exception& e) {
        ERROR_LOG("Failed to send VLC command: ", e.what());
    }
}
```

#### Option 3: OS Command (Simplest but least secure)

```cpp
void Window::sendVLCCommand(const string& videoId) {
    string streamUrl = vspServer_ + "/" + vspProfile_ + "?v=" + videoId;
    string cmd = "vlc \"" + streamUrl + "\" &";
    
    // Execute command (DANGEROUS - only use in trusted environment)
    system(cmd.c_str());
}
```

## Build Instructions

### Prerequisites

```bash
# macOS
brew install cmake poco cef zlib libjpeg-turbo pango freetype2

# Ubuntu/Debian
sudo apt install cmake libpoco-dev libcef-dev zlib1g-dev libjpeg-turbo8-dev libpango1.0-dev libfreetype6-dev
```

### Build

```bash
cd browservice
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Test

```bash
./browservice --vsp-server http://localhost:33000 --vsp-profile default
```

## Security Considerations

1. **Command injection** — Never pass user input directly to `system()`
2. **Network exposure** — Only listen on localhost or trusted network
3. **File scheme blocking** — Keep `--block-file-scheme` enabled
4. **Certificate verification** — Keep TLS verification enabled

## Alternative: JavaScript Injection

Instead of modifying C++, inject JavaScript into the Chromium instance:

```cpp
// In OnAfterCreated or similar
void Window::injectYouTubeDetector() {
    string js = R"(
        // Monitor navigation
        const observer = new MutationObserver(function(mutations) {
            const url = window.location.href;
            if (url.includes('youtube.com/watch') || url.includes('youtu.be/')) {
                const match = url.match(/v=([a-zA-Z0-9_-]{11})/);
                if (match) {
                    // Send message to server
                    fetch('/api/vlc?videoId=' + match[1]);
                }
            }
        });
        
        observer.observe(document, { childList: true, subtree: true });
    )";
    
    browser_->GetMainFrame()->ExecuteJavaScript(js, "about:blank", 0);
}
```

## Status

- [ ] YouTube URL detection
- [ ] Video ID extraction
- [ ] Server → client communication
- [ ] VLC launching
- [ ] Configuration options
- [ ] Error handling
- [ ] Testing on iMac G4

## Contributing

If you implement this modification, please consider submitting a pull request to the Browservice repository. The maintainer has indicated openness to alternative vice plugins, so this could potentially be implemented as a separate plugin rather than a core modification.
