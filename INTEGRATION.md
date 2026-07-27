# Browservice + VintageStreamProxy Integration

Watch YouTube videos on your iMac G4 by combining Browservice (for browsing) with VintageStreamProxy (for video playback).

## How It Works

```
┌─────────────────────────────────────────────────────────────┐
│  iMac G4 (OS X Tiger)                                       │
│                                                             │
│  ┌─────────────────┐    ┌─────────────────┐                │
│  │   Safari/IE     │    │      VLC        │                │
│  │   (Browservice) │    │   (Vintage      │                │
│  │                 │    │    StreamProxy)  │                │
│  └────────┬────────┘    └────────▲────────┘                │
│           │                      │                          │
│           │   1. Browse YouTube  │                          │
│           │   2. Copy URL        │                          │
│           │   3. Paste in form   │                          │
│           │   4. Click Play ─────┘                          │
│           │                                                 │
└───────────┼─────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────┐
│  Server (Linux/Mac VM)                                      │
│                                                             │
│  ┌─────────────────┐    ┌─────────────────┐                │
│  │   Browservice   │    │ VintageStream   │                │
│  │   (Chromium)    │    │    Proxy        │                │
│  └─────────────────┘    └─────────────────┘                │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## Setup

### 1. Install VintageStreamProxy on your server

```bash
# On your Linux/Mac VM
git clone https://github.com/SoftwareGuy/VintageStreamProxy.git
cd VintageStreamProxy
dotnet run -c Release
```

### 2. Install Browservice on your server

```bash
# On your Linux/Mac VM
git clone https://github.com/ttalvitie/browservice.git
cd browservice
# Follow BUILD.md for your platform
```

### 3. Set up the iMac G4

1. Copy `vlc-youtube.sh` to the iMac G4
2. Copy `vlc-launcher.html` to the iMac G4
3. Install VLC on the iMac G4

### 4. Configure

Edit `vlc-youtube.sh` and set the VintageStreamProxy server address:

```bash
VSP_SERVER="http://SERVER_IP:33000"
```

## Usage

### Method 1: Using the HTML Form

1. Open Browservice on the iMac G4
2. Browse YouTube and find a video
3. Copy the YouTube URL
4. Open `vlc-launcher.html` in Browservice (as a local file or from the server)
5. Paste the YouTube URL
6. Click "Open in VLC"
7. Copy the stream URL and open it in VLC

### Method 2: Using the Shell Script

```bash
# On the iMac G4
./vlc-youtube.sh "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
./vlc-youtube.sh "dQw4w9WgXcQ"
./vlc-youtube.sh "dQw4w9WgXcQ" mpeg1
```

### Method 3: Using the Clipboard

1. Browse YouTube in Browservice
2. Select and copy the video ID from the URL bar
3. Open Terminal on the iMac G4
4. Run: `./vlc-youtube.sh <paste-video-id>`

## Available Profiles

| Profile | Container | Video | Audio | Resolution |
|---------|-----------|-------|-------|------------|
| `default` | ASF | WMV2 | WMA2 | 320x240 |
| `win98` | ASF | MSMPEG4v2 | MP3 | 320x240 |
| `mpeg1` | MPEG | MPEG1 | MPEG2 | 320x240 |
| `xvid` | ASF | MSMPEG4v2 | MP3 | varies |
| `potato` | MP2 | MPEG1 | MPEG2 | 240x180 |
| `spud` | MP2 | MPEG1 | MPEG2 | 160x120 |

## Tips

- **Performance**: Use `potato` or `spud` profiles on slower hardware
- **Quality**: Use `default` or `win98` on faster hardware (500MHz+)
- **Audio**: If audio is choppy, try reducing the video quality
- **Network**: Use Ethernet, not WiFi (100Mbps recommended)

## Troubleshooting

- **VLC won't open**: Make sure VLC is installed and can open URLs
- **No video**: Check that VintageStreamProxy is running and accessible
- **Choppy playback**: Try a lower quality profile or check network speed
- **No audio**: Some profiles have limited audio quality

## Future Improvements

Potential enhancements to make this more seamless:

1. **Browser bookmarklet** - One-click URL capture from Browservice
2. **Custom Browservice button** - Add a "Play in VLC" button to the control bar
3. **Automatic detection** - JavaScript injection to detect YouTube URLs
4. **VLC web interface** - Control VLC remotely from Browservice

See `MODDING.md` for details on modifying Browservice's C++ code.
