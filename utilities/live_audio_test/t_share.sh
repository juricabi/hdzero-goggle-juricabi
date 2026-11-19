#!/bin/bash
# The audio input device shared by the stream audio, recording with audio and the AI -> AO loop,
# driven through the real record main loop with goggle messages (as the main app sends them).
# The stub AI device logs any disable while another channel still uses it (AIDEV VIOLATION) and
# a device left enabled with no channel (AIDEV LEAK). The stream audio must not be interrupted.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/share
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc

# audio packet gaps of a capture: "max gap in ms, packets"
audio_gaps() {
    ffprobe -v error -select_streams a -show_entries packet=pts_time -of csv=p=0 "$1" |
        awk 'NR > 1 { d = $1 - p; if (d > m) m = d } { p = $1; n++ } END { printf "%.0f %d\n", 1000 * m, n }'
}

write_conf "$L/live.conf" 1 0
REC_AUDIO=1 write_conf "$L/rec.conf" 1 0

echo "== stream first, then recording and the AI->AO loop start and stop while it runs"
start_record "$L/rec.conf" "$L/record_1.log"
start_live "$L/live.conf" "$L/rtspLive_1.log"
capture "$L/cap_1.nut" 26 &
CAP=$!
step() { sleep "$1"; echo "  +$2: $3"; "$BIN/gmsg" record "$3"; }
step 4 4 start
step 4 8 stop
step 2 10 aio_start
step 2 12 start
step 3 15 aio_stop
step 3 18 stop
wait $CAP
sleep 4
stop_live
stop_record
read -r gap n <<< "$(audio_gaps "$L/cap_1.nut")"
echo "  capture: $(packet_counts "$L/cap_1.nut"), largest audio gap $gap ms"
[ "$gap" -le 100 ]; check $? "stream audio not interrupted by recording / loop start and stop (largest gap $gap ms)"
[ "$(grep -c "ai2aenc_start" "$L/record_1.log")" = 3 ]; check $? "live chain started once, recording audio twice"
grep -q "ffpack_close: .* audio frames" "$L/record_1.log" && ! grep -q "muxed so far: [0-9]* video, 0 audio" "$L/record_1.log"
check $? "recordings got audio frames"
! grep -q "AIDEV VIOLATION\|AIDEV LEAK" "$L/record_1.log"; check $? "AI device: no disable while in use, not left enabled"
grep "AIDEV" "$L/record_1.log" | sed 's/^/    /'

echo "== recording first, stream starts and ends while it records"
cleanup_ipc
start_record "$L/rec.conf" "$L/record_2.log"
"$BIN/gmsg" record start
sleep 2
start_live "$L/live.conf" "$L/rtspLive_2.log"
capture "$L/cap_2.nut" 6
stop_live
sleep 4.5
"$BIN/gmsg" record stop
sleep 1
stop_record
echo "  capture: $(packet_counts "$L/cap_2.nut")"
grep -q "audio" <<< "$(packet_counts "$L/cap_2.nut")"; check $? "stream has audio while recording with audio"
! grep -q "AIDEV VIOLATION\|AIDEV LEAK" "$L/record_2.log"; check $? "AI device: no disable while in use, not left enabled"
grep "AIDEV" "$L/record_2.log" | sed 's/^/    /'
cleanup_ipc
summary
