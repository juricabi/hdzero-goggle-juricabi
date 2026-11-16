#!/bin/sh
# OPTIONAL, ACTIVE TEST - drives pins. Analog module bay EMPTY (no module inserted).
# It is NOT part of the read-only probe. SD-card mode only (see below).
#
# Purpose: (a) confirm where GPIO 192 / 193 / 194 (PG0 / PG1 / PG2 = GPIO_MODULE_CLK /
# _DAT / _CS in src/core/defines.h) arrive, and (b) read their high level: about 3.3 V or
# about 1.8 V (the SoC reports port G's supply in PIO_POW_VAL, which Check Pins shows; this
# is for when that reads "? V", or to confirm which connector pin is which).
# Where (pkendall64's wiki "Analog Module Backpack Control", Hardware mods, original HDZero
# Goggle): the three signals are on the connector between the module-bay board and the
# goggle, the GOGGLE-SIDE connector of the bay board. On a stock goggle they do NOT reach
# the 9-pin analog module connector; only after the 3-wire mod of the wiki do they also
# appear on it. How to measure: docs/module-bay-mod-safety.md, checklist step 4: one clip
# on one pin per boot, attached with the goggle OFF and checked with an ohmmeter (above
# 10 ohms to ground, the supply and each neighbouring pin); never move a probe on the
# connector while the goggle is on.
# Only the pin under test is ever an output: the other two stay inputs (high impedance) all
# the time, so a clip that also touches a neighbouring pin never joins two driven outputs.
# Pattern, repeated 3 times:
#   nothing driven (all three inputs) ......................... 10 s
#   1 beep  -> GPIO 192 (PG0) output, high, others inputs ..... 10 s, then input again, 3 s
#   2 beeps -> GPIO 193 (PG1) output, high, others inputs ..... 10 s, then input again, 3 s
#   3 beeps -> GPIO 194 (PG2) output, high, others inputs ..... 10 s, then input again, 3 s
#   finally 4 long beeps: pins put back exactly as found, goggle app started.
# The pin that reads high right after N beeps is GPIO 191+N; note its voltage.
#
# Refuses (one long beep, nothing driven, reason in the report) unless all hold:
#   - the goggle app is not running (its module-bay code drives the same pins);
#   - the Wi-Fi driver (xradio) is not loaded and no SDIO card is on mmc1 (the device
#     tree gives PG0-PG5 to the SDIO host sdc1 for Wi-Fi; pinmux-pins therefore names
#     the sdmmc as their owner on every goggle - that alone is not a reason);
#   - PG0-PG2 are in the state rc.sh leaves them: function 7 "IO disable" in PG_CFG0
#     (rc.sh writes PG_CFG0 = 0x22777777), read three times with the same result;
#   - GPIO 192-194 are not requested by anyone.
# Afterwards each pin gets its original data bit and function back (PG_CFG0 PG0-PG2
# fields only, verified read-modify-write), and is released.
#
# SD-card mode: copy this file as develop.sh to the SD card root, empty the bay, boot,
#   listen. The goggle app starts when the pattern is done (or refused, or interrupted).
#   The report is bay_gpio_N.txt on the SD card. Delete develop.sh afterwards.
# SSH mode: only with the goggle app stopped (it refuses while it runs, and never starts
#   it: reboot afterwards).
# POSIX sh for busybox 1.27 ash. PROBE_ROOT, PROBE_MODE, PROBE_FAST, PROBE_TL, PROBE_GUARD: PC tests only.

# shellcheck disable=SC2317,SC2034 # trap/guardian functions; common-block variables
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

G="$R/sys/class/gpio"
DBG="$R/sys/kernel/debug"
HOLD=10
GAP=3
[ -n "${PROBE_FAST:-}" ] && HOLD=0 && GAP=0
PINS="192 193 194"
PG_CFG0=0x0300B0D8 # PG0..PG7 function, 4 bits each (V536 datasheet: 7 = IO disable)
PG_DAT=0x0300B0E8
PG_PULL0=0x0300B0F4
PG_MASK=0xfff # PG0..PG2 fields
PG_ORIG=""
DAT_ORIG=""
EXPORTED=""
MOUNTED_DBG=0

