#!/usr/bin/env python
from __future__ import with_statement
"""
Browservice QuickTime Helper - CLI Mode
Runs as a simple HTTP server on OS X Leopard (10.5).
Receives HTTP requests from Browservice to launch QuickTime Player with a stream URL.
Python 2.5 compatible. No PyObjC dependency (avoids NSRunLoop freeze).
"""

import os
import signal
import subprocess
import sys
import threading
import time
import optparse
import cgi
import urlparse
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
from SocketServer import ThreadingMixIn


def json_dumps(obj):
    """Minimal JSON encoder for flat dicts (Python 2.5 compatible)."""
    if isinstance(obj, dict):
        items = []
        for k, v in obj.items():
            items.append('"%s": %s' % (k, json_dumps(v)))
        return '{' + ', '.join(items) + '}'
    elif isinstance(obj, (int, long, float)):
        return str(obj)
    elif isinstance(obj, str):
        escaped = obj.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
        return '"' + escaped + '"'
    elif isinstance(obj, unicode):
        escaped = obj.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
        return '"' + escaped.encode('utf-8') + '"'
    elif obj is None:
        return 'null'
    elif isinstance(obj, bool):
        return 'true' if obj else 'false'
    else:
        return '"%s"' % str(obj)


# --- Globals ---
streams_lock = threading.Lock()
active_streams = {}
args = None
log_messages = []
MAX_LOG = 50


def log(msg):
    ts = time.strftime("%H:%M:%S")
    line = "[%s] %s" % (ts, msg)
    print line
    sys.stdout.flush()
    log_messages.append(line)
    if len(log_messages) > MAX_LOG:
        log_messages.pop(0)


# --- Player Management ---
def kill_existing_player():
    """Kill existing QuickTime Player processes using ps+grep (no pgrep/pkill on Leopard)."""
    try:
        result = subprocess.Popen(
            ['ps', 'aux'], stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        out, _ = result.communicate()
        for line in out.splitlines():
            if '/QuickTime Player.app' in line and 'grep' not in line:
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        pid = int(parts[1])
                        os.kill(pid, signal.SIGTERM)
                    except (ValueError, OSError):
                        pass
    except:
        pass


def get_player_pid():
    """Find QuickTime Player PID using ps+grep (no pgrep on Leopard)."""
    try:
        result = subprocess.Popen(
            ['ps', 'aux'], stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        out, _ = result.communicate()
        for line in out.splitlines():
            if '/QuickTime Player.app' in line and 'grep' not in line:
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        return int(parts[1])
                    except ValueError:
                        pass
    except:
        pass
    return None


def is_process_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except:
        return False


def monitor_stream(pid):
    while True:
        timeout_val = None
        with streams_lock:
            if pid not in active_streams:
                return
            info = active_streams[pid]
            elapsed = time.time() - info['last_activity']
            if elapsed >= info['timeout']:
                del active_streams[pid]
                timeout_val = info['timeout']
        if timeout_val is not None:
            try:
                os.kill(pid, signal.SIGTERM)
                time.sleep(2)
                if is_process_alive(pid):
                    os.kill(pid, signal.SIGKILL)
            except:
                pass
            log("Closed idle QuickTime (pid %d) after %ds" % (pid, timeout_val))
            return
        time.sleep(5)


def launch_quicktime(stream_url, timeout):
    kill_existing_player()
    time.sleep(0.5)
    subprocess.Popen(['open', '-a', 'QuickTime Player', stream_url])
    time.sleep(1.5)
    pid = get_player_pid()
    if pid is None:
        log("Warning: Could not determine QuickTime Player PID")
        return None
    with streams_lock:
        active_streams[pid] = {
            'url': stream_url,
            'start': time.time(),
            'timeout': timeout,
            'last_activity': time.time(),
        }
    if timeout > 0:
        threading.Thread(target=monitor_stream, args=(pid,)).start()
    log("Launched QuickTime Player (pid %d) for stream" % pid)
    return pid


# --- HTTP Server ---
class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class HelperRequestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/vlc'):
            params = cgi.parse_qs(urlparse.urlparse(self.path).query)
            stream_url = params.get('streamUrl', [None])[0]
            timeout_str = params.get('timeout', [str(args.player_timeout)])[0]
            try:
                timeout = int(timeout_str)
            except:
                timeout = args.player_timeout
            if not stream_url:
                self.send_response(400)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json_dumps({
                    "status": "error",
                    "message": "missing streamUrl parameter"
                }))
                return
            pid = launch_quicktime(stream_url, timeout)
            if pid is None:
                self.send_response(500)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json_dumps({
                    "status": "error",
                    "message": "QuickTime Player launch failed"
                }))
                return
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json_dumps({"status": "ok", "pid": pid}))
        elif self.path == '/status':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            with streams_lock:
                count = len(active_streams)
            self.wfile.write(json_dumps({"status": "ok", "streams": count}))
        else:
            self.send_response(404)
            self.send_header('Content-Type', 'text/plain')
            self.end_headers()
            self.wfile.write("Browservice Helper running on port %d\n" % args.port)

    def log_message(self, format, *values):
        log("HTTP %s" % (format % values))


def main():
    global args

    parser = optparse.OptionParser(description="Browservice QuickTime Helper - CLI Mode")
    parser.add_option('--port', type='int', default=9999,
                      help='Port to listen on (default: 9999)')
    parser.add_option('--vlc-timeout', type='int', default=300,
                      help='Idle timeout in seconds (0 = disabled, default: 300)')
    (options, _) = parser.parse_args()

    class Args:
        port = options.port
        player_timeout = options.vlc_timeout
    args = Args()

    log("Browservice Helper starting (CLI mode)")
    log("HTTP server on port %d" % args.port)
    log("Player timeout: %ds" % args.player_timeout)

    server = ThreadedHTTPServer(('0.0.0.0', args.port), HelperRequestHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("Shutting down...")
        server.shutdown()


if __name__ == '__main__':
    main()
