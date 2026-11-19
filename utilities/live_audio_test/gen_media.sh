#!/bin/bash
# Test media for the host tests (needs ffmpeg with libx264, libx265 and the native aac encoder).
# One 32 s timeline (a multiple of both 1/30 s and 1024/48000 s, so the loops of both media stay
# aligned): a moving test pattern at 30 fps, 2 Mbit/s CBR (I frames span several ring slots, x264
# writes 4 slices per picture), an I frame every 0.5 s (the stub encoder starts at the next one),
# with a white frame at t = s + o(s) for s = 1..31, o(s) = floor(15 * frac(0.618034 s)) / 30
# (aperiodic, so a wrong pairing cannot look right), and a 100 ms 1 kHz beep starting at the
# same instants. source_ref.mkv muxes the same elementary streams: the A/V offset measured in it
# (the AAC encoder delay, about 21 ms) is the reference for the stream captures.
set -e
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
mkdir -p "$MEDIA"
cd "$MEDIA"
[ -f video.h264 ] && [ -f video.h265 ] && [ -f audio.aac ] && [ -f source_ref.mkv ] && [ -z "$FORCE" ] && exit 0

FLASH=$(python3 -I -c '
import math
print("+".join("eq(n,%d)" % round(30 * (s + math.floor(15 * ((s * 0.618034) % 1)) / 30)) for s in range(1, 32)))')
BEEP=$(python3 -I -c '
import math
print("+".join("between(t,%.6f,%.6f)" % (t, t + 0.1) for t in (s + math.floor(15 * ((s * 0.618034) % 1)) / 30 for s in range(1, 32))))')

VF="lutyuv=y=val*0.45,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='$FLASH'"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=640x360:r=30:d=32" -vf "$VF" -frames:v 960 \
    -c:v libx264 -preset veryfast -tune zerolatency -profile:v main -g 15 -keyint_min 15 -sc_threshold 0 -bf 0 \
    -b:v 2M -maxrate 2M -bufsize 1M -pix_fmt yuv420p -f h264 video.h264
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=s=640x360:r=30:d=32" -vf "$VF" -frames:v 960 \
    -c:v libx265 -preset veryfast -tune zerolatency -pix_fmt yuv420p -b:v 2M \
    -x265-params "keyint=15:min-keyint=15:bframes=0:scenecut=0:vbv-maxrate=2000:vbv-bufsize=1000:log-level=error" \
    -f hevc video.h265
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "aevalsrc='0.5*sin(2*PI*1000*t)*($BEEP)|0.5*sin(2*PI*1000*t)*($BEEP)':s=48000:d=33" \
    -frames:a 1500 -c:a aac -b:a 64k -f adts audio.aac
ffmpeg -hide_banner -loglevel error -y -r 30 -f h264 -i video.h264 -f aac -i audio.aac -map 0 -map 1 -c copy source_ref.mkv
ls -la "$MEDIA"
