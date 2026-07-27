# RetroREBIRTH — Browservice for iMac G4

A modded version of [Browservice](https://github.com/ttalvitie/browservice) that adds YouTube video streaming to the iMac G4 (OS X 10.5 Leopard, PowerPC G4).

Browservice renders modern web pages (like YouTube) on a Linux server using Chromium, then streams the viewport as images to the old browser on the iMac G4. This mod adds **real video playback** — clicking a YouTube video opens MPlayer on the iMac and plays the video with seeking support.

## What's Included

| Component | Location | What it does |
|-----------|----------|-------------|
| **Browservice** | `src/` | Web proxy server (CEF 144, compiled for Linux) |
| **VSP** | `helper_app/vsp_replacement.py` | Transcodes YouTube videos to MPEG-TS via yt-dlp + ffmpeg |
| **Helper** | `helper_app/helper.py` | Runs on iMac G4, launches MPlayer when Browservice detects a video |
| **Resolution Picker** | Built into VSP | Web UI to change streaming quality (720p/480p/360p/240p/144p) |

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  iMac G4 (OS X 10.5 Leopard)                           │
│                                                         │
│  Safari/IE ◄── Browservice viewport images              │
│      │                                                  │
│      ▼ click red play button                            │
│  Helper (port 9999) ──► MPlayer OSX Extended            │
│                              │                          │
│                              ▼                          │
│                     http://SERVER_IP:33000          │
└──────────────────────────────────────┼──────────────────┘
                                       │
┌──────────────────────────────────────┼──────────────────┐
│  Linux Server (SERVER_IP)                           │
│                                                         │
│  Browservice (port 8080) ◄── iMac browser               │
│      │                                                  │
│      ▼ VLC helper URL                                   │
│  VSP (port 33000)                                       │
│      │  yt-dlp → ffmpeg → MPEG-TS                       │
│      ▼                                                  │
│  MPlayer on iMac streams from VSP                       │
└─────────────────────────────────────────────────────────┘
```

## How Video Streaming Works

1. User browses YouTube via Browservice on the iMac G4
2. Clicks the **red play button** in the Browservice control bar
3. Browservice sends the video URL to the **Helper** (port 9999)
4. Helper launches **MPlayer OSX Extended** on the iMac
5. MPlayer connects to the **VSP** (port 33000) on the Linux server
6. VSP uses **yt-dlp** to extract stream URLs, then **ffmpeg** to transcode to **MPEG-TS** (mpeg2video + mp2, G4-friendly codecs)
7. VSP serves the TS file with a **fake Content-Length** (estimated from duration × bitrate), enabling MPlayer's seek bar
8. MPlayer plays the video with seeking support

## Setup

### Prerequisites

- **Linux server** (Ubuntu 24.04 tested) with Python 3, ffmpeg, yt-dlp
- **iMac G4** running OS X 10.5 Leopard with Python 2.5
- **MPlayer OSX Extended** installed on the iMac G4
- Both machines on the same local network

### 1. Build Browservice on Linux

```bash
# On the Linux server (SERVER_IP)
cd ~/browservice
make
```

See `BUILD.md` for detailed build instructions.

### 2. Deploy the VSP

```bash
# Copy VSP to Linux server
scp helper_app/vsp_replacement.py user@SERVER_IP:~/browservice/vsp_replacement.py

# Start VSP
ssh user@SERVER_IP 'nohup python3 ~/browservice/vsp_replacement.py --port 33000 --profile 720p > /tmp/vsp.log 2>&1 &'
```

### 3. Deploy the Helper on iMac G4

```bash
# Copy helper to iMac
scp helper_app/helper.py user@IMAC_IP:~/helper.py

# Start helper
ssh user@IMAC_IP 'nohup python ~/helper.py --port 9999 > /tmp/helper.log 2>&1 &'
```

### 4. Start Browservice

```bash
# On the Linux server
cd ~/browservice
nohup ./release/bin/browservice \
  --vlc-enabled=YES \
  --vlc-client-ip=IMAC_IP \
  --vlc-helper-port=9999 \
  --vsp-server=http://SERVER_IP:33000 \
  --vsp-profile=default \
  --vlc-timeout=300 \
  --start-page=https://www.google.com \
  --vice-opt-http-listen-addr=0.0.0.0:8080 \
  > /tmp/browservice.log 2>&1 &
```

### 5. Access from iMac G4

Open `http://SERVER_IP:8080` in the iMac's browser.

- **Resolution picker** loads first — select your quality (default: 720p)
- Navigate to YouTube, find a video, click the **red play button** in the control bar
- MPlayer opens and plays the video

## Streaming Profiles

| Profile | Resolution | Video Bitrate | Audio |
|---------|-----------|---------------|-------|
| 720p | 1280×720 | 2500k | 128k mp2 |
| 480p | 640×480 | 1000k | 128k mp2 |
| 360p | 480×360 | 500k | 96k mp2 |
| 240p | 320×240 | 200k | 64k mp2 |
| 144p | 256×144 | 100k | 32k mp2 |

All profiles output **MPEG-TS** with **mpeg2video** (video) and **mp2** (audio) — codecs the G4 can decode natively.

## Key Technical Decisions

### Why MPEG-TS instead of MP4?

MP4 requires a moov atom that's only written when ffmpeg finishes encoding. No player can seek in a growing MP4. MPEG-TS writes program stream maps periodically, so players can parse the stream format immediately.

### Why fake Content-Length?

MPlayer needs a `Content-Length` header to enable its seek bar. Since the file is growing during transcoding, we predict the final size from `duration × bitrate` and report that as Content-Length. MPlayer thinks the file is complete and shows a seek bar. Seeking works by requesting byte ranges within the already-written portion of the file.

### Why mpeg2video + mp2?

The iMac G4 has no hardware H.264 decoder. MPEG-2 Video and MP2 audio are the most efficient codecs the G4 can decode in software at 720p.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| MPlayer doesn't open | Check helper is running: `ssh user@IMAC_IP 'cat /tmp/helper.log'` |
| Video plays but no seek bar | VSP may not have finished transcoding — wait 10-30s |
| Forward seek ends video | The video is short — try a longer video |
| "Connection refused" from MPlayer | VSP may have stopped — restart it |
| Black screen in MPlayer | Kill MPlayer and try again — display connection may have failed |

## Credits

- [Browservice](https://github.com/ttalvitie/browservice) by Topi Talvitie — MIT License
- [VintageStreamProxy](https://github.com/nickoala/vintage-stream-proxy) — original VSP concept
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) — YouTube stream extraction
- [ffmpeg](https://ffmpeg.org/) — video transcoding
- [MPlayer OSX Extended](https://mplayerosxext.sourceforge.net/) — media player for OS X

## License

MIT License — see [LICENSE](LICENSE) for details.
