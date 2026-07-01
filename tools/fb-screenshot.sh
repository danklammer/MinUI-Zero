#!/bin/sh
# fb-screenshot.sh — capture the TrimUI Brick's live screen over SSH by reading /dev/fb0.
# The Brick's display is a plain framebuffer (1024x768, 32bpp little-endian XRGB8888 => byte order BGRA,
# 4096-byte stride). This gives remote visual verification — essential for validating any present-path
# change (e.g. the GPU-dark fbdev present) when you're not holding the device.
#
# Usage: tools/fb-screenshot.sh [user@host] [out.png]
# Needs: ssh key access to the device, and ffmpeg on the host (brew install ffmpeg).
set -e
HOST="${1:-root@192.168.1.90}"
OUT="${2:-screen.png}"
KEY="$HOME/.ssh/tg5040_dev"
TMP="$(mktemp -d)"

# Read the visible 1024x768 page. Double-buffering pans between yoffset 0 and 768; page 0 is a recent
# frame either way. (For a tear-free grab you'd FBIOGET_VSCREENINFO the live yoffset; page 0 is fine here.)
ssh -i "$KEY" -o BatchMode=yes "$HOST" 'dd if=/dev/fb0 bs=4096 skip=0 count=768 of=/tmp/fb.raw 2>/dev/null'
scp -i "$KEY" -o BatchMode=yes -q "$HOST:/tmp/fb.raw" "$TMP/fb.raw"

ffmpeg -y -loglevel error -f rawvideo -pixel_format bgra -video_size 1024x768 -i "$TMP/fb.raw" "$OUT"
rm -rf "$TMP"
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
