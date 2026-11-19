#!/bin/bash
# juricabi: the app's DVR control (src/core/dvr.c) hands the recorder config snapshots, copies of
# record.conf made when the app started (ctl_conf stands in for it). Stream Audio, which the WiFi page
# writes into record.conf later, must still decide the stream: the recorder reads [live] from the file it
# chose at its start, not from the snapshot.
#  1. a snapshot with [live] audio=0 accepted, then Stream Audio On (the start file: audio=1) and rtspLive
#     with audio=1: the chain starts, the stream announces and carries audio
#  2. a snapshot with [live] audio=1 accepted, then Stream Audio Off (the start file: audio=0) and an
#     rtspLive still offering audio: the recorder does not start the chain
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/snapshot
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc
START=$WORK/run/rec/record.conf # the recorder's start file (start_record copies the conf there)
write_conf "$L/live_on.conf" 1 0

echo "== 1. snapshot [live] audio=0, then Stream Audio On"
write_conf "$L/start_off.conf" 0 0
start_record "$L/start_off.conf" "$L/record_1.log"
"$BIN/ctl_conf" "$L/start_off.conf" 1 > "$L/ctl_1.out" 2>&1
check $? "the recorder accepts the snapshot: $(cat "$L/ctl_1.out")"
write_conf "$START" 1 0 # the WiFi page's Apply: [live] audio=1 in record.conf
start_live "$L/live_on.conf" "$L/rtspLive_1.log"
r=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  describe:   $r"
grep -q "m=video,audio$" <<< "$r"; check $? "SDP with audio"
sleep 1
grep -q "started: dev=" "$L/record_1.log"; check $? "the recorder started the audio chain"
! grep -q "\[live\] audio is off" "$L/record_1.log"; check $? "no 'audio is off' in the record log"
n=$(grep -c "first frame (len=" "$L/record_1.log")
[ "$n" -ge 1 ]; check $? "audio frames reach the ring ($n log lines)"
stop_live
stop_record

echo "== 2. snapshot [live] audio=1, then Stream Audio Off (rtspLive still offering audio)"
write_conf "$L/start_on.conf" 1 0
start_record "$L/start_on.conf" "$L/record_2.log"
"$BIN/ctl_conf" "$L/start_on.conf" 1 > "$L/ctl_2.out" 2>&1
check $? "the recorder accepts the snapshot: $(cat "$L/ctl_2.out")"
write_conf "$START" 0 0 # the WiFi page's Apply: [live] audio=0 in record.conf
start_live "$L/live_on.conf" "$L/rtspLive_2.log"
r=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  describe:   $r"
grep -q "m=video$" <<< "$r"; check $? "video-only SDP (no audio frame within the wait)"
sleep 1
grep -q "\[live\] audio is off" "$L/record_2.log"; check $? "the record log says the setting is off"
! grep -q "started: dev=" "$L/record_2.log"; check $? "no audio chain started"
stop_live
stop_record
cleanup_ipc
summary
