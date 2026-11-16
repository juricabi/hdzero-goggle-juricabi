#!/bin/sh
# PC-side tests for the probe kit (host only). Every script runs under dash and busybox ash
# against a fake goggle: a /proc, /sys and /mnt tree, a fake goggle app (a real process
# whose state is checked: running vs stopped), and stub tools with a model of the shared
# sunxi_dump address latch, the two ITE chips and the GPIO driver (test/stub/model.sh).
# Checked: the app is never left stopped and never started twice; every register the
# scripts change is back at the end, also when they are killed at many points (TERM, INT,
# HUP; SIGKILL: the guardian resumes / starts the app); no latch or ITE access while the
# app runs; reads that do not repeat or are implausible lead to no write; PH_CFG0 is
# written only from the stock value and only as one of two constants; IT66121 Reg04 only
# when it holds no video reset; FIFO registers and IT66121 reg 0x17 are never read;
# /dev/mem is only read (devmem without a value); only commands of the goggle's busybox
# are used.
# Usage: sh utilities/probe/test/run_tests.sh   (QUICK=1: fewer kill points; ONLY="test probe bay";
#   SHELLS="dash busybox_sh goggle_ash")
#   ROOTFS=<extracted goggle rootfs>: also check every command against that rootfs, take the
#   applet list from it, and (with qemu-arm) run everything under the goggle's own ash too.
#   KIT_DIR=<dir>: test another copy of the scripts (test/mutants.sh uses it).
# Not as root: see below. As root use utilities/dev/run_probe.sh run (an unused uid in its
# own pid namespace).
#
# busybox ash runs its own applets ahead of PATH (Ubuntu's busybox is built that way), so in
# the busybox passes a stub with an applet's name (pidof, cat, i2cset, mount, ...) would be
# bypassed and the host's real command would run on the host's processes and devices. There
# every stub and forbidden command that is also an applet of that busybox becomes a shell
# function calling it by its path (functions come before applets), and the script is sourced
# by that shell with the same $0 and arguments as when it is run. Before any script runs
# under a shell, every stub and forbidden command must resolve to ours in it (`type`), else
# that shell is not used at all (FAIL).
set -u
if [ "$(id -u)" = 0 ]; then
    echo "run_tests.sh: refusing to run as root: if a stub were bypassed (busybox ash prefers its own" >&2
    echo "applets), the host's real i2cset, mount and kill -STOP would run with root rights." >&2
    echo "Run it as an unprivileged user, or as root through utilities/dev/run_probe.sh run" >&2
    echo "(an unused uid in its own pid namespace)." >&2
    exit 2
fi
HERE=$(cd "$(dirname "$0")" && pwd)
KIT=${KIT_DIR:-$(cd "$HERE/.." && pwd)}
STUB="$HERE/stub"
SCRIPTS="hdmi_audio_probe.sh bay_gpio_pattern.sh hdmi_audio_test.sh"
fail=0
ok() { echo "PASS $*"; }
bad() {
    echo "FAIL $*"
    fail=1
}

# busybox applets of the official images (Goggle 1 Rev20250319 = Goggle 2 Rev20250715: the
# same 89; ROOTFS= reads them from the image instead)
APPLETS="[ [[ addgroup adduser ash awk basename bash cat chmod chown chpasswd chroot cp crond crontab cryptpw cut
dd delgroup deluser df dirname dmesg du echo env expr fgrep flock fsync getty grep hexdump hwclock ifconfig
ifdown ifup init insmod kill killall less ln lock login losetup ls lsmod lsof md5sum mkdir mkpasswd mount mv
passwd pidof ping pivot_root poweroff ps pwd readlink reboot rm rmmod route sed seq setsid sh sleep
start-stop-daemon sync sysctl tail time top touch tr udhcpc udhcpd umount uname usleep vi watchdog wc xargs"
# commands the scripts may use that are not busybox applets on the goggle
OTHER="i2cget i2cset i2cdump i2cdetect amixer aplay arecord"
# commands NOT on the goggle (no applet): a script that runs one fails the tests
DENY="head sort printf timeout date find pgrep pkill stat tee od xxd bc nproc mktemp realpath"

# ---------------------------------------------------------------- static checks
if command -v shellcheck > /dev/null 2>&1; then
    for s in $SCRIPTS; do
        if shellcheck -s sh "$KIT/$s"; then ok "shellcheck $s"; else bad "shellcheck $s"; fi
    done
else
    echo "SKIP shellcheck (not installed)"
fi
block() { sed -n '/^# ===== probe common block/,/^# ===== end of probe common block/p' "$1"; }
b0=$(block "$KIT/hdmi_audio_test.sh")
for s in $SCRIPTS; do
    if [ -n "$b0" ] && [ "$(block "$KIT/$s")" = "$b0" ]; then ok "$s: common safety block identical"; else bad "$s: common safety block differs"; fi
done
for s in $SCRIPTS; do # builtins the goggle's ash does not have (dash has printf built in)
    if grep -v '^ *#' "$KIT/$s" | grep -v awk | grep -n -E '(^|[;&|]|then|do|else)[[:space:]]*(printf|local|source)[[:space:]]'; then
        bad "$s uses printf/local/source"
    else
        ok "$s: no printf/local/source"
    fi
