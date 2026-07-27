# HOWTO — RetroREBIRTH Setup Guide

Step-by-step instructions for setting up Browservice with YouTube video streaming on an iMac G4.

## What You Need

### Hardware
- **iMac G4** (PowerPC G4, 1024×768 display)
- **Linux server** (any x86_64 machine — old PC, VM, Raspberry Pi, etc.)
- Both on the same local network (e.g., connected to same router)

### Software
- **iMac G4**: OS X 10.5 Leopard, Python 2.5 (built-in), MPlayer OSX Extended
- **Linux server**: Ubuntu 24.04 (or similar), Python 3, ffmpeg, yt-dlp

## Step 1: Install MPlayer on the iMac G4

Download MPlayer OSX Extended (last PPC-compatible version) and install it:

1. Download `MPlayer_OSX_Extended.dmg` (rev14, 2011 — last PPC build)
2. Mount the DMG
3. Drag `MPlayer OSX Extended.app` to `/Applications/`
4. The app contains the core mplayer binary at:
   ```
   /Applications/MPlayer OSX Extended.app/Contents/Resources/Binaries/
   mpextended.mpBinaries/Contents/mpextended.mpBinaries/Contents/MacOS/mplayer
   ```

## Step 2: Build Browservice on Linux

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install cmake build-essential libx11-dev libxcursor-dev libxext-dev \
  libxft-dev libxi-dev libxrandr-dev libxrender-dev libxss-dev libxtst-dev \
  libglib2.0-dev libgtk2.0-dev libnss3-dev libasound2-dev libpulse-dev \
  libudev-dev libva-dev libvdpau-dev

# Clone and build
cd ~/browservice
git clone https://github.com/wingk1314/RetroREBIRTH.git browservice
cd browservice
make
```

See `BUILD.md` for the full build process including CEF SDK setup.

## Step 3: Deploy the VSP (Video Stream Proxy)

The VSP transcodes YouTube videos to G4-compatible formats on the Linux server.

```bash
# Copy VSP to Linux server
scp helper_app/vsp_replacement.py user@SERVER_IP:~/browservice/vsp_replacement.py

# Start VSP
ssh user@SERVER_IP 'nohup python3 ~/browservice/vsp_replacement.py \
  --port 33000 --profile 720p > /tmp/vsp.log 2>&1 &'

# Verify it's running
curl http://SERVER_IP:33000/status
```

The VSP requires `yt-dlp` and `ffmpeg` to be installed on the Linux server:

```bash
# Install yt-dlp
sudo pip3 install yt-dlp

# Install ffmpeg
sudo apt install ffmpeg
```

## Step 4: Deploy the Helper on iMac G4

The helper runs on the iMac and launches MPlayer when Browservice detects a video.

```bash
# Copy helper to iMac
scp helper_app/helper.py user@IMAC_IP:~/helper.py

# Start helper
ssh user@IMAC_IP 'nohup python ~/helper.py --port 9999 > /tmp/helper.log 2>&1 &'

# Verify
curl http://IMAC_IP:9999/status
```

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
  --start-page=https://www.google.com \
  --vice-opt-http-listen-addr=0.0.0.0:8080 \
  > /tmp/browservice.log 2>&1 &
'
```

## Step 6: Use It

1. Open `http://SERVER_IP:8080` in the iMac's browser
2. You'll see a **resolution picker** — choose your quality (default: 720p)
3. Navigate to YouTube
4. Find a video and click the **red play button** in the Browservice control bar
5. MPlayer opens on the iMac and plays the video

## Changing Resolution

Click the **Home** button in the Browservice control bar to return to the resolution picker. Select a different quality. The next video you play will use the new resolution.

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

# Restart VSP
ssh user@SERVER_IP 'killall -9 python3 ffmpeg; sleep 1; nohup python3 ~/browservice/vsp_replacement.py --port 33000 --profile 720p > /tmp/vsp.log 2>&1 &'
```

## Troubleshooting

### MPlayer doesn't open when clicking play

1. Check helper is running: `curl http://IMAC_IP:9999/status`
2. Check helper logs: `ssh user@IMAC_IP 'tail -20 /tmp/helper.log'`
3. Check Browservice can reach the helper: `ssh user@SERVER_IP 'curl -s http://IMAC_IP:9999/status'`

### Video plays but no sound

MPlayer uses CoreAudio output by default. If no sound:
- Check System Preferences → Sound → Output is set to built-in speakers
- Try running MPlayer manually: `open -a "MPlayer OSX Extended" http://SERVER_IP:33000/cache?v=VIDEO_ID`

### Forward seek ends the video

This is normal for short videos. The video only has a few seconds of content. Try a longer video (5+ minutes).

### "Connection refused" from MPlayer

The VSP may have stopped. Restart it:
```bash
ssh user@SERVER_IP 'nohup python3 ~/browservice/vsp_replacement.py --port 33000 --profile 720p > /tmp/vsp.log 2>&1 &'
```

### Black screen in MPlayer

Kill MPlayer and try again. The display connection may have failed on launch:
```bash
ssh user@IMAC_IP 'killall -9 mplayer'
```
Then click play again.

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
│   ├── Helper → port 9999 (launches MPlayer)
│   └── MPlayer → http://SERVER_IP:33000 (VSP)
│
└── Linux Server (SERVER_IP)
    ├── Browservice → port 8080 (web proxy)
    ├── VSP → port 33000 (video transcoding)
    └── Xvfb → virtual display for Chromium
```

## File Structure

```
RetroREBIRTH/
├── helper_app/
│   ├── helper.py              # MPlayer launcher (runs on iMac)
│   ├── vsp_replacement.py     # Video stream proxy (runs on Linux)
│   ├── BrowserviceHelper.app/ # Original helper (unused)
│   └── install-login.sh       # Setup script
├── src/                       # Browservice C++ source
├── vlc-launcher.html          # VLC launcher page
├── vlc-youtube.sh             # VLC YouTube helper
├── BUILD.md                   # Build instructions
├── INTEGRATION.md             # VSP integration docs
├── MODDING.md                 # C++ modding guide
├── LICENSE                    # MIT License
└── README.md                  # Original Browservice README
```
