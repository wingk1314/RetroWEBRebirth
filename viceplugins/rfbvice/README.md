# rfbvice: VNC front end for RetroWEBRebirth

`rfbvice` is a Browservice vice plugin. It serves the browser over the VNC (RFB) protocol, so the iMac uses a VNC viewer instead of Safari. It replaces `retrojsvice`'s "full PNG per HTTP request" loop with an open connection that sends only what changed:

| | retrojsvice (Safari) | rfbvice (VNC viewer) |
|---|---|---|
| Per update | Whole frame as PNG/JPEG | Only the changed 32×32 tiles |
| Scrolling | Whole new frame | One CopyRect plus the newly revealed strip |
| Transport | New HTTP request per frame, one frame in flight | One TCP connection; the client asks for updates at its own pace |
| Encoding | PNG or JPEG, chosen on the server | Negotiated per client: Tight (JPEG for photos, zlib for UI), ZRLE, Hextile... |
| Cursor shape | Hidden in the image size | Real cursor shape updates (arrow, hand, I-beam) |
| Input | Queued in the next image URL | Sent the moment it happens |

In the end-to-end test, a 30 px scroll on a 640×480 screen sends 11% of the screen's pixels; the rest is a single CopyRect.

The Browservice control bar (address bar, back/forward, the red PowerVLC play button and the resolution picker) is drawn into the frame, so it works unchanged.

## Build

On the Linux server:

```bash
sudo apt install libvncserver-dev     # Debian/Ubuntu
make rfbvice                          # builds release/bin/rfbvice.so next to browservice
```

Optional tests (unit tests plus an end-to-end test with a real VNC client):

```bash
make -C viceplugins/rfbvice release test
```

## Run

Start Browservice with the plugin. Use your usual flags, but replace the `--vice-opt-http-*` options with the VNC ones:

```bash
./release/bin/browservice \
  --vice-plugin=rfbvice.so \
  --vice-opt-vnc-listen-addr=0.0.0.0:5900 \
  --vice-opt-screen-size=1024x740 \
  --vice-opt-vnc-password=retro \
  --vlc-enabled=YES --vlc-client-ip=IMAC_IP --vlc-helper-port=9999 \
  --vsp-server=http://SERVER_IP:33000 --vsp-profile=default \
  --data-dir=$HOME/browservice/data \
  --start-page=https://www.google.com
```

Then connect from the iMac:

- **Leopard (10.5):** in Finder choose Go > Connect to Server and enter `vnc://SERVER_IP:5900`. This opens the built-in Screen Sharing app. Screen Sharing asks for a password, so set `--vice-opt-vnc-password`.
- **Tiger (10.4) or Leopard:** use [Chicken of the VNC](https://sourceforge.net/projects/cotvnc/) 2.0b4 (Universal binary, Mac OS X 10.3.9+). Host `SERVER_IP`, display `0` (port 5900).

Viewer tips:

- Make `screen-size` match the iMac's screen, minus the viewer's title bar and menu bar if you don't run the viewer full screen. 1024×768 screen: `1024x740` windowed, or `1024x768` full screen.
- In Chicken of the VNC, Tight with a medium JPEG quality is a good default. If you see "bad subencoding" errors, move ZRLE or Hextile above Tight in the connection profile.
- The Mac Command key is mapped to Control, so Command-C/V/X/A/F work in the browser as you expect. Turn this off with `--vice-opt-command-key-as-control=no`.

## Options

| Option | Default | Meaning |
|---|---|---|
| `vnc-listen-addr` | `127.0.0.1:5900` | Bind address and port. Use `0.0.0.0:5900` so the iMac can connect. |
| `screen-size` | `1024x768` | Size of the VNC screen (and the browser viewport). |
| `vnc-password` | none | Classic VNC password (at most 8 characters). It is not encryption, so use it only on your LAN. |
| `scroll-detection` | `yes` | Send vertical scrolling as a CopyRect. |
| `wheel-step` | `120` | Wheel delta per notch. Lower it for slower scrolling. |
| `command-key-as-control` | `yes` | Map Meta/Super (the Mac Command key) to Control. |

## What is not supported (yet)

- **File downloads and uploads.** VNC has no file transfer. Downloads are dropped with a log message, and upload dialogs are refused. If you need them, run a second Browservice instance with `retrojsvice` on another port and a separate `--data-dir`.
- **Multiple windows side by side.** VNC has one screen. Popups and `target=_blank` links open on top of their parent, like tabs. Closing one (for example with the page's close button) returns to the window underneath.
- **Clipboard** travels as Latin-1, which is a limit of the classic VNC protocol. Characters outside Latin-1 become `?`. Text copied in the browser with Control-C (or Command-C) is pushed to the viewer's clipboard automatically.

## How it works

- `src/rfbvice.cpp`: the vice plugin API and the LibVNCServer integration. One background thread runs the VNC server and owns the framebuffer. The program thread receives input events and fetches frames, and hands them over through a small mailbox.
- `src/frame_diff.cpp`: diffs each new frame against what clients already have. Row hashing finds vertical scrolls, which become `rfbDoCopyRect`. Then only the changed 32×32 tiles are copied and marked as modified. A scroll is verified pixel by pixel before it is used, so a hash collision can never corrupt the client's screen.
- `src/keys.cpp`: X11 keysym to vice key mapping, and the Latin-1/UTF-8 clipboard conversion.

This plugin is also the server side for a future native PowerPC client: a Cocoa + OpenGL viewer that speaks the same protocol and adds a local cursor and instant scrolling.
