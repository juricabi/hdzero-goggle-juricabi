#!/bin/bash
# The audio input (AI) device and its users, in the real record main loop (src/record/main.c):
#   R  recording with audio        goggle message record start / stop
#   S  live stream audio           stream_reader connects / leaves the audio side of the ring
#   B  HDMI out audio bridge       HDMIA_cmdSTART / HDMIA_cmdSTOP, route "ai" (a channel on the AI device)
#   L  AI -> AO loop               AIO_cmdSTART / AIO_cmdSTOP
# The device must be enabled exactly while someone uses it. The stub AI device (record_stubs.c)
# logs "AIDEV VIOLATION" for a disable while another channel still uses it and "AIDEV LEAK" for a
# device left enabled with no channel (also at exit); after each scenario no channel may be left
# and the device must be off.
#   1. every order of starting and stopping R, S and B (90 orders)
#   2. R, S, B and L running, stopped in every order (24)
#   3. the bridge's route replaced by the HDMI-in route (no AI channel), with and without R
#   4. a bridge start that fails after taking its channel, with and without R
#   5. the daemon quits with every set of R, S, B, L running (16)
# Uses record_host_fast (stream audio stops 300 ms after the reader leaves, not 3 s).
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
L=$LOGS/aidev
rm -rf "$L"; mkdir -p "$L"
CTRL=$WORK/ctrl_aidev
rm -rf "$CTRL"; mkdir -p "$CTRL"
cleanup_ipc
rm -f /tmp/live.pid
REC_AUDIO=1 write_conf "$L/rec.conf" 1 0
LOG=

start_rec() { # LOG
    local run=$WORK/run/rec_aidev
    LOG=$1
    rm -rf "$run"; mkdir -p "$run"
    cp "$L/rec.conf" "$run/record.conf"
    (cd "$run" && LAT_MEDIA=$MEDIA LAT_CTRL=$CTRL exec stdbuf -oL "$BIN/record_host_fast" record.conf > "$LOG" 2>&1) &
    REC_PID=$!
    for _ in $(seq 50); do grep -q "movies index" "$LOG" 2>/dev/null && break; sleep 0.1; done
}

count() { grep -c -- "$1" "$LOG" 2>/dev/null; }
# wait until PATTERN has appeared N times (at most 5 s)
wait_count() {
    for _ in $(seq 100); do [ "$(count "$1")" -ge "$2" ] && return 0; sleep 0.05; done
    echo "    timeout waiting for '$1' x$2"
    return 1
}
SR_PID=
ev() { # event: R+ R- S+ S- B+ B- L+ L-
    local n
    case $1 in
    R+) n=$(count "record_dumpAeParams] aenc params"); "$BIN/gmsg" record start; wait_count "record_dumpAeParams] aenc params" $((n + 1)) ;;
    R-) n=$(count "vi2venc_stop (record"); "$BIN/gmsg" record stop; wait_count "vi2venc_stop (record" $((n + 1)) ;;
    S+) n=$(count "liveaudio_start] started"); "$BIN/stream_reader" >> "$L/stream_reader.log" 2>&1 &
        SR_PID=$!; wait_count "liveaudio_start] started" $((n + 1)) ;;
    S-) n=$(count "liveaudio_stop] stopped"); kill "$SR_PID"; wait "$SR_PID" 2>/dev/null; SR_PID=
        wait_count "liveaudio_stop] stopped" $((n + 1)) ;;
    B+) n=$(count "pcmbridge start"); "$BIN/gmsg" record hdmia_start; wait_count "pcmbridge start" $((n + 1)) ;;
    B-) n=$(count "pcmbridge stopped"); "$BIN/gmsg" record hdmia_stop; wait_count "pcmbridge stopped" $((n + 1)) ;;
    L+) n=$(count "ai2ao_start"); "$BIN/gmsg" record aio_start; wait_count "ai2ao_start" $((n + 1)) ;;
    L-) n=$(count "ai2ao_stop"); "$BIN/gmsg" record aio_stop; wait_count "ai2ao_stop" $((n + 1)) ;;
    esac
    echo "  -- $1" >> "$LOG"
}

# a scenario: its events, then nothing may be left on the device and nothing may have gone wrong
NSCEN=0
BAD=0
scenario() { # name events...
    local name=$1; shift
    local from=$(($(wc -l < "$LOG") + 1))
    echo "  == $name" >> "$LOG"
    for e in "$@"; do ev "$e" || { BAD=$((BAD + 1)); echo "  FAIL: $name: event $e did not happen"; return; }; done
    local part last
    part=$(tail -n +"$from" "$LOG")
    last=$(grep "AIDEV" <<< "$part" | tail -1)
    NSCEN=$((NSCEN + 1))
    if grep -q "AIDEV VIOLATION\|AIDEV LEAK" <<< "$part"; then
        BAD=$((BAD + 1)); echo "  FAIL: $name: $(grep "AIDEV VIOLATION\|AIDEV LEAK" <<< "$part" | head -1)"
    elif [ -n "$last" ] && ! grep -q "(0 in use, disable=1, device off)" <<< "$last"; then
        BAD=$((BAD + 1)); echo "  FAIL: $name: left as: $last"
    fi
}

