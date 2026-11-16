#!/bin/sh
# HDZero goggle (Goggle 1 / Goggle 2 / BoxPro) - HDMI audio + pin probe. Writes nothing
# persistent and changes no hardware setting; it only collects what the official OS image
# cannot tell (live sound cards, pin mux, I2S registers, IT66121 / IT66021 registers,
# dmesg) into one text file on the SD card. What it does touch, all at run time only:
#   - SIGSTOP/SIGCONT of the goggle app for short windows (< 1 s each) around register
#     access, always resumed (trap, and a guardian process if the script is killed);
#   - the bank-select register 0x0F of the two ITE chips, inside those windows: read with
#     the app stopped, switched for the dump, put back before the app continues;
#   - the sunxi_dump address latch (/sys/class/sunxi_dump/dump), inside those windows;
#     FIFO / data registers are never read (a read would take a sample from a stream);
#   - /dev/mem: one word (PH_CFG0) read, read only, with the devmem of the services
#     package's busybox if it is there (no value is ever passed to it);
#   - GPIO 131 (beeper) export and direction, as rc.sh does;
#   - debugfs is mounted (RAM only) if it is not, and unmounted again;
#   - SD-card mode: it starts the goggle app (see below).
#
# SD-card mode: copy this file to the SD card root as  develop.sh , boot. The firmware runs
#   /mnt/extsd/develop.sh INSTEAD of the goggle app (rc.sh), so this script starts the app
#   itself after part 1 (at the latest after 2 minutes, whatever happens). 2 beeps = part 1
#   done, app starting. Wait 2 more minutes (part 2 runs 60 s after the beeps), power off,
#   send hdmi_audio_probe_N.txt and hdmi_audio_dmesg_N.txt. Delete develop.sh afterwards.
#   For a run that covers HDMI in, select HDMI in within the minute after the 2 beeps.
# SSH mode (app running):  sh /mnt/extsd/hdmi_audio_probe.sh   (one pass, ~1 minute)
#
# POSIX sh for busybox 1.27 ash (no head/sort/find/printf/date/timeout needed).
# PROBE_ROOT, PROBE_MODE, PROBE_OUTDIR, PROBE_WAIT, PROBE_TL, PROBE_GUARD: PC tests only.

# shellcheck disable=SC2317,SC2034 # trap/guardian functions; common-block variables
VERSION=3
R="${PROBE_ROOT:-}"
# ===== probe common block v2: identical in every probe script (test/run_tests.sh checks) =====
# Modes. SD-card mode (MODE=sd): the file runs as /mnt/extsd/develop.sh, which rc.sh runs
# INSTEAD of the goggle app, so the script starts the app itself, exactly once, on every
# path (normal end, refusal, error, signal; if the script hangs or is killed, the guardian
# process starts it). SSH mode (MODE=ssh): started by hand; never starts the app.
# The goggle app is paused (SIGSTOP) only for short windows around chip/SoC register access
# and is always resumed: by the window itself, by the EXIT/INT/TERM/HUP trap, and by the
# guardian if the script dies without running its trap (SIGKILL).
case "${PROBE_MODE:-}" in
    sd | ssh) MODE=$PROBE_MODE ;;
    *) case "$0" in */develop.sh | develop.sh) MODE=sd ;; *) MODE=ssh ;; esac ;;
esac
MAIN=$$
APP="$R/mnt/app/app/HDZGOGGLE"
TMPD="$R/tmp"
DUMP="$R/sys/class/sunxi_dump"
OUT=/dev/null
OUTDIR=$TMPD
N=0
PAUSED=""
BGP=""
GUARD=""
CLEANED=0
RV=""
PST=""
PATH="$PATH:/usr/sbin:/usr/bin:/sbin:/bin"
export PATH

log() {
    echo "$*" >> "$OUT" 2> /dev/null
    [ -t 1 ] && echo "$*"
    return 0
}
have() { type "$1" > /dev/null 2>&1; } # the goggle's ash has no "command" builtin
hex() { awk -v v="$(($1))" 'BEGIN { printf "0x%08x", v }'; }
hex2() { awk -v v="$(($1))" 'BEGIN { printf "0x%02x", v }'; }
is_hex() { # 0x followed by hex digits only
    case "$1" in 0x*) ;; *) return 1 ;; esac
    case "${1#0x}" in "" | *[!0-9a-fA-F]*) return 1 ;; esac
    return 0
}
pause_us() { usleep "$1" 2> /dev/null || sleep 1; }

