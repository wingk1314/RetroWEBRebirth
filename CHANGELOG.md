# Changelog

## [Unreleased]

### Added
- **YouTube video stream blocking** — `BlockMediaHandler` intercepts `googlevideo.com/videoplayback` requests via `CefResourceRequestHandler::OnBeforeResourceLoad`, returning `RV_CANCEL` to prevent in-browser video autoplay. Only active when `--vlc-enabled=YES`. No JavaScript injection required.
- **VSP ffmpeg cleanup on disconnect** — Both disconnect handlers now terminate the ffmpeg process when the client (iMac G4) closes the connection, preventing orphaned transcoding processes.
- **Placeholder IPs in docs** — All real IP addresses and credentials replaced with `SERVER_IP`, `IMAC_IP`, and `user@` placeholders across README, HOWTO, INTEGRATION, and README_RETRO.

### Changed
- **MPlayer Extended cache** — Reduced `MPECacheSizeInMB` from 100MB to 8MB via `defaults write`, enabling near-instant playback on live streams.
- **GitHub repo** — Renamed from `RetroREBIRTH` to `RetroWEBRebirth`, set to public visibility.
- **README** — Updated to reflect corrected helper behavior (launches on red play button click, not auto-detection) and YouTube auto-pause feature description.

### Fixed
- **ffmpeg orphan processes** — VSP now kills ffmpeg when the iMac client disconnects (BrokenPipeError handler).
- **GitHub repo security** — All real IPs/credentials removed from docs, repos visibility updated (`RetroWEBRebirth` public, `browser-history-notion` private).

## [2026-07-27]

### Added
- Initial working build of Browservice with YouTube → MPlayer integration
- VSP (Video Stream Processor) with yt-dlp + ffmpeg transcoding to MPEG-TS
- Helper app on iMac G4 launching MPlayer OSX Extended
- Red play button triggering MPlayer via Browservice control bar
- Cache-based serving (first play streams live, second play from cache with seeking)
