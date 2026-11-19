#!/bin/bash
# rtspLive ends while a client streams (Stream Audio on): the record side must stop the live
# video encoder and the live audio chain, whatever the way rtspLive ends.
#   quit  goggle message MSG_cmdQUIT (WiFi off: gogglecmd -live quit)
#   TERM  killall rtspLive (wlan_stop.sh one second after the quit message)
#   KILL  no handler at all (also what a crash or an rtspLive without the exit handlers leaves)
# Then a new rtspLive must stream both tracks again.
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/kill
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc

# wait_log FILE PATTERN AFTER_LINE TIMEOUT -> seconds until a line matching PATTERN after line AFTER_LINE
wait_log() {
    local t0 t
    t0=$(date +%s.%N)
    while :; do
        if tail -n +"$(( $3 + 1 ))" "$1" | grep -q "$2"; then
            t=$(date +%s.%N); echo "$t - $t0" | bc | sed 's/^\./0./'; return 0
        fi
        if [ "$(echo "$(date +%s.%N) - $t0 > $4" | bc)" = 1 ]; then echo "timeout"; return 1; fi
        sleep 0.05
    done
}
le() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a <= b) }'; }

write_conf "$L/live.conf" 1 0
write_conf "$L/rec.conf" 1 0
for how in quit TERM KILL; do
    echo "== rtspLive ends by $how while streaming"
    start_record "$L/rec.conf" "$L/record_$how.log"
    start_live "$L/live.conf" "$L/rtspLive_$how.log"
    capture "$L/cap_$how.nut" 20 &
    CAP=$!
    for _ in $(seq 100); do grep -q "liveaudio_onFrame\] first frame" "$L/record_$how.log" && break; sleep 0.1; done
    sleep 3
    n=$(wc -l < "$L/record_$how.log")
    case $how in
        quit) gmsg_live quit ;;
        *) kill -"$how" "$LIVE_PID" ;;
    esac
    tv=$(wait_log "$L/record_$how.log" "vi2venc_stop (live" "$n" 8)
    ta=$(wait_log "$L/record_$how.log" "liveaudio_stop\] stopped" "$n" 8)
    echo "  live video encoder stopped after $tv s, live audio chain after $ta s"
    lim=1.0; [ $how = KILL ] && lim=1.5
    le "${tv/timeout/99}" $lim; check $? "$how: live video encoder stopped within $lim s [$tv s]"
    lim=4.0; [ $how = KILL ] && lim=4.5
    le "${ta/timeout/99}" $lim; check $? "$how: live audio chain stopped within $lim s (3 s stop delay) [$ta s]"
    wait "$LIVE_PID" 2>/dev/null; LIVE_PID=
    kill "$CAP" 2>/dev/null; wait "$CAP" 2>/dev/null
    sleep 1
    m=$(wc -l < "$L/record_$how.log")
    sleep 2
    ! tail -n +"$m" "$L/record_$how.log" | grep -q "ring full\|vvLive_onFrame\|first frame"; check $? "$how: nothing written to the ring afterwards"

    # a new rtspLive streams again
    start_live "$L/live.conf" "$L/rtspLive_${how}_2.log"
    s=$(probe_streams "$URL" | tr '\n' ' ')
    echo "  new rtspLive: $s"
    grep -q "codec_name=h264" <<< "$s" && grep -q "codec_name=aac" <<< "$s"; check $? "$how: a new rtspLive streams video and audio"
    stop_live
    stop_record
    ! grep -q "AIDEV VIOLATION\|AIDEV LEAK" "$L/record_$how.log"; check $? "$how: AI device used correctly"
    cleanup_ipc
done
summary
