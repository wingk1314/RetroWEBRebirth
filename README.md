# RetroWEBRebirth — Browservice for iMac G4

A modded version of [Browservice](https://github.com/ttalvitie/browservice) that adds YouTube video streaming to the iMac G4 (OS X 10.5 Leopard, PowerPC G4).

Browservice renders modern web pages (like YouTube) on a Linux server using Chromium, then streams the viewport as images to the old browser on the iMac G4. This mod adds **real video playback** — clicking the red play button in the control bar opens **PowerVLC** on the iMac and plays the video with full-timeline scrubbing.

## What's Included

| Component | Location | What it does |
|-----------|----------|-------------|
| **Browservice** | `src/` | Web proxy server (CEF 144, compiled for Linux) |
| **VSP** | `vsp_replacement.py` | Streaming server (port 33000): direct passthrough + DASH remux |
| **Helper** | `helper_app/helper.py` | Runs on iMac G4, launches PowerVLC when Browservice detects a video |
| **Mode/Resolution Picker** | Browservice control bar | Toggles Direct/Legacy mode + resolution (default: L:480p) |

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  iMac G4 (OS X 10.5 Leopard, 192.168.0.100)            │
│                                                         │
│  Safari ◄── Browservice viewport images                 │
│      │                                                  │
│      ▼ click red play button                            │
│  Helper (port 9999) ──► PowerVLC                        │
│                              │                          │
│                              ▼                          │
│                  http://192.168.0.130:33000             │
└──────────────────────────────────┼──────────────────────┘
                                   │
┌──────────────────────────────────┼──────────────────────┐
│  Linux Server (192.168.0.130)                           │
│                                                         │
│  Browservice (port 8080) ◄── iMac browser               │
│      │                                                  │
│      ▼ stream URL (/direct or /profile)                 │
│  VSP (port 33000)                                       │
│      │  yt-dlp → YouTube CDN (Range proxy)              │
│      ▼  or ffmpeg remux (DASH → MPEG-TS, -c copy)       │
│  PowerVLC streams with full scrubbing                   │
└─────────────────────────────────────────────────────────┘
```

## How Video Streaming Works

1. User browses YouTube via Browservice on the iMac G4
2. Clicks the **red play button** in the Browservice control bar
3. Browservice builds a stream URL and sends it to the **Helper** (port 9999)
4. Helper launches **PowerVLC** on the iMac (binary + `--play-and-exit`)
5. PowerVLC connects to the **VSP** (port 33000) on the Linux server
6. VSP routes based on the URL:
   - `/direct?v=<id>&height=<res>` — **Direct mode** (144p/240p/360p): yt-dlp resolves YouTube's progressive format 18 (360p H.264+AAC), VSP Range-forwards the CDN stream — **no server CPU**
   - `/<profile>?v=<id>&resolution=<res>` — **Legacy mode** (480p/720p): yt-dlp fetches DASH H.264+AAC streams, ffmpeg `-c copy` muxes them to MPEG-TS (mux, not re-encode)
7. VSP serves with **estimated Content-Length** (from `yt-dlp -J` format sizes), enabling the full timeline
8. PowerVLC plays the video with **true scrubbing on first play** (HTTP 206 Range at any position)

## Streaming Modes

| Mode | Display | Route | Resolutions | Server CPU |
|------|---------|-------|-------------|------------|
| **Direct** | `D:144p`…`D:360p` | `/direct` | 144p / 240p / 360p | ~0% (CDN Range passthrough) |
| **Legacy** | `L:480p` / `L:720p` | `/<profile>` | 480p / 720p | ~1% (DASH remux, `-c copy`) |

- **Default: L:480p**
- The resolution button in the control bar cycles: `L:480p → L:720p → D:144p → D:240p → D:360p → L:480p (wrap)`
- Direct mode ≤360p streams YouTube's progressive format 18 with zero transcoding.
- Legacy mode 480p/720p uses ffmpeg `-c copy` (no re-encode): `bestvideo[height<=N][ext=mp4][vcodec^=avc1]+bestaudio[ext=m4a]` → `-bsf:v h264_mp4toannexb -f mpegts`.

## Optional: VNC front end (rfbvice)

Instead of streaming full frames to Safari, Browservice can serve the browser over VNC with the `rfbvice` plugin. It sends only the screen tiles that changed, turns scrolling into a cheap CopyRect, and keeps one open connection. On the iMac, use Leopard's Screen Sharing (`vnc://SERVER_IP:5900`) or Chicken of the VNC. Build it with `make rfbvice` and see [viceplugins/rfbvice/README.md](viceplugins/rfbvice/README.md).

## Setup

### Prerequisites

- **Linux server** (Ubuntu 24.04 tested) with Python 3, ffmpeg, yt-dlp
- **iMac G4** running OS X 10.5 Leopard with Python 2.5 and **PowerVLC** (VLC 3.0.24 port with H.264 AltiVec decode)
- Both machines on the same local network

### 1. Build Browservice on Linux

```bash
# On the Linux server
cd ~/browservice
make
```

See `BUILD.md` for detailed build instructions.

### 2. Deploy the VSP

```bash
# Start VSP (port 33000)
nohup python3 ~/browservice/vsp_replacement.py --port 33000 > /tmp/vsp.log 2>&1 &

# Verify
curl http://127.0.0.1:33000/status   # → {'profile': '480p', 'streams': 0, 'cached': 0}
```

### 3. Deploy the Helper on iMac G4

```bash
# Copy helper to iMac (scp fails against old iMac SSH — pipe via ssh)
cat helper_app/helper.py | ssh imac-g4 'cat > /path/helper.py'

# Start helper
ssh imac-g4 'nohup python /path/helper.py --port 9999 > /tmp/helper.log 2>&1 &'
```

The helper must be Python 2.5 compatible (ASCII-only source, print statements).

### 4. Start Browservice

```bash
# On the Linux server (or use start_browservice.sh)
cd ~/browservice
./release/bin/browservice \
  --vlc-enabled=YES \
  --vlc-client-ip=IMAC_IP \
  --vlc-helper-port=9999 \
  --vsp-server=http://SERVER_IP:33000 \
  --vsp-profile=default \
  --vlc-timeout=300 \
  --data-dir=/home/rk/browservice/data \
  --start-page=https://www.google.com \
  --vice-opt-http-listen-addr=0.0.0.0:8080 \
  > /tmp/browservice.log 2>&1 &
```

### 5. Access from iMac G4

Open `http://SERVER_IP:8080` in the iMac's browser.

- Navigate to YouTube, find a video
- Click the **red play button** in the control bar
- PowerVLC opens and plays the video with full-timeline scrubbing
- The **resolution button** (showing `L:480p` by default) cycles mode + resolution

## VSP Endpoints

| Endpoint | Purpose |
|----------|---------|
| `/` or `/resolution` | Resolution picker page (legacy UI) |
| `/set/<profile>` | Set active profile |
| `/status` | JSON status: profile, active streams, cached count |
| `/direct?v=<id>&height=<res>` | Direct passthrough (≤360p) / DASH remux (>360p) |
| `/<profile>?v=<id>&resolution=<res>` | Legacy ffmpeg transcode (MPlayer fallback) |

## Key Technical Decisions

### Why Direct passthrough for ≤360p?

YouTube's **format 18 is the only progressive format** (360p H.264+AAC, audio+video in one URL). Its CDN natively supports HTTP Range (206, Accept-Ranges, full Content-Length), so PowerVLC gets **true scrubbing on first play with zero transcoding** — no ffmpeg, no server CPU.

### Why remux (not transcode) for 480p/720p?

Everything above 360p on YouTube is split DASH (separate video + audio streams) that needs muxing. ffmpeg `-c copy` combines them **without re-encoding** (~1% CPU vs 36%+ for real transcoding).

### Why estimated Content-Length?

The player derives duration from Content-Length. For remux streams the file grows while playing, so VSP advertises the **estimated final size** (from `yt-dlp -J` format filesizes + ~5% TS overhead) — VLC shows the full timeline and Range requests at any position are honored as the remux catches up.

### Why PowerVLC instead of MPlayer?

PowerVLC is a VLC 3.0.24 port with H.264 AltiVec decode for PowerPC — it plays the modern H.264 streams directly. MPlayer OSX Extended remains as a fallback in the helper (legacy transcode mode uses MPEG-2).

## Troubleshooting

| Problem | Fix |
|---------|-----|
| PowerVLC opens but plays nothing | Helper must launch the binary directly with VLC args (`--play-and-exit --no-video-title-show`); `open -a` doesn't forward URLs on 10.5 |
| Timeline shows only a few seconds | VSP sent current file size as Content-Length — must use estimated final size (check `/tmp/vsp.log` for `Remux URLs resolved`) |
| Stream dies during first play | Remux restart-loop: check for multiple `Starting remux for <id>` lines; the in-progress registry prevents this |
| Video shows as unreadable (bin_data) | Remux missing `-bsf:v h264_mp4toannexb` or picked AV1 (needs `[vcodec^=avc1]`) |
| "Connection refused" from player | VSP may have stopped — restart it |
| No red play button | `--vlc-enabled=YES` and `--vlc-client-ip` must be set; rebuild after any `src/window.cpp` change |

## Credits

- [Browservice](https://github.com/ttalvitie/browservice) by Topi Talvitie — MIT License
- [PowerVLC](https://github.com/Olsro/powervlc) — VLC 3.0.24 port for PowerPC (H.264 AltiVec decode)
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) — YouTube stream extraction
- [ffmpeg](https://ffmpeg.org/) — DASH remuxing

## License

MIT License — see [LICENSE](LICENSE) for details.
