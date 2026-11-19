#!/bin/bash
# The audio chain fails for good while a client streams (stub: "$LAT_CTRL/ai_fail" stops the AI
# capture and makes every start fail), then comes back:
#  - record restarts a chain that stopped producing, and keeps retrying a failing start;
#  - rtspLive gives the audio track up after 6 s without frames: a new DESCRIBE leaves it out
#    (after one bounded wait) instead of announcing a track that never gets data;
#  - once frames flow again the track is announced again; the streaming client gets audio again.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/audio_fail
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc
CTRL=$WORK/ctrl
mkdir -p "$CTRL"
rm -f "$CTRL/ai_fail"

write_conf "$L/live.conf" 1 0
start_record "$L/live.conf" "$L/record.log"
start_live "$L/live.conf" "$L/rtspLive.log"
capture "$L/cap.nut" 40 &
CAP=$!
t0=$(date +%s.%N)
at() { local d; d=$(echo "$t0 + $1 - $(date +%s.%N)" | bc); [ "${d:0:1}" != - ] && sleep "$d"; }

at 6;  touch "$CTRL/ai_fail"; echo "  +6 s: audio chain fails"
at 14; d1=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  +14 s: $d1"
at 18; rm -f "$CTRL/ai_fail"; echo "  +18 s: audio chain can start again"
at 28; d2=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  +28 s: $d2"
wait $CAP
stop_live
stop_record

grep -q "no audio frame for 6 s" "$L/rtspLive.log"; check $? "rtspLive gave the audio track up after 6 s without frames"
grep -q "m=video$" <<< "$d1"; check $? "DESCRIBE during the failure: video only ($(sed 's/.*DESCRIBE \([0-9.]* s\).*/\1/' <<< "$d1"))"
grep -q "m=video,audio$" <<< "$d2"; check $? "DESCRIBE after the recovery: video and audio ($(sed 's/.*DESCRIBE \([0-9.]* s\).*/\1/' <<< "$d2"))"
grep -q "no audio frame for 2000 ms, restarting the chain" "$L/record.log"; check $? "record restarted the stuck chain"
n=$(grep -c "liveaudio_start\] start failed" "$L/record.log")
[ "$n" -ge 1 ]; check $? "record kept retrying the failing start ($n failed starts)"
# audio packets of the streaming client before the failure, during it, after the recovery
read -r before during after <<< "$(ffprobe -v error -select_streams a -show_entries packet=pts_time -of csv=p=0 "$L/cap.nut" |
    awk '{ if ($1 < 5) b++; else if ($1 < 18) d++; else if ($1 > 26) a++ } END { print b + 0, d + 0, a + 0 }')"
echo "  streaming client, audio packets: 0-5 s $before, 5-18 s $during, after 26 s $after"
[ "$before" -gt 150 ] && [ "$after" -gt 300 ]; check $? "the streaming client has audio before the failure and again after it"
grep -E "live audio: (no audio|AAC)" "$L/rtspLive.log" | sed 's/^/    rtspLive: /'
grep -E "restarting|start failed|liveaudio_start\] started" "$L/record.log" | sed 's/^[ 0-9.]*\[rec [a-z_]*\] /    record: /'
cleanup_ipc
summary
