#!/usr/bin/env python3
"""
Browservice VLC Helper Server
Runs on the iMac G4 (client machine). Receives HTTP requests from the
Browservice server to launch VLC with a stream URL and auto-close idle
VLC instances after a configurable timeout.

Requirements:
  - Python 3.5+ (uses subprocess.run with capture_output, threading)
  - VLC installed and accessible via 'open -a VLC'
  - Mac OS X with osascript (AppleScript) for fullscreen keystroke
  - "Enable access for assistive devices" must be enabled in
    System Preferences > Universal Access for System Events keystroke
    commands to work on Tiger.

Usage:
  python3 helper.py [--port PORT] [--vlc-timeout SECONDS]

Endpoint:
  GET /vlc?streamUrl=<encoded_url>&timeout=<seconds>
    200 -> {"status": "ok", "pid": 12345}
    400 -> {"status": "error", "message": "missing streamUrl parameter"}
    500 -> {"status": "error", "message": "VLC launch failed: <detail>"}
"""

import argparse
import json
import os
import signal
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

# Global lock for active_streams dict (thread safety)
streams_lock = threading.Lock()

# Track active VLC streams: {pid: {url, start, timeout, last_activity}}
active_streams = {}

# CLI args (set in main)
args = None


def log(msg):
    """Log a message with timestamp."""
    print("[{}] {}".format(time.strftime("%Y-%m-%d %H:%M:%S"), msg), flush=True)


def check_assistive_devices():
    """Check if 'Enable access for assistive devices' is enabled on Tiger.

    On Mac OS X Tiger, System Events keystroke commands require this setting
    to be enabled in System Preferences > Universal Access. If not enabled,
    fullscreen keystroke will silently fail.
    """
    try:
        # Try to run a harmless AppleScript via System Events
        result = subprocess.run(
            ['osascript', '-e', 'tell application "System Events" to get name'],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode != 0:
            log("WARNING: 'Enable access for assistive devices' may not be enabled.")
            log("  System Preferences > Universal Access > Enable access for assistive devices")
            return False
    except (subprocess.TimeoutExpired, FileNotFoundError):
        log("WARNING: Could not verify assistive devices status (osascript not found or timeout).")
        return False
    return True


def kill_existing_vlc():
    """Kill any existing VLC process."""
    try:
        subprocess.run(['pkill', '-x', 'VLC'], timeout=5)
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass


def get_vlc_pid():
    """Get PID of the running VLC process."""
    try:
        result = subprocess.run(
            ['pgrep', '-x', 'VLC'],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0 and result.stdout.strip():
            return int(result.stdout.strip().split('\n')[0])
    except (subprocess.TimeoutExpired, FileNotFoundError, ValueError):
        pass
    return None


def is_process_alive(pid):
    """Check if a process with the given PID is still running."""
    try:
        os.kill(pid, 0)
        return True
    except (ProcessLookupError, PermissionError):
        return False



def monitor_stream(pid):
    """Monitor VLC process and kill after idle timeout."""
    while True:
        timeout_val = None
        with streams_lock:
            if pid not in active_streams:
                return  # Stream was already removed
            info = active_streams[pid]
            elapsed = time.time() - info['last_activity']

            if elapsed >= info['timeout']:
                # Remove from dict before killing (atomic under lock)
                del active_streams[pid]
                timeout_val = info['timeout']

        if timeout_val is not None:
            # Kill VLC outside the lock to avoid blocking other threads.
            try:
                os.kill(pid, signal.SIGTERM)
                time.sleep(2)
                if is_process_alive(pid):
                    os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            log("Closed idle VLC (pid {}) after {}s".format(pid, timeout_val))
            return

        time.sleep(5)  # Check every 5 seconds


def launch_vlc(stream_url, timeout):
    """Launch VLC with the given stream URL and start monitor thread."""
    # Kill any existing VLC instance first
    kill_existing_vlc()
    time.sleep(0.5)  # Brief pause to ensure old VLC is gone

    subprocess.Popen(['open', '-a', 'VLC', stream_url])
    # 'open -a' returns Finder PID; need actual VLC PID
    time.sleep(1)  # Wait for VLC to start
    pid = get_vlc_pid()

    if pid is None:
        log("Warning: Could not determine VLC PID")
        return None

    with streams_lock:
        active_streams[pid] = {
            'url': stream_url,
            'start': time.time(),
            'timeout': timeout,
            'last_activity': time.time()
        }

    if timeout > 0:
        threading.Thread(target=monitor_stream, args=(pid,), daemon=True).start()

    return pid


class VLCRequestHandler(BaseHTTPRequestHandler):
    """HTTP request handler for VLC launch requests."""

    def do_GET(self):
        if self.path.startswith('/vlc'):
            # Parse query params
            params = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            stream_url = params.get('streamUrl', [None])[0]
            timeout_str = params.get('timeout', [str(args.vlc_timeout)])[0]

            try:
                timeout = int(timeout_str)
            except ValueError:
                timeout = args.vlc_timeout

            if not stream_url:
                self.send_response(400)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({
                    "status": "error",
                    "message": "missing streamUrl parameter"
                }).encode())
                return

            pid = launch_vlc(stream_url, timeout)

            if pid is None:
                self.send_response(500)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({
                    "status": "error",
                    "message": "VLC launch failed: could not determine VLC PID"
                }).encode())
                return

            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({
                "status": "ok",
                "pid": pid
            }).encode())
        else:
            self.send_error(404, "Not found")

    def log_message(self, format, *values):
        """Override to use our log function instead of stderr."""
        log("HTTP {} - {}".format(self.address_string(), format % values))


def main():
    global args

    parser = argparse.ArgumentParser(
        description="Browservice VLC Helper Server"
    )
    parser.add_argument(
        '--port', type=int, default=9999,
        help='Port to listen on (default: 9999)'
    )
    parser.add_argument(
        '--vlc-timeout', type=int, default=300,
        help='Default idle timeout in seconds (0 = disabled, default: 300)'
    )
    args = parser.parse_args()

    log("Browservice VLC Helper starting on port {}".format(args.port))
    log("Default VLC timeout: {}s".format(args.vlc_timeout))

    check_assistive_devices()

    server = HTTPServer(('0.0.0.0', args.port), VLCRequestHandler)
    log("Listening on 0.0.0.0:{}".format(args.port))

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("Shutting down...")
        server.shutdown()
        log("Server stopped")


if __name__ == '__main__':
    main()