refuse() {
    log "REFUSED: $*"
    log "Nothing was driven."
    beep 1 1500000
    exit 1 # the EXIT trap starts the app in SD-card mode (never a second one)
}
pg_fields() { # "f0 f1 f2" = PG0..PG2 function numbers of a PG_CFG0 value
    echo "$(($1 & 7)) $((($1 >> 4) & 7)) $((($1 >> 8) & 7))"
}
pg_all() { # $1 value, $2 allowed function list: every PG0..PG2 field in it
    for pa_f in $(pg_fields "$1"); do
        case " $2 " in *" $pa_f "*) ;; *) return 1 ;; esac
    done
    return 0
}

pins_release() {
    [ -n "$EXPORTED" ] || return 0
    for p in $EXPORTED; do # original data bit, then input (high impedance)
        b=0
        [ -n "$DAT_ORIG" ] && b=$(((DAT_ORIG >> (p - 192)) & 1))
        echo "$b" > "$G/gpio$p/value" 2> /dev/null
        echo in > "$G/gpio$p/direction" 2> /dev/null
    done
    for p in $EXPORTED; do echo "$p" > "$G/unexport" 2> /dev/null; done
    if [ -n "$DAT_ORIG" ]; then
        log "GPIO$EXPORTED: data bits back to PG_DAT $(hex "$DAT_ORIG") bits 0-2, inputs, unexported"
    else
        log "GPIO$EXPORTED: data bits 0 (original not readable), inputs, unexported"
    fi
    EXPORTED=""
    # function back to what it was (IO disable): only the PG0-PG2 fields, and only if the
    # rest of PG_CFG0 is unchanged and PG0-PG2 are what the GPIO driver leaves (in/out)
    [ -n "$PG_ORIG" ] || return 0
    if ! lat_begin; then
        log "PG_CFG0 NOT restored (no window): the pins are inputs; a reboot restores rc.sh's setting"
        return 1
    fi
    if lat_read $PG_CFG0 && [ $((RV & ~PG_MASK)) -eq $((PG_ORIG & ~PG_MASK)) ] && pg_all "$RV" "0 1 7"; then
        cur=$RV
        back=$(((cur & ~PG_MASK) | (PG_ORIG & PG_MASK)))
        if lat_write $PG_CFG0 "$back"; then
            log "PG_CFG0 restored: $(hex "$cur") -> $(hex "$back")"
        else
            log "PG_CFG0 restore did not read back: the pins are inputs; a reboot restores rc.sh's setting"
        fi
    else
        log "PG_CFG0 read not trusted or other pins changed ($(hex "${RV:-0}")): NOT restored; the pins are inputs"
    fi
    PG_ORIG=""
    lat_end
}
undo_window() { return 0; }
undo() {
    pins_release
    if [ "$MOUNTED_DBG" = 1 ]; then
        umount "$DBG" 2> /dev/null
        MOUNTED_DBG=0
    fi
    return 0
}

guard_start 600 # the pattern takes about 3 minutes
out_open bay_gpio
log "bay_gpio_pattern, mode $MODE: checks"

# 1. the goggle app must not run (its module-bay code drives the same pins)
if [ -n "$(app_pid)" ]; then
    log "goggle app pid: $(app_pid)"
    if [ "$MODE" = sd ]; then
        refuse "the goggle app is already running"
    else
        refuse "the goggle app is running: use SD-card mode (this file as /mnt/extsd/develop.sh); this script does not start or stop it"
    fi
fi

# 2. Wi-Fi / SDIO on sdc1 (PG0-PG5)
if grep -q -E '^(xradio|sunxi_wlan)' "$R/proc/modules" 2> /dev/null; then
    refuse "Wi-Fi driver loaded ($(grep -E '^(xradio|sunxi_wlan)' "$R/proc/modules" | awk '{ print $1 }' | tr '\n' ' ')): PG0-PG5 may be the SDIO bus"
fi
for c in "$R"/sys/bus/mmc/devices/mmc1:*; do
    [ -e "$c" ] && refuse "an SDIO/MMC card is present on mmc1 ($c): PG0-PG5 are in use"
