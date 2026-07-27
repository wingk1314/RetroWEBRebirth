#!/usr/bin/env python3
"""
VintageStreamProxy Replacement — yt-dlp + ffmpeg
Drop-in replacement for VintageStreamProxy that uses yt-dlp to extract
YouTube stream URLs and ffmpeg to transcode to a format compatible with
old VLC on PowerPC Macs.

Listens on port 33000 (same as VSP default).
URL pattern: /<profile>?v=<video_id>

Architecture:
  Request -> yt-dlp extract URL -> ffmpeg transcode -> HTTP stream response
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

# Cache directory for transcoded videos
CACHE_DIR = "/tmp/vsp_cache"

# Transcoding profiles: resolution, video bitrate, audio bitrate, codec
# iMac G4 display is 1024x768 — 720p is the sweet spot
# mpeg4 is G4-friendly (no H.264 decode overhead)
PROFILES = {
    "720p": {
        "scale": "1280:720",
        "video_bitrate": "2500k",
        "audio_bitrate": "128k",
        "vcodec": "mpeg2video",
        "acodec": "mp2",
        "format": "mpegts",
        "extra": "",
    },
    "480p": {
        "scale": "640:480",
        "video_bitrate": "1000k",
        "audio_bitrate": "128k",
        "vcodec": "mpeg2video",
        "acodec": "mp2",
        "format": "mpegts",
        "extra": "",
    },
    "360p": {
        "scale": "480:360",
        "video_bitrate": "500k",
        "audio_bitrate": "96k",
        "vcodec": "mpeg2video",
        "acodec": "mp2",
        "format": "mpegts",
        "extra": "",
    },
    "240p": {
        "scale": "320:240",
        "video_bitrate": "200k",
        "audio_bitrate": "64k",
        "vcodec": "mpeg2video",
        "acodec": "mp2",
        "format": "mpegts",
        "extra": "",
    },
    "144p": {
        "scale": "256:144",
        "video_bitrate": "100k",
        "audio_bitrate": "32k",
        "vcodec": "mpeg2video",
        "acodec": "mp2",
        "format": "mpegts",
        "extra": "",
    },
}

# Default profile
DEFAULT_PROFILE = "720p"

# Active state
active_profile = DEFAULT_PROFILE
active_streams = {}
streams_lock = threading.Lock()

# Cache state: tracks which videos are being cached
# Format: {video_id: {"file": path, "size": int, "done": bool, "profile": str}}
cache_state = {}
cache_lock = threading.Lock()


def get_cache_path(video_id, profile_name):
    """Get the file path for a cached video."""
    return os.path.join(CACHE_DIR, "%s_%s.ts" % (video_id, profile_name))


def get_video_duration(video_id):
    """Get video duration in seconds from yt-dlp."""
    try:
        result = subprocess.run(
            [
                "yt-dlp",
                "--no-warnings",
                "--get-duration",
                "https://www.youtube.com/watch?v=%s" % video_id,
            ],
            capture_output=True,
            text=True,
            timeout=15,
        )
        if result.returncode == 0 and result.stdout.strip():
            duration_str = result.stdout.strip()
            # Parse HH:MM:SS or MM:SS or seconds
            parts = duration_str.split(":")
            if len(parts) == 3:
                return int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])
            elif len(parts) == 2:
                return int(parts[0]) * 60 + float(parts[1])
            else:
                return float(parts[0])
    except Exception as e:
        log("Duration lookup failed for %s: %s" % (video_id, str(e)))
    return None


def estimate_file_size(duration_secs, profile_name):
    """Estimate output file size from duration and bitrate."""
    profile = PROFILES.get(profile_name, PROFILES[DEFAULT_PROFILE])
    # Parse bitrates (e.g., "2500k" -> 2500000 bits/sec)
    vbr = int(profile["video_bitrate"].replace("k", "")) * 1000
    abr = int(profile["audio_bitrate"].replace("k", "")) * 1000
    bits_per_sec = vbr + abr
    # Add ~10% overhead for MPEG-TS container
    bytes_est = int(duration_secs * bits_per_sec / 8 * 1.10)
    return bytes_est


def log(msg):
    ts = time.strftime("%H:%M:%S")
    print("[%s] %s" % (ts, msg), flush=True)


def get_stream_urls(video_id):
    """Use yt-dlp to extract stream URLs (video + audio separate)."""
    try:
        result = subprocess.run(
            [
                "yt-dlp",
                "--no-warnings",
                "-f", "bestvideo[height<=1080]+bestaudio/best",
                "--get-url",
                "https://www.youtube.com/watch?v=%s" % video_id,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if result.returncode == 0 and result.stdout.strip():
            urls = result.stdout.strip().split("\n")
            # Filter out empty lines
            urls = [u for u in urls if u.strip()]
            return urls
        else:
            log("yt-dlp failed for %s: %s" % (video_id, result.stderr[:200]))
            return None
    except subprocess.TimeoutExpired:
        log("yt-dlp timeout for %s" % video_id)
        return None
    except Exception as e:
        log("yt-dlp error for %s: %s" % (video_id, str(e)))
        return None


def start_ffmpeg(stream_urls, profile_name, video_id, output_path):
    """Start ffmpeg to transcode to a file for caching/seeking."""
    profile = PROFILES.get(profile_name, PROFILES[DEFAULT_PROFILE])

    # Build input args - one -i per stream URL
    input_args = []
    for url in stream_urls:
        input_args.extend(["-i", url])

    # Map: video from first input, audio from second input (if exists)
    map_args = ["-map", "0:v:0"]
    if len(stream_urls) > 1:
        map_args.extend(["-map", "1:a:0?"])
    else:
        map_args.extend(["-map", "0:a:0?"])

    cmd = [
        "ffmpeg",
        "-y",
        "-re",
    ] + input_args + [
        "-f", profile["format"],
    ] + map_args + [
        "-vcodec", profile["vcodec"],
        "-vf", "scale=%s:force_original_aspect_ratio=decrease,pad=%s:(ow-iw)/2:(oh-ih)/2" % (
            profile["scale"], profile["scale"]
        ),
        "-b:v", profile["video_bitrate"],
        "-acodec", profile["acodec"],
        "-b:a", profile["audio_bitrate"],
        "-ar", "44100",
        "-ac", "2",
        "-max_muxing_queue_size", "1024",
        "-g", "50",
        "-sc_threshold", "0",
    ]

    # Add extra flags (e.g., -movflags +faststart)
    if profile.get("extra"):
        cmd.extend(profile["extra"].split())

    cmd.append(output_path)

    log("Starting ffmpeg for %s (profile: %s, %d streams)" % (
        video_id, profile_name, len(stream_urls)
    ))
    log("Resolution: %s, Video: %s, Audio: %s" % (
        profile["scale"], profile["video_bitrate"], profile["audio_bitrate"]
    ))

    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        return proc
    except Exception as e:
        log("ffmpeg start error: %s" % str(e))
        return None


RESOLUTION_PAGE_HTML = """<!DOCTYPE html>
<html>
<head>
<title>VSP Resolution</title>
<style>
body {
    font-family: monospace;
    background: #1a1a2e;
    color: #e0e0e0;
    margin: 40px;
    text-align: center;
}
h1 { color: #00d4ff; }
h2 { color: #aaa; font-weight: normal; }
.current {
    font-size: 1.4em;
    color: #00ff88;
    margin: 20px 0;
    padding: 15px;
    border: 2px solid #00ff88;
    border-radius: 8px;
    display: inline-block;
}
.options {
    margin: 30px 0;
}
.option {
    display: inline-block;
    margin: 10px;
    padding: 15px 25px;
    border: 2px solid #444;
    border-radius: 8px;
    text-decoration: none;
    color: #e0e0e0;
    background: #16213e;
    font-size: 1.1em;
    font-family: monospace;
    transition: all 0.2s;
}
.option:hover {
    border-color: #00d4ff;
    background: #0f3460;
}
.option.active {
    border-color: #00ff88;
    background: #0f3460;
}
.note {
    color: #666;
    font-size: 0.9em;
    margin-top: 40px;
}
</style>
</head>
<body>
<h1>VintageStreamProxy</h1>
<h2>Streaming Resolution</h2>
<div class="current">Current: %s</div>
<div class="options">
%s
</div>
<div class="note">
Select a resolution to change the stream quality.<br>
The next video you play will use the new resolution.
</div>
</body>
</html>"""


class StreamRequestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global active_profile
        path = self.path

        # Resolution picker page
        if path == "/" or path == "/resolution":
            buttons = []
            for name in PROFILES:
                p = PROFILES[name]
                active_class = ' active' if name == active_profile else ''
                buttons.append(
                    '<a class="option%s" href="/set/%s">%s (%s)</a>' % (
                        active_class, name, name, p["scale"]
                    )
                )
            html = RESOLUTION_PAGE_HTML % (
                active_profile,
                "\n".join(buttons)
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(html.encode())
            return

        # Set resolution
        if path.startswith("/set/"):
            new_profile = path[5:].strip("/")
            if new_profile in PROFILES:
                active_profile = new_profile
                log("Resolution changed to %s (%s)" % (
                    new_profile, PROFILES[new_profile]["scale"]
                ))
                # Redirect back to picker
                self.send_response(302)
                self.send_header("Location", "/")
                self.end_headers()
                return
            else:
                self.send_response(400)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"Unknown profile")
                return

        # Status
        if path == "/status":
            with cache_lock:
                cached_count = len(cache_state)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            with streams_lock:
                count = len(active_streams)
            self.wfile.write(('%s' % {"profile": active_profile, "streams": count, "cached": cached_count}).encode())
            return

        # Stream request: /<profile>?v=<video_id>&resolution=720p
        # Also serve cached files: /cache?v=<video_id>
        if "?" in path:
            # Cache serving endpoint
            if "/cache" in path:
                params = {}
                query = path.split("?", 1)[1] if "?" in path else ""
                for pair in query.split("&"):
                    if "=" in pair:
                        k, v = pair.split("=", 1)
                        params[k] = v
                video_id = params.get("v")
                if not video_id:
                    self.send_error(400, "Missing video ID")
                    return
                # Find cached file (any profile)
                with cache_lock:
                    cached = cache_state.get(video_id)
                if not cached or not cached["done"]:
                    self.send_error(404, "Not cached yet")
                    return
                cache_path = cached["file"]
                if not os.path.exists(cache_path):
                    self.send_error(404, "Cache file missing")
                    return
                # Serve with Range support
                file_size = os.path.getsize(cache_path)
                range_header = self.headers.get("Range")
                if range_header:
                    # Parse Range: bytes=START-END
                    range_spec = range_header.replace("bytes=", "")
                    parts = range_spec.split("-")
                    start = int(parts[0]) if parts[0] else 0
                    end = int(parts[1]) if parts[1] else file_size - 1
                    length = end - start + 1
                    self.send_response(206)
                    self.send_header("Content-Type", "video/mp2t")
                    self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, file_size))
                    self.send_header("Content-Length", str(length))
                    self.send_header("Accept-Ranges", "bytes")
                    self.end_headers()
                    with open(cache_path, "rb") as f:
                        f.seek(start)
                        remaining = length
                        while remaining > 0:
                            chunk_size = min(65536, remaining)
                            chunk = f.read(chunk_size)
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                            remaining -= len(chunk)
                else:
                    self.send_response(200)
                    self.send_header("Content-Type", "video/mp2t")
                    self.send_header("Content-Length", str(file_size))
                    self.send_header("Accept-Ranges", "bytes")
                    self.end_headers()
                    with open(cache_path, "rb") as f:
                        while True:
                            chunk = f.read(65536)
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                return

            profile_name, query = path[1:].split("?", 1)
            params = {}
            for pair in query.split("&"):
                if "=" in pair:
                    k, v = pair.split("=", 1)
                    params[k] = v

            video_id = params.get("v")
            if not video_id:
                self.send_error(400, "Missing video ID")
                return

            # Use resolution param from browservice if present, else use profile from URL
            resolution = params.get("resolution")
            if resolution and resolution in PROFILES:
                profile_name = resolution
            elif profile_name not in PROFILES:
                profile_name = active_profile

            log("Request: profile=%s video_id=%s" % (profile_name, video_id))

            # Get video duration for estimated Content-Length
            duration = get_video_duration(video_id)
            if duration:
                estimated_size = estimate_file_size(duration, profile_name)
                log("Duration: %.0fs, Estimated size: %d bytes" % (duration, estimated_size))
            else:
                estimated_size = None
                log("Could not get duration, will use actual file size")

            stream_urls = get_stream_urls(video_id)
            if not stream_urls:
                self.send_error(502, "Failed to extract stream URL")
                return

            cache_file_path = get_cache_path(video_id, profile_name)

            proc = start_ffmpeg(stream_urls, profile_name, video_id, cache_file_path)
            if proc is None:
                self.send_error(500, "Failed to start ffmpeg")
                return

            with streams_lock:
                active_streams[video_id] = proc

            log("Transcoding to file: %s" % cache_file_path)

            # Log ffmpeg stderr in background for debugging
            def log_ffmpeg_stderr():
                try:
                    while True:
                        line = proc.stderr.readline()
                        if not line:
                            break
                        log("ffmpeg: %s" % line.decode(errors='replace').strip())
                except:
                    pass
            stderr_thread = threading.Thread(target=log_ffmpeg_stderr, daemon=True)
            stderr_thread.start()

            # Check if cache already exists (second play - scrubbing works)
            with cache_lock:
                cached = cache_state.get(video_id)
            if cached and cached.get("done") and os.path.exists(cached["file"]):
                file_size = cached["size"]
                log("Serving from cache: %s (%d bytes)" % (video_id, file_size))
                try:
                    range_header = self.headers.get("Range")
                    if range_header:
                        range_spec = range_header.replace("bytes=", "")
                        parts = range_spec.split("-")
                        start = int(parts[0]) if parts[0] else 0
                        end = int(parts[1]) if parts[1] else file_size - 1
                        length = end - start + 1
                        self.send_response(206)
                        self.send_header("Content-Type", "video/mp2t")
                        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, file_size))
                        self.send_header("Content-Length", str(length))
                        self.send_header("Accept-Ranges", "bytes")
                        self.end_headers()
                        with open(cached["file"], "rb") as f:
                            f.seek(start)
                            remaining = length
                            while remaining > 0:
                                chunk = f.read(min(65536, remaining))
                                if not chunk:
                                    break
                                self.wfile.write(chunk)
                                remaining -= len(chunk)
                    else:
                        self.send_response(200)
                        self.send_header("Content-Type", "video/mp2t")
                        self.send_header("Content-Length", str(file_size))
                        self.send_header("Accept-Ranges", "bytes")
                        self.end_headers()
                        with open(cached["file"], "rb") as f:
                            while True:
                                chunk = f.read(65536)
                                if not chunk:
                                    break
                                self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError, OSError):
                    log("Client disconnected (cache serve)")
                return

            # First play - stream via chunked encoding (plays immediately)
            log("Waiting for initial buffer...")
            for _ in range(10):
                time.sleep(0.5)
                if os.path.exists(cache_file_path) and os.path.getsize(cache_file_path) > 0:
                    break
                if proc.poll() is not None:
                    break

            if not os.path.exists(cache_file_path) or os.path.getsize(cache_file_path) == 0:
                log("No initial buffer for %s" % video_id)
                self.send_error(500, "Transcoding failed")
                with streams_lock:
                    active_streams.pop(video_id, None)
                return

            # Monitor ffmpeg to mark cache done
            def monitor_ffmpeg():
                proc.wait()
                final_size = os.path.getsize(cache_file_path) if os.path.exists(cache_file_path) else 0
                with cache_lock:
                    cache_state[video_id] = {
                        "file": cache_file_path,
                        "size": final_size,
                        "done": True,
                        "profile": profile_name,
                    }
                log("Cache ready: %s (%d bytes)" % (video_id, final_size))

            threading.Thread(target=monitor_ffmpeg, daemon=True).start()

            # Stream with fake Content-Length for MPlayer seeking
            fake_size = estimated_size or 16000000  # fallback 16MB
            log("Streaming live: %s (fake CL=%d)" % (video_id, fake_size))
            try:
                range_header = self.headers.get("Range")
                real_size = os.path.getsize(cache_file_path) if os.path.exists(cache_file_path) else 0

                if range_header:
                    # Range request during live stream
                    range_spec = range_header.replace("bytes=", "")
                    parts = range_spec.split("-", 1)
                    start = int(parts[0]) if parts[0] else 0
                    end = int(parts[1]) if parts[1] and parts[1] else fake_size - 1

                    if start >= real_size:
                        # Wait up to 10s for data
                        waited = 0
                        while waited < 10:
                            time.sleep(0.5)
                            waited += 0.5
                            real_size = os.path.getsize(cache_file_path) if os.path.exists(cache_file_path) else 0
                            if start < real_size:
                                break

                    if start >= real_size:
                        self.send_response(416)
                        self.send_header("Content-Range", "bytes */%d" % fake_size)
                        self.end_headers()
                        return

                    effective_end = min(end, real_size - 1) if real_size > 0 else real_size - 1
                    length = effective_end - start + 1

                    self.send_response(206)
                    self.send_header("Content-Type", "video/mp2t")
                    self.send_header("Content-Range", "bytes %d-%d/%d" % (start, effective_end, fake_size))
                    self.send_header("Content-Length", str(length))
                    self.send_header("Accept-Ranges", "bytes")
                    self.end_headers()

                    with open(cache_file_path, "rb") as f:
                        f.seek(start)
                        remaining = length
                        while remaining > 0:
                            chunk = f.read(min(65536, remaining))
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                            remaining -= len(chunk)
                else:
                    # No range — serve from beginning with fake CL
                    self.send_response(200)
                    self.send_header("Content-Type", "video/mp2t")
                    self.send_header("Content-Length", str(fake_size))
                    self.send_header("Accept-Ranges", "bytes")
                    self.end_headers()

                    offset = 0
                    while True:
                        real_size = os.path.getsize(cache_file_path) if os.path.exists(cache_file_path) else 0
                        if offset < real_size:
                            with open(cache_file_path, "rb") as f:
                                f.seek(offset)
                                chunk = f.read(min(65536, real_size - offset))
                                if chunk:
                                    self.wfile.write(chunk)
                                    offset += len(chunk)
                        else:
                            if proc.poll() is not None:
                                # ffmpeg done, drain remaining
                                with open(cache_file_path, "rb") as f:
                                    f.seek(offset)
                                    while True:
                                        chunk = f.read(65536)
                                        if not chunk:
                                            break
                                        self.wfile.write(chunk)
                                break
                            time.sleep(0.25)

            except (BrokenPipeError, ConnectionResetError, OSError):
                log("Client disconnected for %s" % video_id)
            finally:
                # Mark cache as done
                if os.path.exists(cache_file_path):
                    cache_size = os.path.getsize(cache_file_path)
                    with cache_lock:
                        cache_state[video_id] = {
                            "file": cache_file_path,
                            "size": cache_size,
                            "done": True,
                            "profile": profile_name,
                        }
                    log("Cache final: %s (%d bytes)" % (video_id, cache_size))
                with streams_lock:
                    active_streams.pop(video_id, None)
        else:
            self.send_error(400, "Missing query parameters")

    def log_message(self, format, *values):
        log("HTTP %s" % (format % values))


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global active_profile

    parser = argparse.ArgumentParser(
        description="VintageStreamProxy (yt-dlp + ffmpeg)"
    )
    parser.add_argument("--port", type=int, default=33000)
    parser.add_argument("--host", type=str, default="0.0.0.0")
    parser.add_argument("--profile", type=str, default=DEFAULT_PROFILE,
                        help="Default resolution profile (720p, 480p, 360p, 240p, 144p)")
    args = parser.parse_args()

    if args.profile in PROFILES:
        active_profile = args.profile

    log("VintageStreamProxy (yt-dlp + ffmpeg) starting")
    log("Listening on %s:%d" % (args.host, args.port))
    log("Default profile: %s (%s)" % (active_profile, PROFILES[active_profile]["scale"]))
    log("Profiles: %s" % ", ".join("%s(%s)" % (k, v["scale"]) for k, v in PROFILES.items()))
    log("Cache dir: %s" % CACHE_DIR)
    log("Resolution picker: http://<host>:%d/" % args.port)

    # Create cache directory
    os.makedirs(CACHE_DIR, exist_ok=True)

    server = ThreadedHTTPServer((args.host, args.port), StreamRequestHandler)

    def shutdown_handler(signum, frame):
        log("Shutting down...")
        with streams_lock:
            for vid, proc in list(active_streams.items()):
                try:
                    proc.terminate()
                except:
                    pass
        server.shutdown()

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        shutdown_handler(None, None)


if __name__ == "__main__":
    main()
