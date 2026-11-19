#!/bin/bash
# Wake-ups and CPU time of rtspLive while one client streams (10 s), Stream Audio on and off.
# (Host numbers: they compare versions of the polling, they are not goggle numbers.)
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/cpu
rm -rf "$L"; mkdir -p "$L"
cleanup_ipc

# voluntary context switches of all threads, CPU ticks (utime + stime)
sample() {
    local cs=0 f
    for f in /proc/"$1"/task/*/status; do
        cs=$(( cs + $(awk '/^voluntary_ctxt_switches/ { print $2 }' "$f") ))
    done
    echo "$cs $(awk '{ print $14 + $15 }' /proc/"$1"/stat)"
}

for audio in 1 0; do
    write_conf "$L/$audio.conf" $audio 0
    start_record "$L/$audio.conf" "$L/record_$audio.log"
    start_live "$L/$audio.conf" "$L/rtspLive_$audio.log"
    capture "$L/cap_$audio.nut" 16 &
    CAP=$!
    sleep 4
    read -r c0 t0 <<< "$(sample "$LIVE_PID")"
    sleep 10
    read -r c1 t1 <<< "$(sample "$LIVE_PID")"
    wait $CAP
    hz=$(getconf CLK_TCK)
    echo "Stream Audio $([ $audio = 1 ] && echo on || echo off): rtspLive $(( (c1 - c0) / 10 )) wake-ups/s, CPU $(awk -v t=$(( t1 - t0 )) -v hz="$hz" 'BEGIN { printf "%.1f", 100 * t / hz / 10 }') % of one core"
    stop_live
    stop_record
    cleanup_ipc
done