# PST = state letter of process $1 (R S D T Z ...), empty if it does not exist
pstate_of() {
    PST=""
    ps_l=""
    { read -r ps_l < "/proc/$1/stat"; } 2> /dev/null || return 1
    ps_l=${ps_l##*) }
    PST=${ps_l%% *}
    return 0
}
alive() { pstate_of "$1" && [ -n "$PST" ] && [ "$PST" != Z ]; }
app_pid() { pidof HDZGOGGLE 2> /dev/null; }
bg_forget() {
    bf=""
    for bf_p in $BGP; do [ "$bf_p" = "$1" ] || bf="$bf $bf_p"; done
    BGP=$bf
}

# The goggle firmware's own register tools (/usr/sbin/awr, aww; the app runs aww) share the
# single sunxi_dump address latch: while one runs, a read can return another register.
# shellcheck disable=SC2009 # busybox has no pgrep
latch_users() { ps 2> /dev/null | grep -E '(^|[ /{])aw[rw]([ }]|$)'; }

# Pause the goggle app: SIGSTOP, then wait until it is stopped and no aww/awr runs (an app
# child started before the stop). 1 = it did not settle within 2 s: resumed, do nothing.
app_pause() {
    PAUSED=$(app_pid)
    [ -n "$PAUSED" ] || return 0
    # shellcheck disable=SC2086 # pidof may return several pids
    kill -STOP $PAUSED 2> /dev/null
    pz=0
    while [ "$pz" -lt 40 ]; do
        pz_ok=1
        for pz_p in $PAUSED; do
            pstate_of "$pz_p"
            [ -z "$PST" ] || [ "$PST" = T ] || [ "$PST" = Z ] || pz_ok=0
        done
        [ "$pz_ok" = 1 ] && [ -z "$(latch_users)" ] && return 0
        pause_us 50000
        pz=$((pz + 1))
    done
    log "(the goggle app did not settle within 2 s, or aww/awr is running: step skipped)"
    app_resume
    return 1
}
app_resume() {
    # shellcheck disable=SC2086
    [ -n "$PAUSED" ] && kill -CONT $PAUSED 2> /dev/null
    PAUSED=""
    return 0
}
app_unstop_all() { # never leave a goggle app stopped (whoever stopped it)
    for ua_p in $(app_pid); do
        pstate_of "$ua_p"
        if [ "$PST" = T ]; then
            kill -CONT "$ua_p" 2> /dev/null
            log "(goggle app $ua_p was stopped: resumed)"
        fi
    done
    return 0
}

# SD-card mode only: start the goggle app as rc.sh does, unless one runs or was started.
start_app_once() {
    [ "$MODE" = sd ] || return 0
    [ -n "$(app_pid)" ] && return 0
    if ! mkdir "$TMPD/.probe_app_started" 2> /dev/null; then
        [ -d "$TMPD/.probe_app_started" ] && return 0 # started already (by this run or the guardian)
    fi
    if [ ! -x "$APP" ]; then
        log "(no $APP to start)"
        return 0
    fi
    log "starting the goggle app"
    if [ -e "$R/mnt/extsd/HDZGOGGLE.log" ]; then
        "$APP" > "$R/mnt/extsd/HDZGOGGLE.log" 2>&1 &
    else
        "$APP" &
    fi
    return 0
}

# Guardian: a separate process. If the script is still running after $1 s it sends TERM (the
# trap restores everything); once the script is gone, however it ended, it resumes a stopped
# app and (SD-card mode) starts the app.
guard_start() {
    gd=${PROBE_GUARD:-$1} # PROBE_GUARD: PC tests only
    (
        trap 'exit 0' TERM
        gs=0
        while alive "$MAIN" && [ "$gs" -lt "$gd" ]; do
            sleep 1
            gs=$((gs + 1))
        done
        if alive "$MAIN"; then
            log "(guardian: still running after $gd s, sending TERM)"
            kill -TERM "$MAIN" 2> /dev/null
            gs=0
            while alive "$MAIN" && [ "$gs" -lt 10 ]; do
                sleep 1
                gs=$((gs + 1))
            done
        fi
        app_unstop_all
        start_app_once
    ) < /dev/null > /dev/null 2>&1 &
    GUARD=$!
}

# Time limit for one external command; its output goes to the report. 124 = stopped.
tl() {
    tl_s=${PROBE_TL:-$1} # PROBE_TL: PC tests only
    shift
    "$@" >> "$OUT" 2>&1 < /dev/null &
    tl_c=$!
    BGP="$BGP $tl_c"
    tl_i=0
    tl_rc=0
    while alive "$tl_c"; do
        if [ "$tl_i" -ge $((tl_s * 10)) ]; then
            kill "$tl_c" 2> /dev/null
            log "(stopped after $tl_s s: $*)"
            tl_rc=124
            tl_k=0
            while alive "$tl_c" && [ "$tl_k" -lt 10 ]; do
                pause_us 100000
                tl_k=$((tl_k + 1))
            done
            alive "$tl_c" && kill -KILL "$tl_c" 2> /dev/null
            break
        fi
        pause_us 100000
        tl_i=$((tl_i + 1))
    done
    wait "$tl_c" 2> /dev/null
    tl_w=$?
    [ "$tl_rc" = 124 ] || tl_rc=$tl_w
    bg_forget "$tl_c"
    return "$tl_rc"
}

# Report file: $1_N.txt on the SD card (or /tmp); OUT stays /dev/null if nothing is writable.
out_open() {
    for od in "${PROBE_OUTDIR:-$R/mnt/extsd}" "$TMPD"; do
        if [ ! -d "$od" ] || [ ! -w "$od" ]; then continue; fi
        on=1
        while [ -e "$od/${1}_$on.txt" ] && [ "$on" -lt 1000 ]; do on=$((on + 1)); done
        # (a subshell: a failed redirection on a special builtin would end the script)
        if (: > "$od/${1}_$on.txt") 2> /dev/null; then
            OUT="$od/${1}_$on.txt"
            OUTDIR=$od
            N=$on
            return 0
        fi
    done
    return 1
}

beep() { # $1 count, $2 on-time in us
    bg_g="$R/sys/class/gpio"
    [ -d "$bg_g/gpio131" ] || echo 131 > "$bg_g/export" 2> /dev/null
    echo out > "$bg_g/gpio131/direction" 2> /dev/null
    bk=0
    while [ "$bk" -lt "$1" ]; do
        echo 1 > "$bg_g/gpio131/value" 2> /dev/null
        pause_us "${2:-150000}"
        echo 0 > "$bg_g/gpio131/value" 2> /dev/null
        pause_us 350000
        bk=$((bk + 1))
    done
}

# --- SoC registers through /sys/class/sunxi_dump (one address latch for every process) ---
# Only inside a lat_begin ... lat_end window: the app is paused (its aww cannot interleave)
# and the lock file is held (the record daemon's HDMI audio bridge takes the same lock).
lat_lock() {
    have flock && [ -d "$TMPD" ] && [ -w "$TMPD" ] || return 0
    # (a failed exec redirection ends the shell - checked above; the EXIT trap would still run)
    exec 9> "$TMPD/.sunxi_dump.lock"
    lk=0
    while ! flock -n 9 2> /dev/null; do
        lk=$((lk + 1))
        if [ "$lk" -ge 20 ]; then
            exec 9>&-
            return 1
        fi
        pause_us 100000
    done
    return 0
}
lat_unlock() {
    { exec 9>&-; } 2> /dev/null
    return 0
}
lat_begin() {
    if ! lat_lock; then
        log "(sunxi_dump lock held by another process for 2 s: step skipped)"
        return 1
    fi
    if ! app_pause; then
        lat_unlock
        return 1
    fi
    return 0
}
lat_end() {
    app_resume
    lat_unlock
}
# the only two places that touch the latch
lat_get() { echo "$1" > "$DUMP/dump" 2> /dev/null && cat "$DUMP/dump" 2> /dev/null; }
lat_put() { echo "$1 $2" > "$DUMP/write" 2> /dev/null; }
# one read, for counters and logs (RV = text as read)
lat_peek() { RV=$(lat_get "$(hex "$1")"); }
# verified read: three reads must agree and be hex; RV = value (decimal), else 1
lat_read() {
    RV=""
    lr_a=$(hex "$1")
    lr_1=$(lat_get "$lr_a")
    lr_2=$(lat_get "$lr_a")
    lr_3=$(lat_get "$lr_a")
    is_hex "$lr_1" && [ "$lr_1" = "$lr_2" ] && [ "$lr_2" = "$lr_3" ] || return 1
    RV=$((lr_1))
    return 0
}
# verified write: 0 only if the value reads back (verified)
lat_write() {
    lat_put "$(hex "$1")" "$(hex "$2")"
    lat_read "$1" && [ "$RV" -eq $(($2)) ]
}

cleanup() {
    [ "$CLEANED" = 1 ] && return 0
    CLEANED=1
    undo_window # the script's own: finish/close a chip register window that was open
    app_resume
    lat_unlock
    for cl_p in $BGP; do kill "$cl_p" 2> /dev/null; done
    for cl_p in $BGP; do # let them close their devices (at most 2 s each)
        cl_i=0
        while alive "$cl_p" && [ "$cl_i" -lt 20 ]; do
            pause_us 100000
            cl_i=$((cl_i + 1))
        done
        alive "$cl_p" && kill -KILL "$cl_p" 2> /dev/null
    done
    BGP=""
    undo # the script's own: put back what it changed
    app_resume
    lat_unlock
    app_unstop_all
    start_app_once
    [ -n "$GUARD" ] && kill "$GUARD" 2> /dev/null
    sync
    return 0
}
on_signal() {
    trap 'SIG_AGAIN=1' INT TERM HUP
    log "(signal $1: undoing what this script changed)"
    cleanup
    exit 1
}
trap 'cleanup' EXIT
trap 'on_signal INT' INT
trap 'on_signal TERM' TERM
trap 'on_signal HUP' HUP
# ===== end of probe common block =====

WAIT_APP="${PROBE_WAIT:-60}"
DBG="$R/sys/kernel/debug"
DT="$R/proc/device-tree"
PATH="$PATH:/mnt/app/app/record/audio"
DMESG_OUT=/dev/null
MOUNTED_DBG=0
ITE_WIN=0 # an ITE bank window is open: app paused, ITE_BUS/ITE_ADDR reg 0x0F = ITE_ORIG
ITE_BUS=""
ITE_ADDR=""
ITE_ORIG=0

sec() {
    log ""
    log "==================== $* ===================="
}
run() { # external command, at most 15 s
    log "\$ $*"
    if have "$1"; then
        tl 15 "$@"
        log "[exit $?]"
    else
        log "(command $1 not available)"
    fi
}
catf() {
    for f in "$@"; do
        if [ -r "$f" ] && [ -f "$f" ]; then
            log "--- $f"
            cat "$f" >> "$OUT" 2>&1
        else
            log "--- $f (missing)"
        fi
    done
}
grepf() { # lines matching an extended regex in a file, with a header
    log "--- $2 | grep -E '$1'"
    if [ -r "$2" ]; then grep -E "$1" "$2" >> "$OUT" 2>&1; else log "(missing)"; fi
}

# SoC registers: $1 start, $2 number of 32-bit words, $3 offsets (from $1) NOT to read:
# FIFO / data registers, where a CPU read takes a word out of a running stream.
# Each word is read twice (app paused, 8 words per window); "a/b?" = the two reads differ.
regs() {
    if [ ! -w "$DUMP/dump" ]; then
        log "(no $DUMP/dump, cannot read registers)"
        return
    fi
    rg_a=$(($1))
    rg_i=0
    rg_line=""
    rg_open=0
    while [ "$rg_i" -lt "$2" ]; do
        if [ "$rg_open" = 0 ]; then
            lat_begin || {
                log "(register window skipped)"
                return
            }
            rg_open=1
        fi
        rg_h=$(hex "$rg_a")
        rg_skip=0
        for rg_o in $3; do [ $((rg_a - $1)) -eq $((rg_o)) ] && rg_skip=1; done
        if [ "$rg_skip" = 1 ]; then
            rg_v="(fifo-not-read)"
        else
            lat_peek "$rg_a"
            rg_v=$RV
            lat_peek "$rg_a"
            [ "$rg_v" = "$RV" ] || rg_v="$rg_v/$RV?"
        fi
        rg_line="$rg_line $rg_h=$rg_v"
        rg_a=$((rg_a + 4))
        rg_i=$((rg_i + 1))
        [ $((rg_i % 4)) -eq 0 ] && log "$rg_line" && rg_line=""
        if [ $((rg_i % 8)) -eq 0 ]; then
            lat_end
            rg_open=0
        fi
    done
    [ "$rg_open" = 1 ] && lat_end
    [ -n "$rg_line" ] && log "$rg_line"
}
DAUDIO_FIFOS="0x10 0x20" # I2S/PCM_RXFIFO, I2S/PCM_TXFIFO (V536 datasheet; sunxi-daudio.h)

# One ITE chip: $1 bus, $2 address, $3 bank mask of reg 0x0F, then "bank:first-last,..."
# Bank 0x0F is read only after the app is stopped and put back before it continues.
ite_begin() {
    app_pause || return 1
    io1=$(i2cget -y "$1" "$2" 0x0f 2>&1)
    io2=$(i2cget -y "$1" "$2" 0x0f 2>&1)
    if is_hex "$io1" && [ "$io1" = "$io2" ]; then
        ITE_BUS=$1
        ITE_ADDR=$2
        ITE_ORIG=$((io1))
        ITE_WIN=1
        return 0
    fi
    log "reg 0x0F = '$io1' / '$io2' (chip not answering or unstable: no dump)"
    app_resume
    return 1
}
ite_end() {
    [ "$ITE_WIN" = 1 ] && i2cset -y "$ITE_BUS" "$ITE_ADDR" 0x0f "$(hex2 "$ITE_ORIG")" 2> /dev/null
    ITE_WIN=0
    app_resume
}
ite_dump() {
    id_bus=$1
    id_addr=$2
    id_mask=$3
    shift 3
    if ! have i2cdump || ! have i2cget || ! have i2cset; then
        log "(i2c-tools missing)"
        return
    fi
    ite_begin "$id_bus" "$id_addr" || return
    log "--- chip id (0x00..0x03): $(i2cget -y "$id_bus" "$id_addr" 0x00 2>&1) $(i2cget -y "$id_bus" "$id_addr" 0x01 2>&1) $(i2cget -y "$id_bus" "$id_addr" 0x02 2>&1) $(i2cget -y "$id_bus" "$id_addr" 0x03 2>&1)"
    log "reg 0x0F (bank select, app stopped) = $(hex2 "$ITE_ORIG")"
    ite_end
    for id_spec in "$@"; do
        id_b=${id_spec%%:*}
        ite_begin "$id_bus" "$id_addr" || return
        id_nv=$(hex2 $(((ITE_ORIG & ~id_mask) | id_b)))
        i2cset -y "$id_bus" "$id_addr" 0x0f "$id_nv" 2> /dev/null
        id_rs=$(echo "${id_spec#*:}" | tr ',' ' ')
        for id_r in $id_rs; do
            log "--- bank $id_b (0x0F=$id_nv): i2cdump -y -r $id_r $id_bus $id_addr b"
            i2cdump -y -r "$id_r" "$id_bus" "$id_addr" b >> "$OUT" 2>&1
        done
        ite_end
    done
    if ite_begin "$id_bus" "$id_addr"; then
        log "reg 0x0F after the dumps (app stopped) = $(hex2 "$ITE_ORIG")"
        ite_end
    fi
}

undo_window() {
    [ "$ITE_WIN" = 1 ] && ite_end
    return 0
}
undo() {
    if [ "$MOUNTED_DBG" = 1 ]; then
        umount "$DBG" 2> /dev/null && log "(debugfs unmounted)"
        MOUNTED_DBG=0
    fi
    rm -f "$DEVMEM_OUT" # (a signal during devmem_check)
    return 0
}

dt_props() { # every property of a DT node directory, recurse $2 levels
    [ -d "$1" ] || return
    log "--- node ${1#"$DT"}"
    for f in "$1"/*; do
        [ -e "$f" ] || continue
        if [ -d "$f" ]; then
            [ "$2" -gt 0 ] && dt_props "$f" $(($2 - 1))
            continue
        fi
        nm=${f##*/}
        case "$nm" in phandle | linux,phandle) continue ;; esac
        sz=$(wc -c < "$f" 2> /dev/null)
        s=$(tr '\000' '|' < "$f" 2> /dev/null)
        x=$(hexdump -v -n 64 -e '/1 "%02x"' "$f" 2> /dev/null)
        log "  $nm [$sz] = \"$s\"  hex:$x"
    done
}

