#!/bin/bash
# The cross-built rtspLive as shipped ($ARM_RTSPLIVE, default build_goggle2/rtspLive, linked with
# the prebuilt live555 2022.12.01 in lib/live) under qemu-arm, against the native record_host:
# the end-to-end, DESCRIBE timing and kill tests. (qemu-arm on an x86 host runs the ARM code,
# but with the host's memory ordering: it cannot show missing barriers.)
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
command -v qemu-arm > /dev/null || { echo "qemu-arm not found"; exit 2; }
[ -x "$ARM_RTSPLIVE" ] || { echo "no cross-built rtspLive at $ARM_RTSPLIVE"; exit 2; }
[ -d "$TOOLCHAIN/arm-buildroot-linux-musleabihf/sysroot" ] || { echo "TOOLCHAIN not set"; exit 2; }
rc=0
for t in "t_e2e.sh h264 h265" t_describe.sh t_kill.sh; do
    echo "######## RTSPLIVE=arm $t"
    RTSPLIVE=arm LOGS=$WORK/logs_arm "$LAT_DIR"/$t || rc=1
done
exit $rc
