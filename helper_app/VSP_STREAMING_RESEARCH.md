# VSP Streaming Research: Seeking During Transcode

## Problem
User wants seeking/scrubbing during first play while ffmpeg is still transcoding.

## Key Findings

### 1. VLC 2.0.10 Behavior
- VLC reads `Content-Length` header to determine total file duration
- Shows seekbar based on Content-Length
- **Can seek to un-downloaded portions** by sending new HTTP `Range` request
- Server responds with `206 Partial Content` from available data
- **Critical**: moov atom must be at file beginning (`-movflags +faststart`)

### 2. ffmpeg File Size Prediction
- **Estimated size**: `duration_seconds × (video_bitrate + audio_bitrate) / 8 + overhead`
- With `-maxrate` and `-bufsize`, estimation is within ±5-10%
- **moov_size option**: Can reserve space for moov atom at file start
- **delay_moov**: Delays writing moov until first fragment is cut

### 3. GitHub Projects Found
| Project | Approach | Notes |
|---------|----------|-------|
| mp4-pseudo-streaming | Pre-calculate size, serve with Content-Length | PHP-based |
| video-streaming-server | ffmpeg + HTTP server | Node.js |
| yt-dlp-web-ui | Web UI for yt-dlp | Go-based |

### 4. Recommended Solution: Estimated Content-Length

**Strategy:**
1. Get video duration from yt-dlp metadata
2. Calculate estimated file size: `duration × (video_bitrate + audio_bitrate) / 8 + 10%`
3. Start ffmpeg with `-movflags +faststart -moov_size <estimated_moov_size>`
4. Wait for initial buffer (5 seconds for moov atom)
5. Serve with `Content-Length: <estimated_size>` and `Accept-Ranges: bytes`
6. Stream data from file as it grows
7. When VLC seeks beyond available data, server handles Range request

**Key insight:** VLC CAN seek to un-downloaded portions. When you click the seekbar at a position beyond what's been transcoded:
- VLC sends `Range: bytes=OFFSET-` request
- Server responds with `206 Partial Content` from available data
- If data at that offset doesn't exist yet, server waits or returns available data

### 5. Implementation Plan

```python
# In VSP handler:
duration = get_video_duration(video_id)
estimated_size = int(duration * (video_bitrate + audio_bitrate) / 8 * 1.10)

# Wait for initial buffer
time.sleep(5)  # Wait for moov atom

# Serve with estimated Content-Length
self.send_response(200)
self.send_header("Content-Type", "video/mp4")
self.send_header("Content-Length", str(estimated_size))
self.send_header("Accept-Ranges", "bytes")
self.end_headers()

# Stream from growing file
with open(cache_file_path, "rb") as f:
    while True:
        # Read available data
        pos = f.tell()
        f.seek(0, 2)
        end_pos = f.tell()
        f.seek(pos)
        
        if end_pos > pos:
            chunk = f.read(min(end_pos - pos, 65536))
            if chunk:
                self.wfile.write(chunk)
                self.wfile.flush()
        else:
            if proc.poll() is not None:
                break
            time.sleep(0.25)
```

### 6. Risks
- **VLC may hang** if Content-Length is much larger than actual data
- **moov atom size** is hard to predict accurately
- **VLC 2.0.10** uses Libav (not FFmpeg), may have limited fMP4 support

### 7. Alternative: Download-Then-Play
Most reliable fallback:
1. Start ffmpeg, write to cache file
2. Once complete, serve with real Content-Length
3. Player downloads and plays with full seeking

## Recommendation
Try estimated Content-Length approach first. If VLC hangs, fall back to download-then-play.
