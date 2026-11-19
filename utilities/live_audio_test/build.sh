#!/bin/bash
# Build the host test programs for the live stream audio (nothing here is part of the firmware
# build; all output goes to $WORK, default ${TMPDIR:-/tmp}/live_audio_test).
#
#   record_host   the real src/record/main.c, avshare.c, live_audio.c, confparser.c, gogglemsg.c
#                 with record_stubs.c in place of MPP, ffmpeg and the SD card
#   rtspLive      the real src/rtspLive, built natively against a host live555
#                 (LIVE555=<built live555 source tree>; skipped when not set)
#   ring_test     both ring implementations in one binary (x86; ring_test.arm with TOOLCHAIN=<buildroot sdk>)
#   record_host_fast  the same, stream audio stopped 300 ms (not 3 s) after the reader leaves
#   stream_reader the real ring reader holding /tmp/live.pid: rtspLive's audio track without a server
#   gmsg          sends goggle messages (record start/stop, AI->AO loop, HDMI audio bridge, quit)
#   ctl_conf      ripples-all: publishes a config snapshot and sends RECORD_CONFIG like the app's DVR control
#
# Every file is compiled with -Wall -Wextra; warnings are kept in $WORK/build/*.warn.
set -e
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

B=$WORK/build
mkdir -p "$B" "$BIN"
cd "$REPO"

W="-Wall -Wextra -Wno-unused-parameter"
REC_INC="-Isrc/record -Ilib/log/include -Ilib/minIni/src -Ilib/ffmpeg/include -Ilib/linux/include \
 -Ilib/linux/include/video -Ilib/linux/include/media -Ilib/softwinner/include/system/public/include \
 -Ilib/softwinner/include/system/public/include/vo -Ilib/softwinner/include/system/public/include/utils \
 -Ilib/softwinner/include/middleware/sample/configfileparser \
 -Ilib/softwinner/include/middleware/media/LIBRARY/libcedarx/libcore/common/iniparser \
 -Ilib/softwinner/include/middleware/include -Ilib/softwinner/include/middleware/include/utils \
 -Ilib/softwinner/include/middleware/include/media -Ilib/softwinner/include/middleware/media/include/utils \
 -Ilib/softwinner/include/middleware/media/include/component \
 -Ilib/softwinner/include/middleware/media/LIBRARY/libcedarc/include \
 -Ilib/softwinner/include/middleware/media/LIBRARY/libisp/include \
 -Ilib/softwinner/include/middleware/media/LIBRARY/libisp/isp_tuning"

cc_warn() { # out src flags...
    local out=$1 src=$2; shift 2
    gcc "$@" -c "$src" -o "$out" 2> "$out.warn" || { cat "$out.warn"; exit 1; }
}

# ---- record_host ----------------------------------------------------------------------------
R=$B/record
mkdir -p "$R"
RCF="-O1 -g $W -DAWCHIP=AW_V5 -D_GNU_SOURCE $REC_INC"
for f in main avshare live_audio confparser gogglemsg rolling; do
    cc_warn "$R/$f.o" "src/record/$f.c" $RCF
done
# ripples' recorder: the DVR control queue and the DVR trace (lib/log), the real ones
for f in record_control dvr_trace; do
    cc_warn "$R/$f.o" "lib/log/src/$f.c" $RCF
