# Browservice + PowerVLC Integration

Watch YouTube videos on your iMac G4 by combining Browservice (for browsing) with PowerVLC (for video playback) and VSP (Vintage Stream Proxy replacement) on the Linux box.

## Architecture

```
iMac G4 (192.168.0.100, Mac OS X Leopard 10.5.9, user rk)
  Safari → Browservice viewport images (http://192.168.0.130:8080)
  red play button → helper (port 9999 on iMac) → PowerVLC
  PowerVLC → VSP (port 33000 on Linux box) → YouTube CDN (direct, no transcode)
```

Components:

- **Browservice** (Linux box, `~/browservice`): CEF-based proxy that renders the web into viewport images; runs as systemd service `browservice.service` with AMD hardware acceleration.
- **Helper** (iMac G4): `/Applications/BrowserviceHelper.app/Contents/Resources/helper.py` — Python 2.5 script, listens on port 9999, receives `/vlc?streamUrl=...` from Browservice and launches PowerVLC.
- **VSP** (Linux box, `~/browservice/vsp_replacement.py`): streaming server on port 33000. Handles `/direct` (PowerVLC) and legacy profile routes.

## Streaming Modes

### Direct passthrough (PowerVLC — the default)

Route: `/direct?v=<id>&height=<res>`

- **`height > 360` → remux mode**: yt-dlp fetches DASH streams (H.264 video + AAC audio), `ffmpeg -c copy` muxes them to MPEG-TS. This is a **mux, not a transcode** (~1% CPU vs 36%+ for real transcoding).
- **`height <= 360` → passthrough**: Range-forwarding of YouTube's progressive format 18 (360p, audio+video in one URL). No transcoding at all.
- **True scrubbing on first play**: YouTube CDN supports HTTP Range; VSP resolves URLs with `yt-dlp -J` and advertises the **estimated final size** as Content-Length, so PowerVLC shows the full timeline immediately. No waiting for the stream to finish.

### Legacy transcode (MPlayer fallback)

Route: `/<profile>?v=<id>&resolution=<res>`

- ffmpeg transcodes to MPEG-TS with the profile's codec settings. Used for non-YouTube URLs and as a fallback path; much heavier on CPU.

## Setup

### 1. Linux box (server)

```bash
# Build browservice (see BUILD.md), then:
cd ~/browservice

# VSP server (port 33000)
setsid nohup python3 vsp_replacement.py --port 33000 > /tmp/vsp.log 2>&1 < /dev/null &

# Browservice with all streaming options (see start_browservice.sh)
bash start_browservice.sh
```

The launcher enables the integration with:

```bash
--vlc-enabled=YES \
--vlc-client-ip=192.168.0.100 \
--vlc-helper-port=9999 \
--vsp-server=http://192.168.0.130:33000 \
--vsp-profile=default \
--vlc-timeout=300 \
--data-dir=/home/rk/browservice/data   # persists cookies/history/cache across restarts
```

### 2. iMac G4 (client)

1. Install PowerVLC.
2. Install the helper app (`BrowserviceHelper.app`) — `helper.py` listens on port 9999.

## Usage

1. Open `http://192.168.0.130:8080` in Safari on the iMac G4.
2. Browse to a YouTube video.
3. Click the **red play button** in the control bar (or press `Ctrl+Shift+V` for the manual launch path).
4. An "Opening in VLC..." popup appears; PowerVLC launches with the stream URL and plays it with a fully scrubbable timeline.

## Resolution Selection

Default: **480p**. The control bar includes a resolution selector cycling through `144p / 240p / 360p / 480p / 720p`. Higher resolutions use the remux path; 360p and below use direct passthrough of the progressive stream.

## Configuration Options (Browservice)

| Option | Default | Purpose |
|--------|---------|---------|
| `--vlc-enabled` | NO | Enable YouTube → VLC detection |
| `--vlc-client-ip` | (empty) | IP of the iMac helper (required for detection) |
| `--vlc-helper-port` | 9999 | Port of the iMac helper |
| `--vsp-server` | (empty) | VSP base URL, e.g. `http://192.168.0.130:33000` |
| `--vsp-profile` | default | Legacy transcode profile name |
| `--vlc-timeout` | 300 | Seconds before the helper gives up on the stream |
| `--data-dir` | (empty) | Persistent CEF data dir; empty = incognito (no cookies/history) |

## Troubleshooting

- **PowerVLC opens but never plays**: make sure the helper launches the binary directly (`/Applications/PowerVLC.app/Contents/MacOS/PowerVLC --play-and-exit --no-video-title-show <url>`). `open -a PowerVLC <url>` does NOT forward the URL on OS X 10.5.
- **Timeline shows only a few seconds / no scrubbing**: the VSP is serving the current partial file size as Content-Length instead of the estimated final size. Check `/tmp/vsp.log` and the remux mode docs.
- **Stream keeps dying during first play**: the remux is being restarted on every Range probe. The in-progress remux registry (`remux_procs`) prevents this — make sure it's in place; restarting VSP kills the active stream.
- **No red play button**: `--vlc-enabled=YES` and `--vlc-client-ip` must be set, and the binary rebuilt/restarted after any `src/window.cpp` change.

See `MODDING.md` for the C++ implementation details.
