# Modding Browservice for PowerVLC + VSP Integration

This document describes how Browservice's C++ code was modified to automatically detect YouTube video URLs and trigger PowerVLC playback through the VSP (Vintage Stream Proxy replacement) server.

## Architecture Overview

### Current Flow

```
User clicks YouTube link → CEF OnBeforeBrowse() → detect video ID
  → build /direct stream URL → open "Opening in VLC..." popup
  → helper on iMac (port 9999) → PowerVLC launches with stream URL
  → PowerVLC → VSP (port 33000) → YouTube CDN (direct, no transcode)
```

### Key Design Points

- **PowerVLC is the player**, launched directly by the iMac helper with `--play-and-exit --no-video-title-show <url>` (the `open -a` mechanism does not forward URLs on OS X 10.5).
- **VSP resolves and streams the video** in `/direct` mode: passthrough for ≤360p (YouTube progressive format 18), remux of DASH H.264+AAC streams for 480p/720p. No re-encoding.
- **Full timeline scrubbing** comes from serving the *estimated final* Content-Length (from `yt-dlp -J` format filesizes), not the current growing-file size.

## Key Files to Modify

### 1. `src/window.cpp` — YouTube Detection & Stream URL Building

**YouTube URL detection** (`isYouTubeVideoURL` + `extractYouTubeVideoID`):

```cpp
// Detect YouTube video URLs. Works with www./m. prefixes and case-insensitively.
bool isYouTubeVideoURL(string url) {
    string lower = toLowerASCII(url);
    // matches youtube.com/watch, youtu.be/, m.youtube.com, etc.
}

optional<string> extractYouTubeVideoID(string url);
```

**Mode + resolution selector** (module-level state):

```cpp
// Direct mode: passthrough only (144p/240p/360p via format 18, no server work)
const vector<string> directResolutions = {"144p", "240p", "360p"};
// Legacy mode: remux via /profile (480p/720p, ffmpeg DASH muxing)
const vector<string> legacyResolutions = {"480p", "720p"};

enum StreamMode { StreamDirect, StreamLegacy };
StreamMode currentStreamMode = StreamLegacy;  // Default: L:480p
int currentResIdx = 0; // Index within the current mode's resolution list

string getCurrentTranscodeResolution() {
    const auto& res = (currentStreamMode == StreamDirect) ? directResolutions : legacyResolutions;
    return res[currentResIdx];
}

// Display text: "D:360p" (direct) or "L:480p" (legacy)
string getResolutionDisplayText() {
    string prefix = (currentStreamMode == StreamDirect) ? "D:" : "L:";
    return prefix + getCurrentTranscodeResolution();
}

// Cycle: D:144p -> D:240p -> D:360p -> L:480p -> L:720p -> D:144p (wrap)
void cycleTranscodeResolution() {
    const auto& res = (currentStreamMode == StreamDirect) ? directResolutions : legacyResolutions;
    currentResIdx++;
    if(currentResIdx >= (int)res.size()) {
        currentResIdx = 0;
        currentStreamMode = (currentStreamMode == StreamDirect) ? StreamLegacy : StreamDirect;
        INFO_LOG("Stream mode changed to ",
                 (currentStreamMode == StreamDirect ? "Direct" : "Legacy"));
    }
    INFO_LOG("Resolution changed to ", getResolutionDisplayText());
}
```

**⚠ There are TWO URL-building sites in `window.cpp`** — the red-play-button path (~line 656) AND the manual VLC launch path (~line 1429). Patching only one means videos still hit the legacy transcode path (symptoms: VSP log shows ffmpeg/mpeg2video; PowerVLC crashes on the transcoded stream). Grep for `streamUrl = vspServer` — both must emit `/direct?v=...&height=...` for YouTube IDs.

**Red play button path** (in the navigation/video-detection handler):