done
if [ -n "${ROOTFS:-}" ]; then
    APPLETS=$(for d in bin sbin usr/bin usr/sbin; do
        for f in "$ROOTFS/$d"/*; do [ -L "$f" ] && case "$(readlink "$f")" in *busybox) echo "${f##*/}" ;; esac; done
    done)
    for c in $OTHER; do
        f=""
        for d in bin sbin usr/bin usr/sbin; do [ -e "$ROOTFS/$d/$c" ] && f="$d/$c"; done
        if [ -n "$f" ]; then ok "rootfs has $c ($f)"; else bad "rootfs lacks $c"; fi
    done
    for c in awk basename cat cp dd dirname dmesg flock grep hexdump kill ls lsmod mkdir mount mv pidof ps rm sleep sync tr umount uname usleep wc busybox; do
        case " $(echo $APPLETS) busybox " in *" $c "*) ok "rootfs busybox has $c" ;; *) bad "rootfs busybox lacks $c" ;; esac
    done
fi

# ---------------------------------------------------------------- fake goggle
T0=$(mktemp -d)
trap 'pkill -f "$T0/" 2> /dev/null; rm -rf "$T0"' EXIT
GBIN="$T0/gbin"
DENYBIN="$T0/deny"
mkdir -p "$GBIN" "$DENYBIN"
for a in $APPLETS; do
    case "$a" in "[" | "[[" | cat | ps | pidof | lsmod | dmesg | mount | umount) continue ;; esac
    if [ "$a" = flock ]; then ln -sf "$(command -v flock)" "$GBIN/flock"; else ln -sf "$(command -v busybox)" "$GBIN/$a"; fi
done
for c in $DENY; do
    printf '#!/bin/sh\necho "forbidden command (not on the goggle): %s $*" >> "${MODEL:-/dev/null}/violations"\nexit 127\n' "$c" > "$DENYBIN/$c"
    chmod +x "$DENYBIN/$c"
done
TPATH="$STUB:$DENYBIN:$GBIN:/usr/bin:/bin"
HOSTENV=$(command -v env) # GNU env (for --default-signal); busybox env is in TPATH

