#!/usr/bin/env python
from __future__ import with_statement
"""
Browservice MPlayer Helper - CLI Mode
Runs as a simple HTTP server on OS X Leopard (10.5).
Receives HTTP requests from Browservice to launch MPlayer OSX Extended with a stream URL.
Python 2.5 compatible.
"""
import os, signal, subprocess, sys, threading, time, optparse, cgi, urlparse
from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
from SocketServer import ThreadingMixIn

MPLAYER_BIN = "/Applications/MPlayer OSX Extended.app/Contents/Resources/Binaries/mpextended.mpBinaries/Contents/mpextended.mpBinaries/Contents/MacOS/mplayer"
VSP_HOST = "192.168.0.130"
VSP_PORT = 33000

def json_dumps(obj):
    if isinstance(obj, dict):
        return '{' + ', '.join('"%s": %s' % (k, json_dumps(v)) for k, v in obj.items()) + '}'
    elif isinstance(obj, (int, long, float)):
        return str(obj)
    elif isinstance(obj, str):
        return '"' + obj.replace('\\', '\\\\').replace('"', '\\"') + '"'
    elif isinstance(obj, unicode):
        return '"' + obj.replace('\\', '\\\\').replace('"', '\\"').encode('utf-8') + '"'
    elif obj is None: return 'null'
    elif isinstance(obj, bool): return 'true' if obj else 'false'
    else: return '"%s"' % str(obj)

streams_lock = threading.Lock()
active_streams = {}
args = None

def log(msg):
    print "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    sys.stdout.flush()

def kill_existing_player():
    try:
        out = subprocess.Popen(['ps', 'aux'], stdout=subprocess.PIPE).communicate()[0]
        for line in out.splitlines():
            if 'mplayer' in line.lower() and 'grep' not in line:
                parts = line.split()
                if len(parts) >= 2:
                    try: os.kill(int(parts[1]), signal.SIGTERM)
                    except: pass
    except: pass

def get_player_pid():
    try:
        out = subprocess.Popen(['ps', 'aux'], stdout=subprocess.PIPE).communicate()[0]
        for line in out.splitlines():
            if 'mplayer' in line.lower() and 'grep' not in line:
                parts = line.split()
                if len(parts) >= 2:
                    try: return int(parts[1])
                    except: pass
    except: pass
    return None

def is_process_alive(pid):
    try: os.kill(pid, 0); return True
    except: return False

def monitor_stream(pid):
    while True:
        timeout_val = None
        with streams_lock:
            if pid not in active_streams: return
            info = active_streams[pid]
            if time.time() - info['start'] >= info['timeout']:
                del active_streams[pid]
                timeout_val = info['timeout']
        if timeout_val is not None:
            try:
                os.kill(pid, signal.SIGTERM)
                time.sleep(2)
                if is_process_alive(pid): os.kill(pid, signal.SIGKILL)
            except: pass
            log("Closed idle MPlayer (pid %d) after %ds" % (pid, timeout_val))
            return
        time.sleep(5)

def extract_video_id(stream_url):
    """Extract video ID from the VSP stream URL."""
    try:
        parsed = urlparse.urlparse(stream_url)
        params = cgi.parse_qs(parsed.query)
        return params.get('v', [None])[0]
    except:
        return None

def wait_for_cache(video_id, max_wait=120):
    """Poll VSP /cache endpoint until the file is ready."""
    if not video_id:
        return False
    cache_url = "http://%s:%d/cache?v=%s" % (VSP_HOST, VSP_PORT, video_id)
    log("Waiting for cache: %s" % video_id)
    start = time.time()
    while time.time() - start < max_wait:
        try:
            # Use urllib2 since httplib is complex on Python 2.5
            import urllib2
            req = urllib2.Request(cache_url)
            resp = urllib2.urlopen(req)
            if resp.code == 200:
                log("Cache ready: %s" % video_id)
                return True
        except urllib2.HTTPError, e:
            if e.code == 404:
                pass  # Not ready yet
            else:
                log("Cache check error: %s" % str(e.code))
        except Exception, e:
            log("Cache check failed: %s" % str(e))
        time.sleep(3)
    log("Cache wait timeout after %ds" % max_wait)
    return False

def launch_mplayer(stream_url, timeout):
    kill_existing_player()
    time.sleep(0.5)

    # Launch MPlayer directly with the stream URL
    # VSP serves TS with fake Content-Length for seeking
    play_url = stream_url
    video_id = extract_video_id(stream_url)
    log("Playing stream: %s (id: %s)" % (play_url, video_id))

    cmd = [
        MPLAYER_BIN,
        '-cache', '8192',
        '-really-quiet',
        '-vo', 'quartz',
        '-ao', 'coreaudio',
        play_url,
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(2)
    pid = get_player_pid()
    if pid is None:
        log("Warning: Could not determine MPlayer PID")
        return None
    with streams_lock:
        active_streams[pid] = {'url': play_url, 'start': time.time(), 'timeout': timeout}
    if timeout > 0:
        threading.Thread(target=monitor_stream, args=(pid,)).start()
    log("Launched MPlayer (pid %d) for %s" % (pid, video_id or play_url))
    return pid

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/vlc'):
            params = cgi.parse_qs(urlparse.urlparse(self.path).query)
            stream_url = params.get('streamUrl', [None])[0]
            timeout = int(params.get('timeout', [str(args.player_timeout)])[0])
            if not stream_url:
                self.send_response(400)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json_dumps({"status": "error", "message": "missing streamUrl"}))
                return
            pid = launch_mplayer(stream_url, timeout)
            if pid is None:
                self.send_response(500)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json_dumps({"status": "error", "message": "MPlayer launch failed"}))
                return
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json_dumps({"status": "ok", "pid": pid}))
        elif self.path == '/status':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            with streams_lock: count = len(active_streams)
            self.wfile.write(json_dumps({"status": "ok", "streams": count}))
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write("Helper on port %d\n" % args.port)
    def log_message(self, fmt, *vals):
        log("HTTP %s" % (fmt % vals))

def main():
    global args
    parser = optparse.OptionParser()
    parser.add_option('--port', type='int', default=9999)
    parser.add_option('--vlc-timeout', type='int', default=300)
    (opts, _) = parser.parse_args()
    class A: pass
    args = A()
    args.port = opts.port
    args.player_timeout = opts.vlc_timeout
    log("Helper starting on port %d (MPlayer + cache wait)" % args.port)
    server = ThreadedHTTPServer(('0.0.0.0', args.port), Handler)
    try: server.serve_forever()
    except KeyboardInterrupt: server.shutdown()

if __name__ == '__main__': main()