```cpp
if(globals->config->vlcEnabled && isYouTubeVideoURL(url)) {
    optional<string> videoID = extractYouTubeVideoID(url);
    if(videoID.has_value()) {
        if(globals->config->vlcClientIP.empty()) {
            WARNING_LOG("VLC enabled but --vlc-client-ip not set, ignoring YouTube URL: ", url);
            return false;
        }

        // Build VSP stream URL (direct passthrough mode for PowerVLC)
        string vspServer = globals->config->vspServer;
        if(!vspServer.empty() && vspServer.back() == '/') {
            vspServer.pop_back();
        }
        string resolution = getCurrentTranscodeResolution();
        string streamUrl = vspServer + "/direct"
                         + "?v=" + videoID.value()
                         + "&height=" + resolution;
        string encodedStreamUrl = CefURIEncode(streamUrl, false);

        // Build helper URL with timeout
        string helperUrl = "http://" + globals->config->vlcClientIP + ":"
                         + std::to_string(globals->config->vlcHelperPort)
                         + "/vlc?streamUrl=" + encodedStreamUrl
                         + "&timeout=" + std::to_string(globals->config->vlcTimeout);
        // ... create "Opening in VLC..." popup window, send the request
    }
}
```

**Manual VLC launch path** (Ctrl+Shift+V / control-bar button):

```cpp
string vspServer = globals->config->vspServer;
if(vspServer.empty()) {
    WARNING_LOG("VSP server not configured (--vsp-server)");
    return;
}
if(vspServer.back() == '/') {
    vspServer.pop_back();
}

string streamUrl;
string resolution = getCurrentTranscodeResolution();
optional<string> videoID = extractYouTubeVideoID(url);
if(videoID.has_value()) {
    streamUrl = vspServer + "/direct"
              + "?v=" + videoID.value()
              + "&height=" + resolution;
    INFO_LOG("Manual VLC: YouTube video ", videoID.value(), " at ", resolution);
} else {
    // Pass the full URL to VSP for non-YouTube sites (legacy transcode)
    streamUrl = vspServer + "/" + globals->config->vspProfile
              + "?url=" + string(CefURIEncode(url, false).ToString())
              + "&resolution=" + resolution;
    INFO_LOG("Manual VLC: generic URL ", url, " at ", resolution);
}

string encodedStreamUrl = CefURIEncode(streamUrl, false);

// Build helper URL
string helperUrl = "http://" + globals->config->vlcClientIP + ":"
                 + std::to_string(globals->config->vlcHelperPort)
                 + "/vlc?streamUrl=" + encodedStreamUrl
                 + "&timeout=" + std::to_string(globals->config->vlcTimeout);
```

**⚠ KEY FIX (B1)**: The "Opening in VLC..." notification must be a *new popup window* created with `createSignedDataURL(...)` passed directly to `tryCreate` as the initial URI. `navigateToURI` fails because `browser_` is null until `OnAfterCreated` fires (async).

### 2. `src/config_defs.hpp` — New Configuration Options

Added to `CONF_FOREACH_OPT`:

```cpp
CONF_FOREACH_OPT_ITEM(vlcEnabled)     // --vlc-enabled=YES/NO
CONF_FOREACH_OPT_ITEM(vlcClientIP)    // --vlc-client-ip=192.168.0.100
CONF_FOREACH_OPT_ITEM(vlcHelperPort)  // --vlc-helper-port=9999
CONF_FOREACH_OPT_ITEM(vspServer)      // --vsp-server=http://192.168.0.130:33000
CONF_FOREACH_OPT_ITEM(vspProfile)     // --vsp-profile=default
CONF_FOREACH_OPT_ITEM(vlcTimeout)     // --vlc-timeout=300
```

### 3. iMac Helper (`BrowserviceHelper.app/.../helper.py`)

Python 2.5 script on port 9999. Receives `/vlc?streamUrl=...&timeout=...` and launches PowerVLC:

```python
# NOTE: Python 2.5 — ASCII-only source (no em-dashes in comments),
# print statements, `except Exception, e:` syntax.
# Verify: python -c "import py_compile; py_compile.compile('helper.py', doraise=True)"

subprocess.Popen([
    "/Applications/PowerVLC.app/Contents/MacOS/PowerVLC",
    "--play-and-exit",
    "--no-video-title-show",
    stream_url,
])
```

**⚠ Do NOT use `open -a PowerVLC <url>`** — OS X 10.5 does not forward the URL argument to the app. The binary must be launched directly.

