#!/bin/bash
# DESCRIBE timing with Stream Audio on (rtspLive [live] audio=1):
#  - the record side produces no audio ([live] audio=0 there): a DESCRIBE waits for the first
#    audio frame at most 2.5 s, DESCRIBEs in the next 10 s do not wait, overlapping DESCRIBEs
#    share one wait (none waits longer than the first one's deadline);
#  - the record side produces audio: DESCRIBE only waits for the chain to start, once.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/describe
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc

secs() { sed -n 's/.*DESCRIBE \([0-9.]*\) s.*/\1/p' <<< "$1"; }
le() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a <= b) }'; }

write_conf "$L/live.conf" 1 0
for mode in noaudio audio; do
    echo "== record side: $mode"
    write_conf "$L/rec_$mode.conf" "$([ $mode = audio ] && echo 1 || echo 0)" 0
    start_record "$L/rec_$mode.conf" "$L/record_$mode.log"
    start_live "$L/live.conf" "$L/rtspLive_$mode.log"

    r1=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  first:      $r1"
    r2=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  second:     $r2"
    if [ $mode = noaudio ]; then
        # r1 also waits for the first video frame (SPS/PPS): the stub encoder starts at its next I frame (<= 0.5 s)
        le "$(secs "$r1")" 3.6; check $? "first DESCRIBE: video start + at most 2.5 s for audio [$(secs "$r1") s]"
        grep -q "m=video$" <<< "$r1"; check $? "video-only SDP"
        le "$(secs "$r2")" 0.3; check $? "DESCRIBE right after a timeout does not wait [$(secs "$r2") s]"
        echo "  (waiting 10.5 s for the retry period to pass)"
        sleep 10.5
        r3=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  retry:      $r3"
        le "$(secs "$r3")" 2.8 && le 2.4 "$(secs "$r3")"; check $? "after the retry period one wait of 2.5 s (video SDP known) [$(secs "$r3") s]"
        echo "  (waiting 10.5 s for the retry period to pass)"
        sleep 10.5
        ov=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0,1.0,1.5); echo "$ov" | sed 's/^/  overlap:    /'
        last=$(sed -n 's/.*done +\([0-9.]*\) s.*/\1/p' <<< "$ov" | sort -n | tail -1)
        le "$last" 2.8; check $? "3 overlapping DESCRIBEs (+0, +1.0, +1.5 s) share one wait, all done by +2.8 s [last +$last s]"
        [ "$(grep -c "m=video$" <<< "$ov")" = 3 ]; check $? "all three video-only"
        r4=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0); echo "  after:      $r4"
        le "$(secs "$r4")" 0.3; check $? "next DESCRIBE does not wait [$(secs "$r4") s]"
        sleep 0.2
        n=$(grep -c "no audio frame within" "$L/rtspLive_$mode.log")
        [ "$n" = 3 ]; check $? "three waits for 8 DESCRIBEs in ~30 s ($n)"
    else
        # includes the video start (<= 0.5 s) and the audio chain start (~0.3 s)
        le "$(secs "$r1")" 1.5; check $? "first DESCRIBE with audio [$(secs "$r1") s]"
        grep -q "m=video,audio$" <<< "$r1"; check $? "SDP with audio"
        le "$(secs "$r2")" 0.3; check $? "second DESCRIBE [$(secs "$r2") s]"
        ov=$(python3 -I "$LAT_DIR/rtsp_describe.py" "$URL" 0,0.05,0.1); echo "$ov" | sed 's/^/  overlap:    /'
        [ "$(grep -c "m=video,audio$" <<< "$ov")" = 3 ]; check $? "3 overlapping DESCRIBEs all announce audio"
    fi
    if [ -x "$OPENRTSP" ]; then
        s=$(date +%s.%N)
        timeout 30 "$OPENRTSP" -r -d 1 "$URL" > /dev/null 2> "$L/openrtsp_$mode.err"
        e=$(date +%s.%N)
        echo "  openRTSP DESCRIBE..TEARDOWN (-d 1): $(echo "$e - $s" | bc) s, m= lines: $(grep -c '^m=' "$L/openrtsp_$mode.err")"
    fi
    stop_live
    stop_record
done
cleanup_ipc
summary