echo "== 1. every order of R, S, B start/stop"
start_rec "$L/record_orders.log"
ORDERS=$(python3 - << 'EOF'
import itertools
seen = set()
for p in itertools.permutations(["R+", "R-", "S+", "S-", "B+", "B-"]):
    if all(p.index(u + "+") < p.index(u + "-") for u in "RSB") and p not in seen:
        seen.add(p)
        print(" ".join(p))
EOF
)
n=0
while read -r order; do
    scenario "$order" $order
    n=$((n + 1))
done <<< "$ORDERS"
check $([ "$n" = 90 ] && [ "$BAD" = 0 ]; echo $?) "$n orders of R, S, B: device on exactly while used, off at the end ($BAD bad)"

echo "== 2. R, S, B, L running, stopped in every order"
BAD=0
n=0
for stops in $(python3 -c 'import itertools; print(" ".join(",".join(p) for p in itertools.permutations(["R-","S-","B-","L-"])))'); do
    scenario "L+ R+ S+ B+ then $stops" L+ R+ S+ B+ ${stops//,/ }
    n=$((n + 1))
done
check $([ "$n" = 24 ] && [ "$BAD" = 0 ]; echo $?) "$n stop orders of all four users ($BAD bad)"

echo "== 3. HDMI out route replaced by the HDMI-in route (no AI channel)"
BAD=0
rm -f "$CTRL/hdmia_src"
ev B+; echo "hw:snddaudio2" > "$CTRL/hdmia_src"; scenario "B+(ai) B+(hw:snddaudio2), nothing else" B+
ev B-
rm -f "$CTRL/hdmia_src"
ev R+; ev B+; echo "hw:snddaudio2" > "$CTRL/hdmia_src"; ev B+; scenario "R+ B+(ai) B+(hw:snddaudio2) R-" R-
ev B-
rm -f "$CTRL/hdmia_src"
check $([ "$BAD" = 0 ]; echo $?) "route replacement releases the AI channel and disables the device only when free"
grep -q "pcmbridge stopped (ai, disableAiDev=1)" "$LOG" && grep -q "pcmbridge stopped (ai, disableAiDev=0)" "$LOG"
check $? "  (the replacement disabled the device alone, kept it with a recording)"

echo "== 4. a bridge start that fails after taking its channel"
BAD=0
touch "$CTRL/hdmia_fail"
scenario "B+ fails, nothing else" B+
scenario "R+ B+ fails R-" R+ B+ R-
scenario "S+ B+ fails S-" S+ B+ S-
rm -f "$CTRL/hdmia_fail"
check $([ "$BAD" = 0 ]; echo $?) "a failed bridge start: device disabled only when nobody else uses it"
"$BIN/gmsg" record quit
wait "$REC_PID" 2>/dev/null
grep -q "AIDEV at exit: no channel, device disabled" "$LOG"; check $? "  (quit afterwards: nothing left)"

echo "== 5. quit with any set of users running"
BAD=0
n=0
for mask in $(seq 0 15); do
    users=""
    [ $((mask & 1)) != 0 ] && users="$users R+"
    [ $((mask & 2)) != 0 ] && users="$users S+"
    [ $((mask & 4)) != 0 ] && users="$users B+"
    [ $((mask & 8)) != 0 ] && users="$users L+"
    cleanup_ipc
    start_rec "$L/record_exit_$mask.log"
    for e in $users; do ev "$e" || BAD=$((BAD + 1)); done
    "$BIN/gmsg" record quit
    wait "$REC_PID" 2>/dev/null
    [ -n "$SR_PID" ] && { kill "$SR_PID"; wait "$SR_PID" 2>/dev/null; SR_PID=; }
    n=$((n + 1))
    if grep -q "AIDEV VIOLATION\|AIDEV LEAK" "$LOG" || ! grep -q "AIDEV at exit: no channel, device disabled" "$LOG"; then
        BAD=$((BAD + 1))
        echo "  FAIL: quit with${users:- nothing}: $(grep "AIDEV VIOLATION\|AIDEV LEAK\|AIDEV at exit" "$LOG" | head -2 | tr '\n' ' ')"
    fi
done
check $([ "$n" = 16 ] && [ "$BAD" = 0 ]; echo $?) "quit with each of the $n sets of users: every channel released, device off ($BAD bad)"

! cat "$L"/record_*.log | grep -q "AIDEV VIOLATION\|AIDEV LEAK"; check $? "no AIDEV VIOLATION / LEAK anywhere in the logs"
echo "  scenarios checked: $NSCEN + 16 exits"
cleanup_ipc
rm -f /tmp/live.pid
summary
