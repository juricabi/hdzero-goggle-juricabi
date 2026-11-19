#!/bin/bash
# ripples' own tests (test/*) on a copy of this worktree, as an otherwise unused uid in its own pid and mount
# namespace with a private /tmp: the tests compile production code (dvr.c, hardware-*.c, ...) with stubs, and
# whatever a stub misses can reach neither the host's devices nor its files.  Run as root.
#   utilities/dev/run_ripples_tests.sh            (logs: $TEST_OUT/ripples_tests, default /root/hdz/test-out)
# One line per test: "<name>: rc <exit status>".  ripples 1.2.2's tests (the base since juricabi.2) and
# juricabi's test/dvr.  test/record's "run" also builds test_dvr_naming, which does not compile on glibc (its
# statfs() macro meets <sys/statfs.h>): test_avshare_reset, which builds on its own, is run separately as well.
set -u
WT=$(cd "$(dirname "$0")/../.." && pwd)
LOGD=${TEST_OUT:-/root/hdz/test-out}/ripples_tests
mkdir -p "$LOGD"
U=64999
C=$(mktemp -d /var/tmp/rtests.XXXXXX)
rsync -a --exclude .git --exclude 'build_*' --exclude toolchain "$WT/" "$C/tree/"
cat > "$C/run.sh" << 'EOF'
#!/bin/bash
cd "$1/tree" || exit 1
L=$2
t() { local n=$1; shift; "$@" > "$L/$n.log" 2>&1; local rc=$?; echo "$n: rc $rc"; }
t battery make -C test/battery run
t framebuffer make -C test/framebuffer run
for T in HDZGOGGLE HDZGOGGLE2 HDZBOXPRO; do t hdmi_$T make -C test/hdmi run TARGET=$T; done
t ht make -C test/ht run
for T in HDZGOGGLE HDZGOGGLE2 HDZBOXPRO; do t motion_$T make -C test/motion run TARGET=$T; done
t record make -C test/record run
t record_avshare_only bash -c 'cd test/record && make test_avshare_reset && ./test_avshare_reset'
t dvr_auto make -C test/dvr run
t screen make -C test/screen run
t sleep make -C test/sleep run
t video make -C test/video run
t safety_bounds python3 -I test/safety/test_bounds.py
t safety_shell_arg python3 -I test/safety/test_shell_arg.py
EOF
chmod 755 "$C/run.sh"
LT=$C/logs
mkdir -p "$LT"
chown -R $U:$U "$C"
chmod 755 "$C"
unshare -m -p -f --mount-proc --propagation private bash -c '
    mount -t tmpfs -o size=256m tmpfs /tmp || exit 9
    exec setpriv --reuid='$U' --regid='$U' --clear-groups env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$0" TMPDIR=/tmp LANG=C \
        bash "$0/run.sh" "$0" "$1"' "$C" "$LT" > "$LOGD/summary.txt" 2>&1
cp "$LT"/*.log "$LOGD/" 2> /dev/null
rm -rf "$C"
cat "$LOGD/summary.txt"