setup() { # $1 = test dir: fresh root (R) + model (M)
    T=$1
    R=$T/root
    M=$T/model
    rm -rf "$T"
    mkdir -p "$M" "$R/proc/asound/card0/pcm0p/sub0" "$R/proc/asound/card1/pcm0p/sub0" "$R/proc/asound/card2/pcm0c/sub0" \
        "$R/proc/device-tree/soc@03000000/daudio@0x05090000" "$R/proc/device-tree/soc@03000000/sound@1" \
        "$R/proc/device-tree/soc@03000000/pinctrl@0300b000/daudio0@0" "$R/proc/device-tree/snddaudo0" \
        "$R/sys/class/sunxi_dump" "$R/sys/kernel/debug/pinctrl/300b000.pinctrl" "$R/sys/kernel/debug/asoc" \
        "$R/sys/bus/mmc/devices" "$R/mnt/extsd" "$R/mnt/app/app" "$R/tmp" "$R/lib/modules" "$R/dev/snd"
    for g in 131 192 193 194; do
        mkdir -p "$R/sys/class/gpio/gpio$g"
        : > "$R/sys/class/gpio/gpio$g/value"
        : > "$R/sys/class/gpio/gpio$g/direction"
    done
    : > "$R/sys/class/gpio/export"
    : > "$R/sys/class/gpio/unexport"
    echo "Linux version 4.9.118 (stub)" > "$R/proc/version"
    echo "console=ttyS0" > "$R/proc/cmdline"
    echo "12.34 5.67" > "$R/proc/uptime"
    echo "hdzero 16384 0 - Live 0x00000000" > "$R/proc/modules"
    echo "debugfs /sys/kernel/debug debugfs rw 0 0" > "$R/proc/mounts"
    printf ' 0 [audiocodec     ]: audiocodec - audiocodec\n 1 [snddaudio0     ]: snddaudio0 - snddaudio0\n 2 [snddaudio2     ]: snddaudio2 - snddaudio2\n' > "$R/proc/asound/cards"
    echo "Advanced Linux Sound Architecture Driver Version k4.9.118." > "$R/proc/asound/version"
    for c in 0 1 2; do echo "card$c" > "$R/proc/asound/card$c/id"; done
    echo closed > "$R/proc/asound/card0/pcm0p/sub0/status"
    printf 'allwinner,sunxi-daudio\000' > "$R/proc/device-tree/soc@03000000/daudio@0x05090000/compatible"
    printf 'PH1\000PH2\000PH3\000' > "$R/proc/device-tree/soc@03000000/pinctrl@0300b000/daudio0@0/allwinner,pins"
    : > "$R/sys/class/sunxi_dump/dump"
    : > "$R/sys/class/sunxi_dump/write"
    pm="$R/sys/kernel/debug/pinctrl/300b000.pinctrl/pinmux-pins"
    # the stock device tree: sdc1 (sdmmc, SDIO Wi-Fi) owns PG0-PG5 on every goggle
    for p in 192 193 194; do echo "pin $p (PG$((p - 192))): 4021000.sdmmc (GPIO UNCLAIMED) function sdc1 group PG$((p - 192))" >> "$pm"; done
    echo "pin 225 (PH1): 5001000.uart (GPIO UNCLAIMED) function uart4 group PH1" >> "$pm"
    printf 'gpiochip0: GPIOs 0-287, parent: platform/300b000.pinctrl, 300b000.pinctrl:\n gpio-131 (                    |sysfs               ) out lo\n' > "$R/sys/kernel/debug/gpio"
    echo "snd-soc-dummy" > "$R/sys/kernel/debug/asoc/codecs"
    echo "goggle 2 stub" > "$R/mnt/app/version"
    cp "$STUB/HDZGOGGLE" "$R/mnt/app/app/HDZGOGGLE"
    # the services package's busybox (it has devmem): a wrapper of test/stub/services_busybox
    mkdir -p "$R/mnt/app/services/busybox"
    printf '#!/bin/sh\nexec "%s/services_busybox" "$@"\n' "$STUB" > "$R/mnt/app/services/busybox/busybox"
    chmod +x "$R/mnt/app/services/busybox/busybox"
    : > "$M/services_busybox"
    # SoC registers after rc.sh (sunxi_dump model)
    cat > "$M/soc" << 'EOF'
0x0300b098 0x00775577
0x0300b0d8 0x22777777
0x0300b0dc 0x00377777
0x0300b0e8 0x00000100
0x0300b0f4 0x00000000
0x0300b0fc 0x35517751
0x0300b10c 0x00000011
0x05090000 0x00060000
0x05090028 0x00001234
0x05092000 0x00060000
0x0300b0f8 0x00000000
EOF
    cp "$M/soc" "$M/soc.orig"
    printf 'g_0x00 0x54\ng_0x01 0x49\ng_0x02 0x12\ng_0x03 0x06\ng_0x0f 0x08\ng_0x04 0x14\nb0_0xf8 0xff\nb0_0xe0 0xc0\nb0_0xc5 0x04\nb0_0x5f 0xe0\nb0_0x58 0x11\nb0_0xce 0x00\n' > "$M/i2c_1_0x4c"
    printf 'g_0x0f 0x00\nb0_0x99 0x08\nb0_0x52 0x30\n' > "$M/i2c_3_0x49"
    cp "$M/i2c_1_0x4c" "$M/i2c_1_0x4c.orig"
    cp "$M/i2c_3_0x49" "$M/i2c_3_0x49.orig"
    : > "$M/violations"
    : > "$M/soc_writes"
    : > "$M/soc_reads"
    : > "$M/i2c_reads"
    : > "$M/i2c_writes"
}
# PRE: the shell functions of the shell in use (busybox passes, see the top), "" for dash
PRE=""
shims() { # $1 = applet list of the shell's busybox: a function for each stub / forbidden command among them
    for c in $(cd "$STUB" && ls); do
        case "$c" in model.sh | services_busybox | HDZGOGGLE) continue ;; esac
        case " $(echo $1) " in *" $c "*) echo "$c() { \"$STUB/$c\" \"\$@\"; }" ;; esac
    done
    for c in $DENY; do
        case " $(echo $1) " in *" $c "*) echo "$c() { \"$DENYBIN/$c\" \"\$@\"; }" ;; esac
    done
}
stubs_reached() { # $1 = shell command: every stub and forbidden command resolves to ours in it
    sr=0
    for c in $(cd "$STUB" && ls) $DENY; do
        case "$c" in model.sh | services_busybox | HDZGOGGLE) continue ;; esac
        # shellcheck disable=SC2086 # $1 holds a command with options
        w=$(PATH=$TPATH $1 -c "$PRE"'
type "$0"' "$c" 2>&1 < /dev/null)
        case "$w" in
            *function* | *"$STUB/$c"* | *"$DENYBIN/$c"*) ;;
            *builtin*) case " $DENY " in *" $c "*) ;; *) echo "  $c: $w" && sr=1 ;; esac ;; # (printf)
            *) echo "  $c: $w" && sr=1 ;;
        esac
    done
    return $sr
}
env_run() { # $1 shell, $2 mode, $3 script, rest args: run in the foreground
    sh_=$1
    md=$2
    sc=$3
    shift 3
    if [ -n "$PRE" ]; then set -- -c "$PRE"'
. "$0"' "$KIT/$sc" "$@"; else set -- "$KIT/$sc" "$@"; fi
    # shellcheck disable=SC2086 # sh_ holds a command with options
    PATH=$TPATH MODEL=$M PROBE_ROOT=$R PROBE_MODE=$md PROBE_FAST=1 PROBE_WAIT=0 TMPDIR=$R/tmp \
        $sh_ "$@" < /dev/null > /dev/null 2>&1
}
env_bg() { # same, in the background with SIGINT not ignored; BGPID
    sh_=$1
    md=$2
    sc=$3
    shift 3
    if [ -n "$PRE" ]; then set -- -c "$PRE"'
. "$0"' "$KIT/$sc" "$@"; else set -- "$KIT/$sc" "$@"; fi
    # shellcheck disable=SC2086
    PATH=$TPATH MODEL=$M PROBE_ROOT=$R PROBE_MODE=$md PROBE_FAST=1 PROBE_WAIT=0 TMPDIR=$R/tmp \
        "$HOSTENV" --default-signal=INT $sh_ "$@" < /dev/null > /dev/null 2>&1 &
    BGPID=$!
}
start_fake_app() {
    MODEL=$M PROBE_ROOT=$R "$R/mnt/app/app/HDZGOGGLE" &
    i=0
    while [ "$i" -lt 50 ] && [ -z "$(app_pids)" ]; do
        sleep 0.05
        i=$((i + 1))
    done
}
app_pids() {
    [ -f "$M/app.pids" ] || return 0
    while read -r p; do
        s=$(awk '{ print $3 }' "/proc/$p/stat" 2> /dev/null)
        case "$s" in "" | Z) ;; *) echo "$p" ;; esac
    done < "$M/app.pids"
}
apps_ok() { # $1 = expected number of app processes: that many, none stopped
    n=0
    for p in $(app_pids); do
        n=$((n + 1))
        [ "$(awk '{ print $3 }' "/proc/$p/stat" 2> /dev/null)" = T ] && return 1
    done
    [ "$n" -eq "$1" ]
}
stop_apps() {
    for p in $(app_pids); do kill -CONT "$p" 2> /dev/null; kill "$p" 2> /dev/null; done
}
flush() { PATH=$TPATH MODEL=$M PROBE_ROOT=$R MODEL_FLUSH=1 "$STUB/cat" "$R/sys/class/sunxi_dump/dump"; }
soc() { awk -v k="$1" '$1 == k { print $2 }' "$M/soc"; }
# a store as it would read: registers never written read 0 in the model
same_sorted() { [ "$(grep -v -E ' 0x0+$' "$1" | sort)" = "$(grep -v -E ' 0x0+$' "$2" | sort)" ]; }
all_back() {
    flush
    same_sorted "$M/soc" "$M/soc.orig" && same_sorted "$M/i2c_1_0x4c" "$M/i2c_1_0x4c.orig" &&
        same_sorted "$M/i2c_3_0x49" "$M/i2c_3_0x49.orig"
}
no_viol() { [ ! -s "$M/violations" ]; }
gone() { # nothing of this scenario is left but the app (script, guardian, players) within 3 s
    i=0
    while [ "$i" -lt 30 ]; do
        left=""
        for e in $(grep -l -z -x "MODEL=$M" /proc/[0-9]*/environ 2> /dev/null); do
            p=${e#/proc/}
            p=${p%/environ}
            grep -q -x "$p" "$M/app.pids" 2> /dev/null || left="$left $p"
        done
        [ -z "$left" ] && return 0
        sleep 0.1
        i=$((i + 1))
    done
    echo "  still running:$left ($(for p in $left; do tr '\000' ' ' < "/proc/$p/cmdline"; echo; done 2> /dev/null | head -3))"
    return 1
}
WHY=""
need() { # $1 label, then a command: a failing command adds the label to WHY
    nl=$1
    shift
    "$@" || WHY="$WHY [$nl]"
}
not_left_stopped() { # the scripts' final app_unstop_all logs this only if the app was left stopped
    [ ! -f "$1" ] || ! grep -q "was stopped: resumed" "$1"
}
verdict() { # $1 test name, $2 report file
    need app-not-left-stopped not_left_stopped "$2"
    if [ -z "$WHY" ]; then ok "$1"; else
        bad "$1:$WHY"
        show "$2"
    fi
    WHY=""
}
store_diff() { # what differs from the start (registers never written read 0)
    for st in soc i2c_1_0x4c i2c_3_0x49; do
        grep -v -E ' 0x0+$' "$M/$st" | sort > "$M/$st.now"
        grep -v -E ' 0x0+$' "$M/$st.orig" | sort > "$M/$st.was"
        diff "$M/$st.was" "$M/$st.now" | sed -n "s/^[<>]/$st &/p"
    done
}
show() {
    echo "--- registers changed (< before, > now):"
    store_diff
    echo "--- violations:"
    cat "$M/violations"
    echo "--- soc writes:"
    cat "$M/soc_writes"
    echo "--- report:"
    tail -25 "$1" 2> /dev/null
}

# shells: dash, the host's busybox ash, and with ROOTFS + qemu-arm the goggle's own busybox
# 1.27 ash (its builtins differ: no "command", no printf)
GOGGLE_ASH=""
if [ -n "${ROOTFS:-}" ] && [ -x "$ROOTFS/bin/busybox" ] && command -v qemu-arm > /dev/null 2>&1; then
    GOGGLE_ASH="qemu-arm -L $ROOTFS $ROOTFS/bin/busybox sh"
fi
SHELLS=${SHELLS:-"dash busybox_sh${GOGGLE_ASH:+ goggle_ash}"}
GROUPS_=${ONLY:-"test probe bay"}
stopped_now() { # 0 if the fake app is stopped right now
    for p in $(app_pids); do [ "$(awk '{ print $3 }' "/proc/$p/stat" 2> /dev/null)" = T ] && return 0; done
    return 1
}
dirs_released() { # never exported (killed during the checks), or all back to input
    dr=$(cat "$R/sys/class/gpio/gpio192/direction" "$R/sys/class/gpio/gpio193/direction" "$R/sys/class/gpio/gpio194/direction" | tr '\n' ' ')
    case "$dr" in "in in in " | "") return 0 ;; esac
    return 1
}
nwrites() { [ "$(grep -c . "$M/soc_writes")" -eq "$1" ]; }
for shn in $SHELLS; do
    sh=$(echo "$shn" | tr '_' ' ')
    lbl=$shn
    [ "$shn" = goggle_ash ] && sh=$GOGGLE_ASH && lbl="goggle-ash(qemu)"
    command -v "${sh%% *}" > /dev/null 2>&1 || {
        echo "SKIP $sh"
        continue
    }
    T="$T0/$shn"
    # busybox ash prefers its own applets: our stubs as functions there (see the top)
    case "$shn" in
        busybox_sh) PRE=$(shims "$(busybox --list 2> /dev/null)") ;;
        goggle_ash) PRE=$(shims "$APPLETS") ;;
        *) PRE="" ;;
    esac
    if stubs_reached "$sh"; then
        ok "$lbl: every stub and forbidden command is reached (none bypassed by a builtin or an applet)"
    else
        bad "$lbl: a stub or forbidden command would be bypassed (above): no script run under this shell"
        continue
    fi

    case " $GROUPS_ " in *" test "*)
        # 1. SSH mode, app running, both parts
        setup "$T"
        start_fake_app
        env_run "$sh" ssh hdmi_audio_test.sh
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need ph-set grep -q "PH_CFG0 0x35517751 -> 0x35513331" "$o"
        need ph-restored grep -q "PH_CFG0 restored: 0x35517751" "$o"
        need ctl-set grep -q "I2S0 CTL 0x00060000 -> 0x00060010" "$o"
        need ctl-restored grep -q "I2S0 CTL restored: 0x00060000" "$o"
        need it-restored grep -q "IT66121 audio registers restored" "$o"
        need part-b grep -q "recording 1s from hw:snddaudio2" "$o"
        need it-programmed grep -q "W 1 0x4c b1_0x6d 0x70" "$M/i2c_writes"
        need 4-soc-writes nwrites 4
        need all-back all_back
        need no-violation no_viol
        need one-app-running apps_ok 1
        need tone-removed test ! -e "$R/tmp/hdmi_tone.raw"
        need helpers-gone gone hdmi_audio_test.sh
        verdict "$lbl: hdmi_audio_test ssh: both parts, everything restored, app paused only around access" "$o"
        stop_apps

        # 2. the latch returns another register for one of the three reads: no write
        setup "$T"
        start_fake_app
        STUB_RACE_ADDR=0x0300b0fc STUB_RACE_N=2 env_run "$sh" ssh hdmi_audio_test.sh out
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need detected grep -q "PH_CFG0 read unstable" "$o"
        need no-ph-write sh -c "! grep -q 0x0300b0fc '$M/soc_writes'"
        need all-back all_back
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test: an interleaved latch access is detected, PH_CFG0 not written" "$o"
        stop_apps
        # 3. race during the verify read after the write: the restore still puts PH1-PH3 back
        setup "$T"
        start_fake_app
        STUB_RACE_ADDR=0x0300b0fc STUB_RACE_N=5 env_run "$sh" ssh hdmi_audio_test.sh out
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need ph-restored grep -q "PH_CFG0 restored: 0x35517751" "$o"
        need all-back all_back
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test: race on the verify read, PH_CFG0 still restored" "$o"
        stop_apps
        # 4. a latch that always answers with something else (the old stub): nothing written
        setup "$T"
        start_fake_app
        STUB_LATCH_ECHO=1 env_run "$sh" ssh hdmi_audio_test.sh out
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need no-soc-write nwrites 0
        need ph-left grep -q "pin mux left alone" "$o"
        need ctl-left grep -q "I2S0 CTL read not trusted" "$o"
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test: implausible reads (wrong register) lead to no SoC write" "$o"
        stop_apps
        # 5. an aww of the app keeps running: no window, nothing written, app resumed
        setup "$T"
        start_fake_app
        : > "$M/aww"
        env_run "$sh" ssh hdmi_audio_test.sh out
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need no-soc-write nwrites 0
        need no-i2c-write test ! -s "$M/i2c_writes"
        need logged grep -q "did not settle" "$o"
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test: aww running, nothing touched, app resumed" "$o"
        stop_apps
        # 5a. PH1-PH3 are I2S0 already (HDMI Out Audio in effect this boot): used, never written or undone
        setup "$T"
        sed -i 's/^0x0300b0fc .*/0x0300b0fc 0x35513331/' "$M/soc" && cp "$M/soc" "$M/soc.orig"
        start_fake_app
        env_run "$sh" ssh hdmi_audio_test.sh out
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need found grep -q "PH1-PH3 are I2S0 already" "$o"
        need no-ph-write sh -c "! grep -q 0x0300b0fc '$M/soc_writes'"
        need ctl-restored grep -q "I2S0 CTL restored: 0x00060000" "$o"
        need it-restored grep -q "IT66121 audio registers restored" "$o"
        need 2-soc-writes nwrites 2
        need all-back all_back
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test: PH1-PH3 already I2S0 (in effect this boot): not written, left as found" "$o"
        stop_apps
        # 5b. PH_CFG0 neither the stock value nor the I2S0 value: refused, never written
        setup "$T"
        sed -i 's/^0x0300b0fc .*/0x0300b0fc 0x35513351/' "$M/soc" && cp "$M/soc" "$M/soc.orig"
        start_fake_app
        env_run "$sh" ssh hdmi_audio_test.sh out
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need refused grep -q "0x35513351 is neither" "$o"
        need no-ph-write sh -c "! grep -q 0x0300b0fc '$M/soc_writes'"
        need all-back all_back
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test: PH_CFG0 neither stock nor I2S0: refused, not written" "$o"
        stop_apps
        # 5c. IT66121 Reg04 with the video reset bit set: part A skipped, no IT66121 register written
        setup "$T"
        sed -i 's/^g_0x04 .*/g_0x04 0x1c/' "$M/i2c_1_0x4c" && cp "$M/i2c_1_0x4c" "$M/i2c_1_0x4c.orig"
        start_fake_app
        env_run "$sh" ssh hdmi_audio_test.sh out
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need skipped grep -q "Reg04 = 0x1c: a video/HDCP/RCLK reset or reserved bit is set" "$o"
        need only-bank-writes sh -c "! { grep 'W 1 0x4c' '$M/i2c_writes' | grep -q -v ' g_0x0f '; }"
        need no-soc-write nwrites 0
        need all-back all_back
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test: IT66121 Reg04 not as the stock init leaves it: nothing written" "$o"
        stop_apps
        # 6. SD-card mode: the app is started first, exactly once
        setup "$T"
        env_run "$sh" sd hdmi_audio_test.sh
        o="$R/mnt/extsd/hdmi_audio_test_1.txt"
        need one-app-running apps_ok 1
        need all-back all_back
        need no-violation no_viol
        need mode grep -q "mode sd" "$o"
        need ph-restored grep -q "PH_CFG0 restored: 0x35517751" "$o"
        verdict "$lbl: hdmi_audio_test sd: app started once, everything restored" "$o"
        stop_apps
        # 7. SD-card mode, the SD card does not take the report: /tmp instead, the app starts
        setup "$T"
        PROBE_OUTDIR=/proc env_run "$sh" sd hdmi_audio_test.sh out
        need one-app-running apps_ok 1
        need report-in-tmp test -s "$R/tmp/hdmi_audio_test_1.txt"
        need all-back all_back
        verdict "$lbl: hdmi_audio_test sd: report not writable on the SD card, app still started" "$R/tmp/hdmi_audio_test_1.txt"
        stop_apps

        # 8. killed at many points (TERM / INT / HUP): app resumed, everything back
        pts="0.05 0.15 0.3 0.5 0.7 0.9 1.1 1.4 1.7 2.0 2.4 2.8 3.3 3.9 4.6 5.4"
        [ -n "${QUICK:-}" ] && pts="0.2 0.9 1.7 2.8"
        nk=0
        nf=0
        ns=0
        sigs="TERM INT HUP"
        for d in $pts; do
            sig=${sigs%% *}
            sigs="${sigs#* } $sig"
            setup "$T"
            start_fake_app
            STUB_I2C_US=8000 env_bg "$sh" ssh hdmi_audio_test.sh
            sleep "$d"
            stopped_now && ns=$((ns + 1))
            kill -"$sig" "$BGPID" 2> /dev/null
            wait "$BGPID" 2> /dev/null
            nk=$((nk + 1))
            need one-app-running apps_ok 1
            need all-back all_back
            need no-violation no_viol
            need tone-removed test ! -e "$R/tmp/hdmi_tone.raw"
            need helpers-gone gone hdmi_audio_test.sh
            need app-not-left-stopped not_left_stopped "$R/mnt/extsd/hdmi_audio_test_1.txt"
            if [ -n "$WHY" ]; then
                nf=$((nf + 1))
                echo "  killed with $sig after $d s:$WHY"
                show "$R/mnt/extsd/hdmi_audio_test_1.txt"
                WHY=""
            fi
            stop_apps
        done
        if [ "$nf" = 0 ]; then ok "$lbl: hdmi_audio_test killed at $nk points ($ns while the app was stopped): app running, all registers back"; else bad "$lbl: hdmi_audio_test kill: $nf of $nk"; fi
        # 9. SIGKILL (no trap possible) while the app is stopped: the guardian resumes it
        setup "$T"
        start_fake_app
        STUB_I2C_US=20000 env_bg "$sh" ssh hdmi_audio_test.sh out
        i=0
        while [ "$i" -lt 200 ] && ! stopped_now; do
            sleep 0.02
            i=$((i + 1))
        done
        kill -KILL "$BGPID"
        wait "$BGPID" 2> /dev/null
        sleep 2.5
        need caught-stopped test "$i" -lt 200
        need one-app-running apps_ok 1
        verdict "$lbl: hdmi_audio_test SIGKILLed while the app is stopped: the guardian resumed it" /dev/null
        stop_apps
        pkill -f "$KIT/hdmi_audio_test.sh" 2> /dev/null
        ;;
    esac

    case " $GROUPS_ " in *" probe "*)
        for md in sd ssh; do
            setup "$T"
            [ "$md" = ssh ] && start_fake_app
            env_run "$sh" "$md" hdmi_audio_probe.sh
            o="$R/mnt/extsd/hdmi_audio_probe_1.txt"
            need part2 grep -q "part 2" "$o"
            need pio grep -q "PIO port H" "$o"
            need cards grep -q "snddaudio0" "$o"
            need dt grep -q 'allwinner,pins \[12\] = "PH1|PH2|PH3|"' "$o"
            need pinmux grep -q "(PH1)" "$o"
            need fifo-skipped grep -q "fifo-not-read" "$o"
            need no-fifo-read sh -c "! grep -q -E 'R 0x0509[0126]0(10|20)\$' '$M/soc_reads'"
            need no-ddc-fifo-read sh -c "! grep -q 'R 1 0x4c g_0x17' '$M/i2c_reads'"
            need no-soc-write nwrites 0
            need it-bank1-dumped grep -q "W 1 0x4c g_0x0f 0x09" "$M/i2c_writes"
            need ir-bank2-dumped grep -q "W 3 0x49 g_0x0f 0x02" "$M/i2c_writes"
            need banks-back all_back
            need no-violation no_viol
            need one-app-running apps_ok 1
            need no-mount-in-test sh -c "! grep -q mount '$M/mounts' 2> /dev/null"
            need helpers-gone gone hdmi_audio_probe.sh
            # PH_CFG0 two ways: the latch (and its raw answer) and /dev/mem through devmem, read only
            need ph-state grep -q "PH_CFG0 state: stock" "$o"
            need latch-bytes grep -q "bytes: 30 78 33 35 35 31 37 37 35 31 0a " "$o"
            need devmem-read grep -q -x "devmem 0x0300B0FC 32" "$M/services_busybox"
            need devmem-read-only sh -c "! grep -v -x -e '--list' -e 'devmem 0x0300B0FC 32' '$M/services_busybox'"
            need devmem-agree grep -q "/dev/mem and sunxi_dump agree" "$o"
            need devmem-file-gone test ! -e "$R/tmp/.probe_devmem"
            verdict "$lbl: probe $md: app runs once, read-only (no SoC write, no FIFO, no 0x17, devmem read only), banks back" "$o"
            stop_apps
        done
        # no services busybox: /dev/mem is not read, the rest runs as before
        setup "$T"
        rm -f "$R/mnt/app/services/busybox/busybox"
        start_fake_app
        env_run "$sh" ssh hdmi_audio_probe.sh
        o="$R/mnt/extsd/hdmi_audio_probe_1.txt"
        need no-devmem grep -q "/dev/mem not read: no devmem" "$o"
        need ph-state grep -q "PH_CFG0 state: stock" "$o"
        need part2 grep -q "part 2" "$o"
        need no-soc-write nwrites 0
        need no-violation no_viol
        need one-app-running apps_ok 1
        verdict "$lbl: probe without the services busybox: /dev/mem not read, the rest runs" "$o"
        stop_apps
        # an amixer that hangs and ignores TERM: stopped by the time limit, app started
        setup "$T"
        STUB_HANG_CARD=0 PROBE_TL=1 env_run "$sh" sd hdmi_audio_probe.sh
        o="$R/mnt/extsd/hdmi_audio_probe_1.txt"
        need stopped grep -q "stopped after 1 s: amixer -c 0 contents" "$o"
        need part2 grep -q "part 2" "$o"
        need one-app-running apps_ok 1
        verdict "$lbl: probe: a hanging command is stopped by its time limit" "$o"
        stop_apps
        pkill -f "amixer -c 0 contents" 2> /dev/null
        # the whole part 1 hangs: the guardian ends it and the app starts
        setup "$T"
        STUB_HANG_CARD=0 PROBE_TL=60 PROBE_GUARD=3 env_bg "$sh" sd hdmi_audio_probe.sh
        sleep 9
        need one-app-running apps_ok 1
        verdict "$lbl: probe sd: part 1 hangs, the guardian ends it, app started once" "$R/mnt/extsd/hdmi_audio_probe_1.txt"
        kill "$BGPID" 2> /dev/null
        wait "$BGPID" 2> /dev/null
        stop_apps
        pkill -f "amixer -c 0 contents" 2> /dev/null
        # SIGKILL in SD-card mode before the app was started: the guardian starts it
        setup "$T"
        env_bg "$sh" sd hdmi_audio_probe.sh
        sleep 0.5
        kill -KILL "$BGPID"
        wait "$BGPID" 2> /dev/null
        sleep 2.5
        need one-app-running apps_ok 1
        verdict "$lbl: probe sd SIGKILLed in part 1: the guardian started the app" "$R/mnt/extsd/hdmi_audio_probe_1.txt"
        stop_apps
        pkill -f "$KIT/hdmi_audio_probe.sh" 2> /dev/null
        # killed in SSH mode at many points
        nf=0
        nk=0
        ns=0
        pts="0.3 1.0 2.0 3.0 4.5 6.0 8.0"
        [ -n "${QUICK:-}" ] && pts="1.0 4.5"
        for d in $pts; do
            setup "$T"
            start_fake_app
            env_bg "$sh" ssh hdmi_audio_probe.sh
            sleep "$d"
            stopped_now && ns=$((ns + 1))
            kill -TERM "$BGPID" 2> /dev/null
            wait "$BGPID" 2> /dev/null
            nk=$((nk + 1))
            need one-app-running apps_ok 1
            need banks-back all_back
            need no-violation no_viol
            need helpers-gone gone hdmi_audio_probe.sh
            need app-not-left-stopped not_left_stopped "$R/mnt/extsd/hdmi_audio_probe_1.txt"
            if [ -n "$WHY" ]; then
                nf=$((nf + 1))
                echo "  killed after $d s:$WHY"
                show "$R/mnt/extsd/hdmi_audio_probe_1.txt"
                WHY=""
            fi
            stop_apps
        done
        if [ "$nf" = 0 ]; then ok "$lbl: probe killed at $nk points ($ns while the app was stopped): app running, bank registers back"; else bad "$lbl: probe kill: $nf of $nk"; fi
        ;;
    esac

    case " $GROUPS_ " in *" bay "*)
        # SD-card mode, stock goggle (sdmmc named in pinmux-pins, PG0-2 'IO disable'): runs
        setup "$T"
        env_run "$sh" sd bay_gpio_pattern.sh
        o="$R/mnt/extsd/bay_gpio_1.txt"
        need pattern grep -q "cycle 3: GPIO 194 high" "$o"
        need each-pin-driven sh -c "grep -q 'cycle 3: GPIO 192 high' '$o' && grep -q 'cycle 3: GPIO 193 high' '$o'"
        # (stub sleep: never two of GPIO 192-194 outputs at a time)
        need one-output-at-a-time sh -c "! grep -q 'outputs at once' '$M/violations'"
        need pg-restored grep -q "PG_CFG0 restored: 0x22777000 -> 0x22777777" "$o"
        need released dirs_released
        need all-back all_back
        need one-app-running apps_ok 1
        need no-violation no_viol
        need where grep -q "goggle-side connector" "$o"
        need voltage grep -q "3.3 V or 1.8 V" "$o"
        need step-4 grep -q "checklist step 4" "$o"
        verdict "$lbl: bay sd: runs on a stock goggle, one pin an output at a time, pins and PG_CFG0 back, app started once" "$o"
        stop_apps
        # SSH mode with the app running: refuses, starts nothing (no second app), stops nothing
        setup "$T"
        start_fake_app
        env_run "$sh" ssh bay_gpio_pattern.sh
        o="$R/mnt/extsd/bay_gpio_1.txt"
        need refused grep -q "REFUSED: the goggle app is running" "$o"
        need one-app-running apps_ok 1
        need nothing-driven test ! -s "$R/sys/class/gpio/gpio192/direction"
        verdict "$lbl: bay ssh with the app running: refused, no second app" "$o"
        stop_apps
        # SSH mode without an app (stopped by hand): runs, and does not start the app
        setup "$T"
        env_run "$sh" ssh bay_gpio_pattern.sh
        o="$R/mnt/extsd/bay_gpio_1.txt"
        need pattern grep -q "cycle 3: GPIO 194 high" "$o"
        need one-output-at-a-time sh -c "! grep -q 'outputs at once' '$M/violations'"
        need released dirs_released
        need all-back all_back
        need no-app-started apps_ok 0
        verdict "$lbl: bay ssh without the app: runs, one pin an output at a time, puts everything back, starts no app" "$o"
        stop_apps
        # SD-card mode but an app runs already: refused, still one app
        setup "$T"
        start_fake_app
        env_run "$sh" sd bay_gpio_pattern.sh
        o="$R/mnt/extsd/bay_gpio_1.txt"
        need refused grep -q "REFUSED" "$o"
        need one-app-running apps_ok 1
        verdict "$lbl: bay sd with an app running: refused, one app" "$o"
        stop_apps
        # Wi-Fi loaded / PG0-2 in use (sdc1 function) / GPIO requested: refused, nothing driven
        for case_ in wifi mux gpio; do
            setup "$T"
            case "$case_" in
                wifi) echo "xradio_wlan 300000 0 - Live 0x00000000" >> "$R/proc/modules" ;;
                mux) sed -i 's/^0x0300b0d8 .*/0x0300b0d8 0x22777222/' "$M/soc" && cp "$M/soc" "$M/soc.orig" ;;
                gpio) echo " gpio-193 (                    |module_dat          ) out lo" >> "$R/sys/kernel/debug/gpio" ;;
            esac
            env_run "$sh" sd bay_gpio_pattern.sh
            o="$R/mnt/extsd/bay_gpio_1.txt"
            need refused grep -q "REFUSED" "$o"
            need nothing-driven test ! -s "$R/sys/class/gpio/gpio192/direction"
            need no-soc-write nwrites 0
            need one-app-running apps_ok 1
            verdict "$lbl: bay refuses ($case_), nothing driven, app started" "$o"
            stop_apps
        done
        # killed at many points: pins released, PG_CFG0 back, app started once
        nf=0
        nk=0
        pts="0.2 0.6 1.0 1.6 2.4 3.4"
        [ -n "${QUICK:-}" ] && pts="0.6 2.4"
        for d in $pts; do
            setup "$T"
            env_bg "$sh" sd bay_gpio_pattern.sh
            sleep "$d"
            kill -TERM "$BGPID" 2> /dev/null
            wait "$BGPID" 2> /dev/null
            nk=$((nk + 1))
            need released dirs_released
            need one-app-running apps_ok 1
            need all-back all_back
            need no-violation no_viol
            need helpers-gone gone bay_gpio_pattern.sh
            need app-not-left-stopped not_left_stopped "$R/mnt/extsd/bay_gpio_1.txt"
            if [ -n "$WHY" ]; then
                nf=$((nf + 1))
                echo "  killed after $d s:$WHY"
                show "$R/mnt/extsd/bay_gpio_1.txt"
                WHY=""
            fi
            stop_apps
        done
        if [ "$nf" = 0 ]; then ok "$lbl: bay killed at $nk points: pins released, PG_CFG0 back, app started once"; else bad "$lbl: bay kill: $nf of $nk"; fi
        ;;
    esac
done

[ "$fail" = 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
