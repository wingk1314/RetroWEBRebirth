#!/bin/bash
# vlc-youtube.sh - Open YouTube video in VLC via VintageStreamProxy
# Run this on the iMac G4

VSP_SERVER="${VSP_SERVER:-http://192.168.1.100:33000}"
PROFILE="${1:-default}"

# Extract video ID from various YouTube URL formats
extract_video_id() {
    local url="$1"
    local video_id=""
    
    # youtube.com/watch?v=VIDEO_ID
    if [[ "$url" =~ youtube\.com/watch\?v=([a-zA-Z0-9_-]{11}) ]]; then
        video_id="${BASH_REMATCH[1]}"
    # youtu.be/VIDEO_ID
    elif [[ "$url" =~ youtu\.be/([a-zA-Z0-9_-]{11}) ]]; then
        video_id="${BASH_REMATCH[1]}"
    # youtube.com/embed/VIDEO_ID
    elif [[ "$url" =~ youtube\.com/embed/([a-zA-Z0-9_-]{11}) ]]; then
        video_id="${BASH_REMATCH[1]}"
    # Bare video ID (11 characters)
    elif [[ "$url" =~ ^[a-zA-Z0-9_-]{11}$ ]]; then
        video_id="$url"
    fi
    
    echo "$video_id"
}

# Check if URL/ID was provided
if [ -z "$1" ]; then
    echo "Usage: $0 <youtube-url-or-id> [profile]"
    echo ""
    echo "Examples:"
    echo "  $0 https://www.youtube.com/watch?v=dQw4w9WgXcQ"
    echo "  $0 dQw4w9WgXcQ"
    echo "  $0 dQw4w9WgXcQ mpeg1"
    echo ""
    echo "Profiles: default, win98, mpeg1, xvid, potato, spud"
    echo ""
    echo "Environment:"
    echo "  VSP_SERVER=$VSP_SERVER"
    exit 1
fi

VIDEO_ID=$(extract_video_id "$1")

if [ -z "$VIDEO_ID" ]; then
    echo "Error: Could not extract video ID from: $1"
    exit 1
fi

STREAM_URL="${VSP_SERVER}/${PROFILE}?v=${VIDEO_ID}"

echo "Video ID: $VIDEO_ID"
echo "Profile: $PROFILE"
echo "Stream URL: $STREAM_URL"
echo ""

# Try to open in VLC
if command -v vlc &> /dev/null; then
    echo "Opening in VLC..."
    vlc "$STREAM_URL" &
elif [ -f "/Applications/VLC.app/Contents/MacOS/VLC" ]; then
    echo "Opening in VLC..."
    open -a VLC "$STREAM_URL"
elif [ -f "/usr/bin/open" ]; then
    echo "Trying to open URL..."
    open "$STREAM_URL"
else
    echo "VLC not found. Please open this URL manually in VLC:"
    echo "$STREAM_URL"
fi