# PH_CFG0 read two ways, read only: through the sunxi_dump latch (three reads, app paused) and
# through /dev/mem (no latch: the candidate for a latch-free access). The goggle's busybox has no
# devmem; the BusyBox 1.27.2 of the services package in the app partition (also in the official
# images) has. "devmem ADDRESS 32" with no value opens /dev/mem read only, maps the page read only
# and reads one word (busybox miscutils/devmem.c); nothing here passes a value.
BB_SERVICES="$R/mnt/app/services/busybox/busybox"
DEVMEM_OUT="$TMPD/.probe_devmem"
devmem_check() {
    if [ -e "$R/dev/mem" ]; then log "/dev/mem: $(ls -l "$R/dev/mem" 2>&1)"; else log "/dev/mem: absent"; fi
    dm_lat="(not read)"
    dm_raw=""
    if lat_begin; then
        if lat_read 0x0300B0FC; then dm_lat=$(hex "$RV"); else dm_lat="(three reads differ or not hex)"; fi
        # one more answer of the latch as bytes: the exact format src/record/soc_reg.c accepts
        dm_raw=$(lat_get 0x0300b0fc | hexdump -v -e '/1 "%02x "' 2> /dev/null)
        lat_end
    fi
    log "PH_CFG0 through sunxi_dump: $dm_lat; one answer of the dump node as bytes: $dm_raw (expected 0x%08x and a newline: 30 78 + 8 digits + 0a)"
    case "$dm_lat" in
        0x35517751) log "PH_CFG0 state: stock (as rc.sh writes it)" ;;
        0x35513331) log "PH_CFG0 state: PH1-PH3 = I2S0 (HDMI Out Audio switched them this boot)" ;;
        0x*) log "PH_CFG0 state: NEITHER the stock value nor the I2S0 value: HDMI Out Audio would refuse (report this)" ;;
    esac
    if [ ! -x "$BB_SERVICES" ] || ! "$BB_SERVICES" --list 2> /dev/null | grep -q -x devmem; then
        log "/dev/mem not read: no devmem ($BB_SERVICES missing or without the applet)"
        return
    fi
    rm -f "$DEVMEM_OUT"
    # read only (no value argument); the answer into a file of its own, error messages into the report
    # shellcheck disable=SC2016 # $0 and $1 are expanded by that sh
    tl 5 sh -c 'exec "$0" devmem 0x0300B0FC 32 > "$1"' "$BB_SERVICES" "$DEVMEM_OUT"
    dm_rc=$?
    dm_v=$(cat "$DEVMEM_OUT" 2> /dev/null)
    rm -f "$DEVMEM_OUT"
    log "PH_CFG0 through /dev/mem ($BB_SERVICES devmem 0x0300B0FC 32, exit $dm_rc): $dm_v"
    if is_hex "$dm_v" && is_hex "$dm_lat"; then
        if [ $((dm_v)) -eq $((dm_lat)) ]; then log "/dev/mem and sunxi_dump agree"; else log "/dev/mem and sunxi_dump DIFFER"; fi
    else
        log "/dev/mem read not usable (see above)"
    fi
}

