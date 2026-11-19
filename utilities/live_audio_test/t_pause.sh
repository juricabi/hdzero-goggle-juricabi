#!/bin/bash
# juricabi: at a source switch the app stops the live stream encoding (LIVE_cmdSTOP, ripples 1.2.2's
# REC_STOP_LIVE: the recorder stops the live video encoder and avshare_reset() flushes the ring for the next
# video bitstream), and the recorder starts it again by itself while a client reads; gmsg stands in for the app.
# The stream audio runs on through it: its chain is not stopped, and its side of the ring is not reset
# (avshare_reset() flushes the video side only): no audio frame is lost, the largest pts gap stays below 1.5 AAC
# frames (1024 samples at 48 kHz = 21.3 ms).
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/pause
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc

# audio packet gaps of a capture: "max gap in ms, packets" (as in t_share.sh)
audio_gaps() {
    ffprobe -v error -select_streams a -show_entries packet=pts_time -of csv=p=0 "$1" |
        awk 'NR > 1 { d = $1 - p; if (d > m) m = d } { p = $1; n++ } END { printf "%.0f %d\n", 1000 * m, n }'
}
# audio packets whose pts goes backwards
audio_back() {
    ffprobe -v error -select_streams a -show_entries packet=pts_time -of csv=p=0 "$1" |
        awk 'NR > 1 && $1 < p { b++ } { p = $1 } END { print b + 0 }'
}

write_conf "$L/live.conf" 1 0
write_conf "$L/rec.conf" 1 0
echo "== streaming with audio; +5 s the live encoding stopped (a source switch)"
start_record "$L/rec.conf" "$L/record.log"
start_live "$L/live.conf" "$L/rtspLive.log"
capture "$L/cap.nut" 16 &
CAP=$!
for _ in $(seq 100); do grep -q "liveaudio_onFrame\] first frame" "$L/record.log" && break; sleep 0.1; done
sleep 4
n=$(wc -l < "$L/record.log")
"$BIN/gmsg" record live_stop
check $? "live stop sent"
wait $CAP
tail -n +"$((n + 1))" "$L/record.log" | grep -q "vi2venc_stop (live"; check $? "the live video encoder was stopped"
! tail -n +"$((n + 1))" "$L/record.log" | grep -q "liveaudio_stop\] stopped"; check $? "the stream audio chain kept running"
read -r gap np <<< "$(audio_gaps "$L/cap.nut")"
back=$(audio_back "$L/cap.nut")
echo "  capture: $(packet_counts "$L/cap.nut"), largest audio gap $gap ms, $back audio packets going back in time"
[ "$gap" -le 32 ]; check $? "no stream audio frame lost through the live stop (largest gap $gap ms, at most 1.5 frames: 32 ms)"
[ "$back" = 0 ]; check $? "audio packets in order"
vlast=$(ffprobe -v error -select_streams v -show_entries packet=pts_time -of csv=p=0 "$L/cap.nut" | sort -n | tail -1)
awk -v t="$vlast" 'BEGIN { exit !(t >= 12) }'; check $? "video back after the live stop (last video pts $vlast s)"
stop_live
stop_record
cleanup_ipc
summary