### 4. VSP Server (`vsp_replacement.py`)

Python 3 streaming server on port 33000:

- **`/direct?v=<id>&height=<res>`** — PowerVLC mode. `height > 360` → remux (DASH muxing); `height <= 360` → Range-forwarding passthrough of progressive format 18.
- **`/<profile>?v=<id>&resolution=<res>`** — legacy ffmpeg transcode to MPEG-TS (MPlayer fallback).
- Serves with Range support and **estimated final Content-Length** for scrubbing.
- In-progress remux registry (`remux_procs` keyed by `(video_id, height)`) prevents restart loops from PowerVLC's repeated Range probes.

Key VSP internals (remux mode):

- yt-dlp selector MUST force H.264: `bestvideo[height<=N][ext=mp4][vcodec^=avc1]+bestaudio[ext=m4a]` — without `[vcodec^=avc1]`, yt-dlp picks AV1 which breaks the Annex B filter.
- ffmpeg: `ffmpeg -y -i <vurl> -i <aurl> -c copy -bsf:v h264_mp4toannexb -f mpegts -max_muxing_queue_size 1024 <out>.ts` — `-bsf:v h264_mp4toannexb` is REQUIRED (MP4 length-prefixed framing → MPEG-TS start-code framing).

## Build Instructions

```bash
# On the Linux box, after any src/*.cpp change:
cd ~/browservice
make -j4
# Restart the service (kills any active stream — warn the user first!)
sudo systemctl restart browservice
```

## Communication Flow

Browservice never talks to PowerVLC directly. It talks to the **helper** on the iMac over HTTP; the helper launches PowerVLC; PowerVLC talks to **VSP** over HTTP Range requests.

```
Browservice (Linux) ──HTTP──> helper (iMac, :9999) ──launch──> PowerVLC (iMac)
PowerVLC ──HTTP Range──> VSP (Linux, :33000) ──HTTP 206──> YouTube CDN
```

## Gotchas (all hit in production)

1. **TWO URL-building sites in `src/window.cpp`** — red play button (~line 656) AND manual VLC launch (~line 1429). Both must emit `/direct`.
2. **Default mode+resolution**: `window.cpp` initializes `currentStreamMode = StreamLegacy` + `currentResIdx = 0` (→ L:480p); `vsp_replacement.py` `DEFAULT_PROFILE = "480p"` is the fallback for legacy routes. The control-bar button cycles the full set: L:480p → L:720p → D:144p → D:240p → D:360p.
3. **`open -a PowerVLC <url>` opens but never plays** on OS X 10.5 — launch the binary directly with VLC args.
4. **Helper is Python 2.5**: ASCII-only source, print statements, `except Exception, e:`.
5. **`pkill -f vsp_replacement.py` often does NOT kill it** (setsid'd from ssh) — kill by explicit PID.
6. **Remux HTTP 500 "Remux produced no data"** — AV1 stream selected (fix: `[vcodec^=avc1]`) or missing `-bsf:v h264_mp4toannexb`.
7. **Timeline shows only a few seconds** — VSP sent current growing-file size as Content-Length; must send estimated final size.
8. **Stream dies during first play** — remux restart loop; fixed by the `remux_procs` registry; never `proc.terminate()` on client disconnect.
9. **Restarting VSP kills the active stream** — warn the user before deploying mid-session.

## Status

- [x] YouTube URL detection
- [x] Video ID extraction
- [x] Server → client communication (HTTP helper on iMac)
- [x] PowerVLC launching (direct binary + args)
- [x] Configuration options (--vlc-*, --vsp-*)
- [x] Direct passthrough streaming (format 18, Range)
- [x] Remux mode (480p/720p DASH muxing)
- [x] Full-timeline scrubbing (estimated Content-Length)
- [x] Persistent sessions (--data-dir)
- [x] Tested on iMac G4

## Contributing

The streaming integration is implemented as a set of modifications to the open-source [Browservice](https://github.com/ttalvitie/browservice) codebase, plus the standalone `vsp_replacement.py` server and the iMac `helper.py`. The upstream maintainer has indicated openness to alternative vice plugins; a plugin-based approach could replace the C++ modifications in the future.
