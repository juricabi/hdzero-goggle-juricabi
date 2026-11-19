#!/bin/bash
# Stream Audio off: the SDP and the RTP packetization of this rtspLive against the rtspLive of a
# base commit (default 8364a43, before the stream audio), both native against the same host
# live555, fed the same frames (LAT_VIDEO_FROM_START) by the same record_host.
#   usage: t_off_identical.sh [BASE_COMMIT]     (needs LIVE555)
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
BASE=${1:-8364a43}
L=$LOGS/off_identical
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc
[ -n "$LIVE555" ] || { echo "LIVE555 not set"; exit 2; }

# build the base rtspLive from the commit's tree (git archive: no checkout, no worktree)
B=$WORK/build/base_$BASE
rm -rf "$B"; mkdir -p "$B/src"
git -C "$REPO" archive "$BASE" src/rtspLive lib/minIni | tar -x -C "$B/src"
LINC="-I$LIVE555/liveMedia/include -I$LIVE555/groupsock/include -I$LIVE555/UsageEnvironment/include -I$LIVE555/BasicUsageEnvironment/include"
S=$B/src
INC="-I$S/src/rtspLive -I$S/src/rtspLive/stream -I$S/src/rtspLive/server -I$S/lib/minIni/src $LINC"
for f in "$S"/src/rtspLive/stream/*.c "$S"/lib/minIni/src/minIni.c; do gcc -O1 -w -D_GNU_SOURCE -DNO_OPENSSL=1 $INC -c "$f" -o "$B/$(basename "$f").o"; done
for f in "$S"/src/rtspLive/main.cpp "$S"/src/rtspLive/server/*.cpp; do g++ -O1 -w -D_GNU_SOURCE -DNO_OPENSSL=1 $INC -c "$f" -o "$B/$(basename "$f").o"; done
g++ -o "$B/rtspLive" "$B"/*.o "$LIVE555/liveMedia/libliveMedia.a" "$LIVE555/groupsock/libgroupsock.a" \
    "$LIVE555/BasicUsageEnvironment/libBasicUsageEnvironment.a" "$LIVE555/UsageEnvironment/libUsageEnvironment.a" -lpthread
echo "built rtspLive of $BASE"

for codec in h264 h265; do
    h265=$([ $codec = h265 ] && echo 1 || echo 0)
    write_conf "$L/$codec.conf" 0 $h265
    for who in base new; do
        export LAT_VIDEO_FROM_START=1
        start_record "$L/$codec.conf" "$L/record_${codec}_$who.log"
        unset LAT_VIDEO_FROM_START
        RTSPLIVE_BIN=$([ $who = base ] && echo "$B/rtspLive" || echo "$BIN/rtspLive") RTSPLIVE=native \
            start_live "$L/$codec.conf" "$L/rtspLive_${codec}_$who.log"
        python3 -I "$LAT_DIR/rtsp_rtp.py" "$URL" 5 "$L/${codec}_$who" > "$L/${codec}_$who.txt" 2>&1
        stop_live
        stop_record
        cleanup_ipc
    done
    if [ ! -s "$L/${codec}_base.pkts" ] || [ ! -s "$L/${codec}_new.pkts" ]; then
        check 1 "$codec: both servers streamed"; continue
    fi
    grep -v "^o=" "$L/${codec}_base.sdp" > "$L/${codec}_base.sdp.cmp"
    grep -v "^o=" "$L/${codec}_new.sdp" > "$L/${codec}_new.sdp.cmp"
    diff "$L/${codec}_base.sdp.cmp" "$L/${codec}_new.sdp.cmp" > "$L/${codec}_sdp.diff"; check $? "$codec: SDP identical apart from the o= line"
    # packets: marker, payload size, first payload bytes; RTP timestamp steps +-1 (90 kHz rounding)
    n=$(( $(wc -l < "$L/${codec}_base.pkts") < $(wc -l < "$L/${codec}_new.pkts") ? $(wc -l < "$L/${codec}_base.pkts") : $(wc -l < "$L/${codec}_new.pkts") ))
    n=$(( n - 50 ))
    head -n $n "$L/${codec}_base.pkts" | cut -d' ' -f1-4 > "$L/${codec}_base.pkts.cmp"
    head -n $n "$L/${codec}_new.pkts" | cut -d' ' -f1-4 > "$L/${codec}_new.pkts.cmp"
    cmp -s "$L/${codec}_base.pkts.cmp" "$L/${codec}_new.pkts.cmp"; check $? "$codec: first $n RTP packets identical (marker, size, payload header)"
    d=$(paste -d' ' <(head -n $n "$L/${codec}_base.pkts" | cut -d' ' -f5) <(head -n $n "$L/${codec}_new.pkts" | cut -d' ' -f5) |
        awk '{ d = $1 - $2; if (d < 0) d = -d; if (d > 1) n++ } END { print n + 0 }')
    [ "$d" = 0 ]; check $? "$codec: RTP timestamp steps identical within 1 tick ($d differ)"
    echo "  base: $(grep '^video' "$L/${codec}_base.txt")"
    echo "  new:  $(grep '^video' "$L/${codec}_new.txt")"
done
summary