done

# 3. debugfs (RAM only): nobody may have requested the GPIOs
if ! grep -q debugfs "$R/proc/mounts" 2> /dev/null; then
    if [ -z "$R" ] && mount -t debugfs none /sys/kernel/debug 2> /dev/null; then
        MOUNTED_DBG=1
        log "(debugfs mounted, RAM only)"
    fi
fi
[ -r "$DBG/gpio" ] || refuse "cannot read $DBG/gpio: whether GPIO 192-194 are in use is unknown"
for p in $PINS; do
    l=$(grep -E "gpio-${p}[ (]" "$DBG/gpio")
    [ -n "$l" ] && refuse "GPIO $p is requested: $l"
done
for d in "$DBG"/pinctrl/*/; do # for the record only: the DT gives PG0-PG5 to sdc1 (sdmmc)
    [ -r "${d}pinmux-pins" ] && grep -E '^pin 19[234] ' "${d}pinmux-pins" | while read -r l; do log "pinmux: $l"; done
done

# 4. PG0-PG2 must be as rc.sh leaves them: function 7, IO disable
lat_begin || refuse "could not get a register window"
if lat_read $PG_CFG0; then
    PG_ORIG=$RV
    lat_read $PG_DAT && DAT_ORIG=$RV
    lat_peek $PG_PULL0
    pull=$RV
fi
lat_end
[ -n "$PG_ORIG" ] || refuse "PG_CFG0 does not read the same three times"
log "PG_CFG0 = $(hex "$PG_ORIG") (PG0-PG2 functions: $(pg_fields "$PG_ORIG")), PG_DAT = ${DAT_ORIG:+$(hex "$DAT_ORIG")} PG_PULL0 = $pull"
if ! pg_all "$PG_ORIG" "7"; then
    p0=$PG_ORIG
    PG_ORIG=""
    refuse "PG0-PG2 are not 'IO disable' (PG_CFG0 = $(hex "$p0")): something uses them"
fi
log "checks passed, driving the pattern (hold ${HOLD}s); only the pin under test is an output"
log "the clip on ONE pin of the goggle-side connector of the module-bay board (after the 3-wire mod"
log "also the 9-pin module connector), attached with the goggle off (docs/module-bay-mod-safety.md,"
log "checklist step 4), meter against ground, bay EMPTY:"
log "  after 1 beep: GPIO 192 (PG0) high  -> does your pin go high? voltage (about 3.3 V or 1.8 V)?"
log "  after 2 beeps: GPIO 193 (PG1) high -> ?"
log "  after 3 beeps: GPIO 194 (PG2) high -> ?"
sync

# Every pin an input first; then each in turn an output (low, high, low) and an input again:
# never two of them outputs at the same time (a clip touching two pins joins no outputs).
for p in $PINS; do
    echo "$p" > "$G/export" 2> /dev/null
    EXPORTED="$EXPORTED $p"
    echo in > "$G/gpio$p/direction" 2> /dev/null || refuse "cannot make GPIO $p an input"
done

stop() { # (pins may have been driven already: the EXIT trap puts them back)
    log "STOPPED: $*"
    beep 1 1500000
    exit 1
}
cycle=1
while [ "$cycle" -le 3 ]; do
    log "cycle $cycle: nothing driven (all inputs)"
    sleep "$HOLD"
    i=1
    for p in $PINS; do
        beep "$i"
        echo low > "$G/gpio$p/direction" 2> /dev/null || stop "cannot make GPIO $p a low output"
        sleep 1
        log "cycle $cycle: GPIO $p high (announced by $i beep(s)), the other two inputs"
        echo 1 > "$G/gpio$p/value"
        sleep "$HOLD"
        echo 0 > "$G/gpio$p/value"
        echo in > "$G/gpio$p/direction" 2> /dev/null
        sleep "$GAP"
        i=$((i + 1))
    done
    cycle=$((cycle + 1))
done

pins_release
log "done"
[ "$MODE" = ssh ] && log "SSH mode: this script does not start the goggle app; reboot the goggle"
sync
beep 4 600000
exit 0