pin_summary() {
    for d in "$DBG"/pinctrl/*/; do
        [ -r "${d}pinmux-pins" ] && grep -E '\((P(G([0-9]|1[0-3])|H[0-8]|E(1[6-9]|2[01])))\)' "${d}pinmux-pins" >> "$OUT" 2>&1
    done
}

######################################################################
part1() {
    sec "hdmi_audio_probe v$VERSION part 1 (static), mode $MODE"
    catf "$R/proc/version" "$R/proc/cmdline" "$R/proc/uptime"
    run uname -a
    catf "$R/mnt/app/version" "$R/mnt/app/platform" "$R/etc/openwrt_version"
    log "app running at start: pid '$(app_pid)'"
    run ps

    sec "kernel config"
    if [ -r "$R/proc/config.gz" ]; then
        cp "$R/proc/config.gz" "$OUTDIR/hdmi_audio_config_$N.gz" 2> /dev/null && log "copied /proc/config.gz to hdmi_audio_config_$N.gz"
    else
        log "/proc/config.gz absent (the shipped kernels have no IKCONFIG)"
    fi
    run lsmod
    catf "$R/proc/modules" "$R/proc/filesystems"
    run ls -l "$R/lib/modules" "$R/mnt/app/ko"

    sec "ALSA"
    run ls -l "$R/proc/asound" "$R/dev/snd"
    catf "$R/proc/asound/version" "$R/proc/asound/cards" "$R/proc/asound/devices" "$R/proc/asound/pcm" "$R/proc/asound/timers"
    for c in "$R"/proc/asound/card[0-9]*; do
        [ -d "$c" ] || continue
        catf "$c/id"
        for p in "$c"/pcm*; do
            [ -d "$p" ] || continue
            catf "$p/info" "$p/sub0/info" "$p/sub0/hw_params" "$p/sub0/sw_params" "$p/sub0/status"
        done
    done
    cards=$(awk '/^ *[0-9]+ \[/ { print $1 }' "$R/proc/asound/cards" 2> /dev/null)
    for c in $cards; do
        run amixer -c "$c" info
        run amixer -c "$c" contents
    done
    run aplay -l
    run arecord -l
    run aplay -L

    sec "ASoC debugfs / clocks"
    if ! grep -q debugfs "$R/proc/mounts" 2> /dev/null; then
        if [ -z "$R" ] && mount -t debugfs none /sys/kernel/debug 2> /dev/null; then
            MOUNTED_DBG=1
            log "(mounted debugfs on /sys/kernel/debug, RAM only; unmounted at the end)"
        else
            log "(debugfs not mounted and could not be mounted)"
        fi
    fi
    catf "$DBG/asoc/codecs" "$DBG/asoc/dais" "$DBG/asoc/platforms"
    grepf 'i2s|daudio|audio|codec|pll_audio|hdmi|dmic' "$DBG/clk/clk_summary"
    grepf 'daudio|i2s|codec|audio|dma|mmc|sdc' "$R/proc/interrupts"

    sec "device tree: sound / i2s / codec / hdmi / sdc1 / wlan / pin groups"
    soc=""
    for s in "$DT"/soc*; do [ -d "$s" ] && soc="$s"; done
    for nd in "$soc"/daudio* "$soc"/sound* "$soc"/codec* "$soc"/i2s* "$soc"/dmic* "$soc"/hdmi* "$soc"/sdmmc@04021000 "$soc"/uart@05001000; do
        [ -d "$nd" ] && dt_props "$nd" 0
    done
    for nd in "$DT"/snddaudo* "$DT"/wlan "$DT"/aliases; do
        [ -d "$nd" ] && dt_props "$nd" 0
    done
    pc="$soc/pinctrl@0300b000"
    for nd in "$pc"/daudio* "$pc"/aif* "$pc"/sdc1* "$pc"/uart4* "$pc"/uart3* "$pc"/dmic* "$pc"/hdmi*; do
        [ -d "$nd" ] && dt_props "$nd" 0
    done
    run ls "$R/sys/class"
    run ls -l "$R/sys/class/script"

    sec "pin control (debugfs) - after rc.sh"
    for d in "$DBG"/pinctrl/*/; do
        [ -d "$d" ] || continue
        for f in pinmux-pins pins pinmux-functions pingroups pinconf-pins gpio-ranges; do
            catf "$d$f"
        done
    done
    sec "pin mux summary: PE16-21, PG0-13, PH0-8 (bay PG0-2 = GPIO 192-194, I2S0 PH1-3, I2S2 PG10/11/13)"
    pin_summary
    catf "$DBG/gpio"
    run ls "$R/sys/class/gpio"

    sec "SoC registers (sunxi_dump; two reads per word, app paused, FIFO registers skipped)"
    log "PIO port E CFG0..PULL1 (0x0300B090..B0B0), rc.sh writes PE_CFG2 0x0300B098=0x00775577"
    regs 0x0300B090 9
    log "PIO port G CFG0..PULL1 (0x0300B0D8..B0F8), rc.sh writes PG_CFG0 0x0300B0D8=0x22777777 (PG0-5 off, PG6/7 uart1)"
    regs 0x0300B0D8 9
    log "PIO port H CFG0..PULL1 (0x0300B0FC..B11C), rc.sh writes PH_CFG0 0x0300B0FC=0x35517751 (PH1 uart4, PH2/3 off)"
    regs 0x0300B0FC 9
    log "I2S0 daudio@0x05090000 (CTL FMT0 FMT1 ISTA [RXFIFO] FCTL FSTA INT [TXFIFO] CLKD TXCNT RXCNT CHCFG ...)"
    regs 0x05090000 29 "$DAUDIO_FIFOS"
    log "I2S1 / tdmhdmi daudio@0x05091000"
    regs 0x05091000 29 "$DAUDIO_FIFOS"
    log "I2S2 daudio@0x05092000"
    regs 0x05092000 29 "$DAUDIO_FIFOS"
    log "codec internal I2S 0x05096000.. (first 16 words, DA_RXFIFO/DA_TXFIFO skipped) and mic bias 0x050967c0"
    regs 0x05096000 16 "$DAUDIO_FIFOS"
    regs 0x050967c0 1
    log "CCU: PLL_AUDIO area 0x03001070.. and audio/I2S clock + gating area 0x03001A00..0x03001A7C"
    regs 0x03001070 4
    regs 0x03001A00 32

    sec "PH_CFG0 through sunxi_dump and through /dev/mem (read only); HDMI audio boot record"
    devmem_check
    catf "$TMPD/hdmi_audio.boot"

    sec "I2C"
    run i2cdetect -l
    run ls -l "$R"/dev/i2c-*
}