done
gcc -O1 -g -Ilib/minIni/src -c lib/minIni/src/minIni.c -o "$R/minIni.o"
cc_warn "$R/stubs.o" "$LAT_DIR/record_stubs.c" $RCF -I"$LAT_DIR"
gcc -o "$BIN/record_host" "$R"/*.o -lpthread
echo "built $BIN/record_host"
# the same with the stream audio stopping 300 ms after the reader leaves (not 3 s): t_aidev.sh
mkdir -p "$R/fast"
cc_warn "$R/fast/live_audio.o" src/record/live_audio.c $RCF -DLIVE_AUDIO_STOP_DELAY_MS=300
gcc -o "$BIN/record_host_fast" $(ls "$R"/*.o | grep -v "/live_audio.o$") "$R/fast/live_audio.o" -lpthread
echo "built $BIN/record_host_fast"
# stands in for rtspLive's audio track (the real ring reader, no server): t_aidev.sh
SR=$B/stream_reader
mkdir -p "$SR"
cc_warn "$SR/stream_reader.o" "$LAT_DIR/stream_reader.c" -O1 -g $W -D_GNU_SOURCE -Isrc/rtspLive/stream
cc_warn "$SR/avshare.o" src/rtspLive/stream/avshare.c -O1 -g $W -Wno-unused-variable -Wno-unused-function -D_GNU_SOURCE -Isrc/rtspLive/stream
gcc -o "$BIN/stream_reader" "$SR"/*.o -lpthread
echo "built $BIN/stream_reader"

# ---- gmsg (and an ARM one: SysV message payloads are not translated by qemu-arm, a 64-bit
# sender cannot talk to the 32-bit rtspLive under qemu) ---------------------------------------
gcc -O1 $W -Isrc/record -o "$BIN/gmsg" "$LAT_DIR/gmsg.c"
if [ -n "$TOOLCHAIN" ] && [ -x "$TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-gcc" ]; then
    "$TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-gcc" -static -O1 $W -Isrc/record -o "$BIN/gmsg.arm" "$LAT_DIR/gmsg.c"
fi

# ---- ctl_conf (ripples' record control queue, lib/log) ----------------------------------------
gcc -O1 $W -Ilib/log/include -o "$BIN/ctl_conf" "$LAT_DIR/ctl_conf.c" lib/log/src/record_control.c
echo "built $BIN/ctl_conf"

# ---- rtspLive (native) ----------------------------------------------------------------------
if [ -n "$LIVE555" ]; then
    O=$B/rtspLive
    mkdir -p "$O"
    rm -f "$O"/*.o "$O"/*.warn
    LINC="-I$LIVE555/liveMedia/include -I$LIVE555/groupsock/include -I$LIVE555/UsageEnvironment/include -I$LIVE555/BasicUsageEnvironment/include"
    INC="-Isrc/rtspLive -Isrc/rtspLive/stream -Isrc/rtspLive/server -Ilib/minIni/src $LINC"
    for f in src/rtspLive/stream/*.c; do
        cc_warn "$O/$(basename "$f").o" "$f" -O1 -g $W -DNO_OPENSSL=1 -D_GNU_SOURCE $INC
    done
    gcc -O1 -g -Ilib/minIni/src -c lib/minIni/src/minIni.c -o "$O/minIni.o"
    for f in src/rtspLive/main.cpp src/rtspLive/server/*.cpp; do
        g++ -O1 -g $W -DNO_OPENSSL=1 -D_GNU_SOURCE $INC -c "$f" -o "$O/$(basename "$f").o" 2> "$O/$(basename "$f").o.warn" \
            || { cat "$O/$(basename "$f").o.warn"; exit 1; }
    done
    g++ -o "$BIN/rtspLive" "$O"/*.o "$LIVE555/liveMedia/libliveMedia.a" "$LIVE555/groupsock/libgroupsock.a" \
        "$LIVE555/BasicUsageEnvironment/libBasicUsageEnvironment.a" "$LIVE555/UsageEnvironment/libUsageEnvironment.a" -lpthread
    echo "built $BIN/rtspLive (host live555 $(grep -o '"[0-9.]*"' "$LIVE555/liveMedia/include/liveMedia_version.hh" | head -1))"
else
    echo "LIVE555 not set: native rtspLive not built (the ARM build can still be run with RTSPLIVE=arm)"
fi

# ---- ring test ------------------------------------------------------------------------------
build_ring() { # suffix cc objcopy nm
    local sfx=$1 cc=$2 objcopy=$3 nm=$4 out=$B/ring$1
    local CF="-O2 -g $W -Wno-unused-variable -Wno-unused-function -D_GNU_SOURCE"
    mkdir -p "$out"
    $cc $CF -c src/record/avshare.c -o "$out/w_avshare.o" 2> "$out/w_avshare.o.warn" || { cat "$out/w_avshare.o.warn"; exit 1; }
    $cc $CF -Isrc/rtspLive/stream -c src/rtspLive/stream/avshare.c -o "$out/r_avshare.o" 2> "$out/r_avshare.o.warn" \
        || { cat "$out/r_avshare.o.warn"; exit 1; }
    for p in w r; do
        local args=""
        for s in $($nm --defined-only -g "$out/${p}_avshare.o" | awk '{print $3}'); do
            args="$args --redefine-sym $s=${p}_$s"
        done
        $objcopy $args "$out/${p}_avshare.o"
    done
    $cc $CF -pthread "$LAT_DIR/ring_test.c" "$out/w_avshare.o" "$out/r_avshare.o" -o "$BIN/ring_test$sfx"
    echo "built $BIN/ring_test$sfx"
}
build_ring "" gcc objcopy nm
if [ -n "$TOOLCHAIN" ] && [ -x "$TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-gcc" ]; then
    X=$TOOLCHAIN/bin/arm-buildroot-linux-musleabihf-
    build_ring .arm "${X}gcc -static" "${X}objcopy" "${X}nm"
fi

echo "--- -Wall -Wextra warnings per file:"
for w in $(find "$B" -name "*.warn" | sort); do
    n=$(grep -c "warning:" "$w" || true)
    [ "$n" != 0 ] && echo "  $(basename "$w" .o.warn): $n"
done
echo "(done)"
