#!/bin/bash
# All host tests of the live stream audio. See common.sh for WORK, LIVE555, TOOLCHAIN.
#   LIVE555=~/live TOOLCHAIN=~/toolchain utilities/live_audio_test/run_all.sh
# Not covered (needs a goggle): docs/live-audio.md, "Testing on a goggle".
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"
set -o pipefail
mkdir -p "$WORK"
"$LAT_DIR/build.sh" > "$WORK/build.log" 2>&1 || { tail -20 "$WORK/build.log"; exit 1; }
"$LAT_DIR/gen_media.sh" > /dev/null || exit 1
rc=0
run() { echo "######## $*"; "$@" 2>&1 | tee "$WORK/$(basename "$1" .sh).out" | grep -E "^==|FAIL|ok:|checks|wake-ups" ; [ "${PIPESTATUS[0]}" = 0 ] || rc=1; }
run "$BIN/ring_test"
run "$LAT_DIR/t_aidev.sh"
if [ -x "$BIN/ring_test.arm" ] && command -v qemu-arm > /dev/null; then run qemu-arm "$BIN/ring_test.arm"; fi
if [ -x "$BIN/rtspLive" ]; then
    for t in t_describe.sh t_kill.sh t_share.sh t_e2e.sh t_avsync.sh t_audio_fail.sh t_cpu.sh t_off_identical.sh t_snapshot.sh; do
        run "$LAT_DIR/$t"
    done
fi
[ -x "$ARM_RTSPLIVE" ] && command -v qemu-arm > /dev/null && run "$LAT_DIR/t_arm.sh"
echo "######## $([ $rc = 0 ] && echo "all passed" || echo "FAILURES") (logs in $WORK/logs, $WORK/logs_arm, $WORK/*.out)"
exit $rc
