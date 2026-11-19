#!/bin/bash
# Shared setup of the live stream audio host tests (sourced by build.sh and the t_*.sh tests).
#
# Environment:
#   WORK       output directory (default ${TMPDIR:-/tmp}/live_audio_test; logs in $LOGS = $WORK/logs)
#   LIVE555    built host live555 source tree: native rtspLive and its openRTSP client
#   RTSPLIVE   "native" (default: $WORK/bin/rtspLive, or $RTSPLIVE_BIN) or "arm": the cross-built rtspLive
#              ($ARM_RTSPLIVE, default $REPO/build_goggle2/rtspLive, with the prebuilt live555
#              2022.12.01) under qemu-arm, with the C/C++ runtime from $TOOLCHAIN
#   TOOLCHAIN  buildroot SDK (arm-buildroot-linux-musleabihf), for RTSPLIVE=arm and ring_test.arm
#
# The programs under test use fixed paths and IPC keys of the goggle (/tmp/record.pid,
# /tmp/live.pid, SysV shm 0x568066, message queues 0x5680/0x5682): one test at a time.
LAT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$LAT_DIR/../.." && pwd)
WORK=${WORK:-${TMPDIR:-/tmp}/live_audio_test}
BIN=${BIN:-$WORK/bin}
MEDIA=$WORK/media
LOGS=${LOGS:-$WORK/logs}
PORT=${PORT:-18554}
URL=rtsp://127.0.0.1:$PORT/hdzero
RTSPLIVE=${RTSPLIVE:-native}
ARM_RTSPLIVE=${ARM_RTSPLIVE:-$REPO/build_goggle2/rtspLive}
if [ -z "$TOOLCHAIN" ] && [ -d "$REPO/toolchain" ]; then TOOLCHAIN=$REPO/toolchain; fi
OPENRTSP=${OPENRTSP:-$LIVE555/testProgs/openRTSP}

# write_conf FILE AUDIO(0|1) H265(0|1) [more ini lines...]
write_conf() {
    local f=$1 audio=$2 h265=$3
    shift 3
    mkdir -p "$(dirname "$f")"
    cat > "$f" <<EOF
[record]
disk=/mnt/extsd
audio=${REC_AUDIO:-0}

[live]
multicast=0
port=$PORT
audio=$audio
$(for l in "$@"; do echo "$l"; done)

[venc_live]
h265=$h265

[ai]
device=0
sample_rate=48000
sample_bits=16
channels=2

[aenc]
aac=1
bps=64000
EOF
}

# start_record CONF LOG  (environment: LAT_* knobs of record_stubs.c)
# (record keeps the conf path in 128 bytes: it is passed relative to the run directory)
start_record() {
    local run=$WORK/run/rec
    rm -rf "$run"
    mkdir -p "$run" "$WORK/ctrl"
    cp "$1" "$run/record.conf"
    (cd "$run" && LAT_MEDIA=$MEDIA LAT_CTRL=${LAT_CTRL:-$WORK/ctrl} exec stdbuf -oL "$BIN/record_host" record.conf > "$2" 2>&1) &
    REC_PID=$!
    for _ in $(seq 50); do grep -q "record start" "$2" 2>/dev/null && break; sleep 0.1; done
    sleep 0.3
}

# start_live CONF LOG  -> LIVE_PID (rtspLive as on the goggle: no arguments, server in its own
# thread, the main thread polls the goggle messages; the conf next to the binary or in the ARM root)
start_live() {
    local conf=$1 log=$2 run=$WORK/run/live
    rm -rf "$run"
    mkdir -p "$run/confs"
    cp "$conf" "$run/confs/record.conf"
    if [ "$RTSPLIVE" = arm ]; then
        local sys=$TOOLCHAIN/arm-buildroot-linux-musleabihf/sysroot root=$run/armroot
        mkdir -p "$root/lib" "$root/usr/lib" "$root/data/confs"
        cp "$sys/lib/libc.so" "$root/lib/ld-musl-armhf.so.1" # (musl: the loader is libc.so)
        cp "$sys/lib/libc.so" "$root/lib/libc.so"
        cp -L "$sys/lib/libgcc_s.so.1" "$root/lib/"
        cp -L "$sys/usr/lib/libstdc++.so.6" "$root/usr/lib/"
        cp "$conf" "$root/data/confs/record.conf"
        cp "$ARM_RTSPLIVE" "$run/rtspLive"
        (cd "$run" && exec stdbuf -oL qemu-arm -L "$root" ./rtspLive > "$log" 2>&1) &
    else
        cp "${RTSPLIVE_BIN:-$BIN/rtspLive}" "$run/rtspLive"
        (cd "$run" && exec stdbuf -oL ./rtspLive > "$log" 2>&1) &
    fi
    LIVE_PID=$!
    for _ in $(seq 100); do grep -q "LIVE555 Media Server" "$log" 2>/dev/null && break; sleep 0.1; done
    sleep 0.3
}

