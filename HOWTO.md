# HOWTO — RetroWEBRebirth Setup Guide

Step-by-step instructions for setting up Browservice with YouTube video streaming on an iMac G4.

## What You Need

### Hardware
- **iMac G4** (PowerPC G4, 1024×768 display, OS X 10.5 Leopard)
- **Linux server** (any x86_64 machine — old PC, VM, Raspberry Pi, etc.)
- Both on the same local network (e.g., connected to same router)

### Software
- **iMac G4**: OS X 10.5 Leopard, Python 2.5 (built-in), **PowerVLC** (VLC 3.0.24 port with H.264 AltiVec decode for PowerPC)
- **Linux server**: Ubuntu 24.04 (or similar), Python 3, ffmpeg, yt-dlp

## Step 1: Install PowerVLC on the iMac G4

PowerVLC is a port of VLC 3.0.24 that runs on PowerPC Macs and includes H.264 AltiVec decode — this is what plays modern YouTube streams on the G4.

1. Install PowerVLC (see https://github.com/Olsro/powervlc for the PowerPC build)
2. Verify the binary path:
   ```
   /Applications/PowerVLC.app/Contents/MacOS/PowerVLC
   ```
   The helper launches this binary **directly** with VLC args — `open -a PowerVLC <url>` does NOT forward URLs on OS X 10.5.

## Step 2: Build Browservice on Linux

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install cmake build-essential libx11-dev libxcursor-dev libxext-dev \
  libxft-dev libxi-dev libxrandr-dev libxrender-dev libxss-dev libxtst-dev \
  libglib2.0-dev libgtk2.0-dev libnss3-dev libasound2-dev libpulse-dev \
  libudev-dev libva-dev libvdpau-dev

# Clone and build
git clone https://github.com/wingk1314/RetroWEBRebirth.git browservice
cd browservice
make
```

See `BUILD.md` for the full build process including CEF SDK setup.

## Step 3: Deploy the VSP (Video Stream Proxy)

The VSP streams YouTube videos to the iMac: direct passthrough for ≤360p, DASH remux for 480p/720p.

```bash
# Start VSP (port 33000)
nohup python3 ~/browservice/vsp_replacement.py --port 33000 > /tmp/vsp.log 2>&1 &

# Verify it's running
curl http://SERVER_IP:33000/status
```

The VSP requires `yt-dlp` and `ffmpeg` on the Linux server:

```bash
# Install yt-dlp
sudo pip3 install yt-dlp

# Install ffmpeg
sudo apt install ffmpeg
```

## Step 4: Deploy the Helper on iMac G4

The helper runs on the iMac and launches PowerVLC when Browservice detects a video.

```bash
# Copy helper to iMac (scp fails against old iMac SSH — pipe via ssh)
cat helper_app/helper.py | ssh user@IMAC_IP 'cat > ~/helper.py'

# Start helper
ssh user@IMAC_IP 'nohup python ~/helper.py --port 9999 > /tmp/helper.log 2>&1 &'

# Verify
curl http://IMAC_IP:9999/status
```

The helper is Python 2.5 — keep the source ASCII-only (no fancy unicode in comments).

## Step 5: Start Browservice

```bash
ssh user@SERVER_IP '
cd ~/browservice
nohup ./release/bin/browservice \
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
'
```

`--data-dir` persists cookies, logins, and history across restarts. Omit it for incognito mode.

## Step 6: Use It

1. Open `http://SERVER_IP:8080` in the iMac's browser
2. Navigate to YouTube
3. Find a video and click the **red play button** in the Browservice control bar
4. PowerVLC opens on the iMac and plays the video
5. The **resolution button** in the control bar shows the current mode+resolution (default `L:480p`)

## Changing Mode / Resolution

Click the **resolution button** in the Browservice control bar. It cycles through:

```
L:480p → L:720p → D:144p → D:240p → D:360p → L:480p (wrap)
```

- **D:** = Direct mode — 144p/240p/360p passthrough, zero server CPU
- **L:** = Legacy mode — 480p/720p DASH remux (ffmpeg `-c copy`, no re-encode)

## Helper Commands

```bash
# Check helper status
curl http://IMAC_IP:9999/status

# Check helper logs
ssh user@IMAC_IP 'cat /tmp/helper.log'

# Restart helper
ssh user@IMAC_IP 'killall -9 Python; sleep 1; nohup python ~/helper.py --port 9999 > /tmp/helper.log 2>&1 &'
```

## VSP Commands

```bash
# Check VSP status
curl http://SERVER_IP:33000/status

# Check VSP logs
ssh user@SERVER_IP 'tail -20 /tmp/vsp.log'

# Restart VSP (kill by PID — pkill -f often misses setsid'd processes)
ssh user@SERVER_IP 'kill -9 $(pgrep -f vsp_replacement.py); sleep 1; nohup python3 ~/browservice/vsp_replacement.py --port 33000 > /tmp/vsp.log 2>&1 &'
```

## Troubleshooting

### PowerVLC doesn't open when clicking play

1. Check helper is running: `curl http://IMAC_IP:9999/status`
2. Check helper logs: `ssh user@IMAC_IP 'tail -20 /tmp/helper.log'`
3. Check Browservice can reach the helper: `ssh user@SERVER_IP 'curl -s http://IMAC_IP:9999/status'`

### PowerVLC opens but never plays

The helper must launch the binary directly with VLC args:
```
/Applications/PowerVLC.app/Contents/MacOS/PowerVLC --play-and-exit --no-video-title-show <url>
```
`open -a PowerVLC <url>` does NOT forward the URL on OS X 10.5.

### Timeline shows only a few seconds / scrubbing broken

The VSP is sending the current (partial) file size as Content-Length instead of the estimated final size. Check `/tmp/vsp.log` for `Remux URLs resolved for <id>` (means the size estimation ran). The remux mode must serve `Content-Length: est_size` from `yt-dlp -J` format filesizes.

### Stream keeps dying during first play

This is the remux restart-loop: PowerVLC's repeated Range probes each started a new `ffmpeg -y`, truncating the growing file. The in-progress registry (`remux_procs` keyed by `(video_id, height)`) prevents this — make sure it's in place and never kill the remux on client disconnect.

### Video shows as unreadable garbage (bin_data)

The remux is missing `-bsf:v h264_mp4toannexb` (MP4→TS framing) or yt-dlp picked an AV1 stream. The selector MUST force H.264: `bestvideo[height<=N][ext=mp4][vcodec^=avc1]+bestaudio[ext=m4a]`.

### No red play button in the control bar

`--vlc-enabled=YES` and `--vlc-client-ip` must be set, and Browservice must be rebuilt after any `src/window.cpp` change (`make`, then restart).

### "Connection refused" from the player

The VSP may have stopped. Restart it (see VSP Commands above).

### Browservice won't start

Check if CEF is installed:
```bash
ssh user@SERVER_IP 'ls ~/browservice/release/bin/retrojsvice.so'
```
If missing, run `make` in the browservice directory to rebuild.

## Network Diagram

```
Router (192.168.x.x)
├── iMac G4 (IMAC_IP)
│   ├── Browser → http://SERVER_IP:8080 (Browservice)
│   ├── Helper → port 9999 (launches PowerVLC)
│   └── PowerVLC → http://SERVER_IP:33000 (VSP)
│
└── Linux Server (SERVER_IP)
    ├── Browservice → port 8080 (web proxy)
    ├── VSP → port 33000 (streaming: passthrough + remux)
    └── Xvfb → virtual display for Chromium
```

## File Structure

```
RetroWEBRebirth/
├── helper_app/
│   ├── helper.py              # PowerVLC launcher (runs on iMac, Python 2.5)
│   ├── vsp_replacement.py     # VSP copy (legacy location; live copy is repo root)
│   └── install-login.sh       # Setup script
├── vsp_replacement.py         # Video stream proxy (runs on Linux, port 33000)
├── src/                       # Browservice C++ source
├── README.md                  # Original Browservice README
├── README_RETRO.md            # RetroWEBRebirth overview
├── INTEGRATION.md             # PowerVLC + VSP integration docs
├── MODDING.md                 # C++ modding guide
├── BUILD.md                   # Build instructions
├── HOWTO.md                   # This guide
└── LICENSE                    # MIT License
```
