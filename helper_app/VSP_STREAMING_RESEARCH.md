# VSP Streaming Research: Seeking During Stream

## Problem
User wants seeking/scrubbing during first play while the stream is still being prepared.

## Status (2026-08)
**RESOLVED — implemented and production-proven.** The final architecture uses **PowerVLC direct passthrough** (≤360p) and **DASH remux** (480p/720p) with **estimated Content-Length** for full-timeline scrubbing. Details below are the research trail; the "Recommended Solution" section now matches what shipped.

## Key Findings

### 1. VLC/PowerVLC Behavior
- VLC reads `Content-Length` header to determine total file duration
- Shows seekbar based on Content-Length
- **Can seek to un-downloaded portions** by sending new HTTP `Range` request
- Server responds with `206 Partial Content` from available data
- **Critical**: container framing must be parseable at the requested offset — MP4 needs moov atom at file beginning; MPEG-TS uses Annex B start-codes (why `-bsf:v h264_mp4toannexb` is mandatory)

### 2. ffmpeg File Size Prediction
- **Estimated size**: `duration_seconds × (video_bitrate + audio_bitrate) / 8 + overhead`
- With `-maxrate` and `-bufsize`, estimation is within ±5-10%
- For remux mode: `est_size = (video_format_filesize + audio_format_filesize) * 1.05` from `yt-dlp -J` — much more accurate than bitrate math

### 3. GitHub Projects Found
| Project | Approach | Notes |
|---------|----------|-------|
| mp4-pseudo-streaming | Pre-calculate size, serve with Content-Length | PHP-based |
| video-streaming-server | ffmpeg + HTTP server | Node.js |
| yt-dlp-web-ui | Web UI for yt-dlp | Go-based |

### 4. Recommended Solution: Estimated Content-Length (shipped)

**Strategy (as implemented in vsp_replacement.py):**
1. Get format sizes from yt-dlp `-J` JSON (`requested_formats[0/1]` filesize/filesize_approx/tbr)
2. Compute `est_size = (vsize + asize) * 1.05` (TS overhead); fallback `tbr × duration / 8`
3. Resolve DASH URLs forcing H.264: `bestvideo[height<=N][ext=mp4][vcodec^=avc1]+bestaudio[ext=m4a]`
4. Start ffmpeg remux: `-c copy -bsf:v h264_mp4toannexb -f mpegts` (mux, NOT re-encode)
5. Serve with `Content-Length: <est_size>` and `Accept-Ranges: bytes`
6. Follow the growing file; on Range requests beyond available data, wait up to ~30s for the remux to catch up
7. In-progress registry `remux_procs[(video_id, height)]` prevents restart loops from repeated Range probes

**Key insight:** VLC CAN seek to un-downloaded portions. When you click the seekbar at a position beyond what's been muxed:
- VLC sends `Range: bytes=OFFSET-` request
- Server responds with `206 Partial Content` once the remux reaches that offset (or waits)

### 5. Implementation Sketch (superseded by remux mode)

The original sketch served a growing MP4 with `-movflags +faststart`. The shipped implementation instead uses **MPEG-TS** remux, which is parseable while growing (no moov atom dependency) and keeps `-c copy` (no re-encode). For direct passthrough (≤360p), no ffmpeg at all — the YouTube CDN URL is Range-forwarded (format 18 progressive, native 206 support).

### 6. Risks (addressed)
- **VLC may hang** if Content-Length is much larger than actual data → mitigated by accurate `yt-dlp -J` sizes + wait-and-serve on far Ranges
- **moov atom size** hard to predict → avoided entirely via MPEG-TS
- **AV1 stream breaking Annex B** → `[vcodec^=avc1]` forces H.264
- **Restart loops** → `remux_procs` registry + never kill remux on client disconnect

### 7. Alternative: Download-Then-Play
Still valid fallback for legacy profile mode (ffmpeg transcode for MPlayer fallback): transcode to cache file, serve with real Content-Length once complete.