# goggle message to rtspLive (quit), from a sender of the same word size
gmsg_live() {
    if [ "$RTSPLIVE" = arm ]; then qemu-arm "$BIN/gmsg.arm" live "$1"; else "$BIN/gmsg" live "$1"; fi
}

# like WiFi off: the quit message (rtspLive exits normally), then SIGTERM if it is still there
stop_live() {
    if [ -n "$LIVE_PID" ]; then
        gmsg_live quit 2>/dev/null
        for _ in $(seq 30); do kill -0 "$LIVE_PID" 2>/dev/null || break; sleep 0.1; done
        kill "$LIVE_PID" 2>/dev/null
        wait "$LIVE_PID" 2>/dev/null
    fi
    LIVE_PID=
}

stop_record() {
    if [ -n "$REC_PID" ]; then
        "$BIN/gmsg" record quit 2>/dev/null
        for _ in $(seq 50); do kill -0 "$REC_PID" 2>/dev/null || break; sleep 0.1; done
        kill "$REC_PID" 2>/dev/null
        wait "$REC_PID" 2>/dev/null
    fi
    REC_PID=
}

# leftovers of killed processes: the ring segment and the message queues
cleanup_ipc() {
    pkill -x rtspLive 2>/dev/null
    pkill -f "qemu-arm -L $WORK/run/live" 2>/dev/null
    pkill -f "$BIN/record_host" 2>/dev/null
    for k in 0x00568066; do ipcrm -M $k 2>/dev/null; done
    for k in 0x00005680 0x00005682; do ipcrm -Q $k 2>/dev/null; done
    true
}

# ffprobe/ffmpeg capture helpers
probe_streams() { # URL
    timeout -k 3 25 ffprobe -v error -rtsp_transport tcp -show_entries stream=codec_name,profile,sample_rate,channels \
        -of compact "$1" 2>&1
}
capture() { # OUT SECONDS [transport]
    timeout -k 5 $(( $2 + 25 )) ffmpeg -hide_banner -loglevel error -rtsp_transport "${3:-tcp}" -i "$URL" -t "$2" \
        -map 0 -c copy -y "$1" 2> "$1.err"
}
packet_counts() { # FILE -> "video N audio M"
    ffprobe -v error -show_entries stream=index,codec_type -of csv=p=0 "$1" > "$1.streams" 2>/dev/null
    local out=""
    while IFS=, read -r idx type; do
        out="$out $type $(ffprobe -v error -select_streams "$idx" -show_entries packet=stream_index -of csv=p=0 "$1" | wc -l)"
    done < "$1.streams"
    echo "${out# }"
}
# decoder errors, each stream on its own (a capture's container timestamps are not checked here)
decode_errors() {
    local n=0 t
    for t in v a; do
        n=$(( n + $(ffmpeg -v error -i "$1" -map "0:$t?" -f null - 2>&1 | wc -l) ))
    done
    echo $n
}

PASS=0
FAIL=0
check() { # condition-result(0/1) message
    if [ "$1" = 0 ]; then PASS=$((PASS + 1)); echo "  ok:   $2"; else FAIL=$((FAIL + 1)); echo "  FAIL: $2"; fi
}
summary() {
    echo "== $(basename "$0"): $PASS passed, $FAIL failed"
    [ "$FAIL" = 0 ]
}
