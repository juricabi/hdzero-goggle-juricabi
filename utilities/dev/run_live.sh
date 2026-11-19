#!/bin/bash
# The live-stream audio suite (utilities/live_audio_test) in private mount, IPC, network and pid namespaces (run as
# root): its fixed /tmp/record.pid, /tmp/live.pid, SysV keys, port 18554 and "pkill -x rtspLive" cannot reach or be
# reached by anything else on the host.
#   utilities/dev/run_live.sh all | <t_script> [args]      (env: TOOLCHAIN, default /root/hdz/toolchain; RTSPLIVE)
set -u
WT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TEST_OUT:-/root/hdz/test-out}/live
export WORK=${TEST_OUT:-/root/hdz/test-out}/law
export TOOLCHAIN=${TOOLCHAIN:-/root/hdz/toolchain}
mkdir -p "$WORK" "$OUT"
tag=${1:-all}
log=$OUT/${tag%.sh}${RTSPLIVE:+_$RTSPLIVE}.log
unshare -m -i -n -p -f --mount-proc --propagation private bash -c '
    mount -t tmpfs -o size=512m tmpfs /tmp || exit 9
    ip link set lo up || exit 9
    cd "$0"
    L=utilities/live_audio_test
    s=$(date +%s)
    if [ "$1" = all ]; then
        $L/run_all.sh
    else
        t=$1; shift
        $L/$t "$@"
    fi
    rc=$?
    echo "exit $rc after $(( $(date +%s) - s )) s"
    exit $rc
' "$WT" "$@" > "$log" 2>&1
echo "rc $? log $log"