part2() {
    sec "hdmi_audio_probe v$VERSION part 2 (app running: pid '$(app_pid)')"
    catf "$R/proc/uptime"
    run ps
    sec "ALSA streams now"
    for c in "$R"/proc/asound/card[0-9]*; do
        for p in "$c"/pcm*; do
            [ -d "$p" ] && catf "$p/sub0/hw_params" "$p/sub0/status"
        done
    done
    sec "pin mux after app start"
    pin_summary
    catf "$DBG/gpio"
    regs 0x0300B0D8 9
    regs 0x0300B0FC 9
    log "I2S0 / I2S2 again (TXCNT 0x28 / RXCNT 0x2C count samples only while clocks run)"
    regs 0x05090000 29 "$DAUDIO_FIFOS"
    regs 0x05092000 29 "$DAUDIO_FIFOS"

    sec "IT66121 HDMI out (bus 1, 0x4c): bank 0 (0x17 = DDC FIFO not read) and bank 1"
    ite_dump 1 0x4c 0x01 "0:0x00-0x16,0x18-0xff" "1:0x30-0xbf"
    sec "IT66021 HDMI in (bus 3, 0x49): banks 0, 1, 2; EDID RAM (0x54)"
    ite_dump 3 0x49 0x03 "0:0x00-0xff" "1:0x00-0xff" "2:0x00-0xff"
    run i2cdump -y 3 0x54 b
    sec "dmesg (audio/pin related lines; full log in $(basename "$DMESG_OUT"))"
    if have dmesg; then
        dmesg > "$DMESG_OUT" 2>&1
        grep -i -E 'daudio|snd|sound|audio|codec|i2s|hdmi|pinctrl|pin [0-9]|sdc1|mmc1|uart4|xradio|wlan|asoc' "$DMESG_OUT" >> "$OUT" 2>&1
    fi
    sec "tools present"
    run busybox --list
    run ls /bin /sbin /usr/bin /usr/sbin /mnt/app/app/record/audio
}

######################################################################
if [ "$MODE" = sd ]; then
    guard_start 120 # part 1 hangs: TERM after 2 minutes, the trap starts the app
else
    guard_start 600
fi
out_open hdmi_audio_probe && DMESG_OUT="$OUTDIR/hdmi_audio_dmesg_$N.txt"
part1
sync

if [ "$MODE" = sd ]; then
    start_app_once
    beep 2
    log ""
    log "goggle app started, part 2 in $WAIT_APP s"
    sync
    [ -n "$GUARD" ] && kill "$GUARD" 2> /dev/null
    guard_start $((WAIT_APP + 180))
    sleep "$WAIT_APP"
elif [ -z "$(app_pid)" ]; then
    log "(SSH mode and no goggle app running: part 2 runs without it; this script never starts it)"
fi
part2
sec "end"
exit 0
