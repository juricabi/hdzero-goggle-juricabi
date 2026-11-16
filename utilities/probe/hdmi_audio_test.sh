#!/bin/sh
# OPTIONAL, ACTIVE TEST of the HDMI audio paths. At run time only it writes IT66121 audio
# registers, the PH1-PH3 pin function (PH_CFG0) and two fields of I2S0 CTL; it undoes
# exactly what it changed on every exit path (end, error, Ctrl-C, kill, hang-up) and never
# writes flash. A power cycle also undoes everything (rc.sh sets the pins at boot).
#
# Part A, HDMI OUT: TV/monitor with speakers on the goggle HDMI out, source NOT HDMI in
#   (the app switches HDMI out off while HDMI in is shown). Programs the IT66121 audio
#   input, plays a 1 kHz tone (-12 dBFS) for 15 s into ALSA hw:snddaudio0 (I2S0) and logs
#   the IT66121 CTS counter (non-zero = an I2S clock reaches the chip). Tone on the TV =
#   HDMI out audio works.
# Part B, HDMI IN: an HDMI source playing sound on the goggle HDMI in, source HDMI in
#   selected. Records 10 s from hw:snddaudio2 (I2S2) into hdmi_in_test_N.wav and logs the
#   I2S2 sample counter and the IT66021 registers. Play the WAV on a PC. No register writes
#   except the IT66021 bank-select register (put back at once).
#
# SD-card mode: copy this file as develop.sh plus tone1k_48k_s16le_2ch.raw to the SD card
#   root and boot. The script starts the goggle app FIRST, then
#   45 s later 1 beep -> part A (tone 15 s on HDMI out), 2 beeps = A done;
#   then you have 60 s to select HDMI in, 3 beeps -> part B (10 s), 4 beeps = done.
#   Delete develop.sh afterwards.
# SSH mode (app running):  sh hdmi_audio_test.sh [out|in]   (Enter between the steps)
# Report: hdmi_audio_test_N.txt on the SD card.
# Register reads go through the sunxi_dump latch only while the app is paused, are read
# three times, must agree and pass a plausibility check before anything is written from
# them; when in doubt nothing is written.
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

HERE=$(dirname "$0")
TONE_SRC="$HERE/tone1k_48k_s16le_2ch.raw"
W1=45
W2=60
TONE_S=15
REC_S=10
[ -n "${PROBE_FAST:-}" ] && W1=0 && W2=0 && TONE_S=1 && REC_S=1
PATH="$PATH:/mnt/app/app/record/audio"
WHAT="${1:-both}"

# state of everything this script changes (undo() puts back exactly this)
IT_WIN=0   # an IT66121 register window is open: app paused, bank register = IT_BANK
IT_BANK=0
IT_ON=0    # IT66121 audio registers programmed by this script (S0/S1 hold the saved values)
S0=""
S1=""
IR_WIN=0   # same for the IT66021 bank register
IR_BANK=0
PH_SET="" # PH_AUDIO while this script has PH1-PH3 switched (ph_restore puts PH_STOCK back)
CTL_ORIG="" # I2S0 CTL before, and the value this script wrote
CTL_SET=""
TONE_FILE=""

interactive() { [ -t 0 ] && [ -t 1 ]; }
nap() { [ -n "${PROBE_FAST:-}" ] || sleep "$1"; }
step() { # $1 message, $2 seconds to wait when not interactive, $3 beeps
    log ""
    log ">>> $1"
    if interactive; then
        echo "    press Enter to continue"
        read -r _
    else
        sleep "$2"
        beep "${3:-1}"
    fi
}

# ---- IT66121 (bus 1, 0x4c); reg 0x0F bit 0 selects bank 1 (0x130-0x1BF) ----
B=1
A=0x4c
IV=0
it_r() { i2cget -y $B $A "$1" 2> /dev/null; }
it_w() { i2cset -y $B $A "$1" "$2" 2> /dev/null; }
it_rv() { # verified read: two reads agree; IV = value
    iv1=$(it_r "$1")
    iv2=$(it_r "$1")
    is_hex "$iv1" && [ "$iv1" = "$iv2" ] || return 1
    IV=$((iv1))
}
it_b0() { it_w 0x0f "$(hex2 $((IT_BANK & ~1)))"; }
it_b1() { it_w 0x0f "$(hex2 $((IT_BANK | 1)))"; }
it_begin() { # pause the app; remember the bank register to put it back
    app_pause || return 1
    [ "$IT_WIN" = 1 ] && return 0 # a window a signal interrupted: keep its bank value
    if it_rv 0x0f; then
        IT_BANK=$IV
        IT_WIN=1
        return 0
    fi
    log "IT66121 bank register 0x0F not readable"
    app_resume
    return 1
}
it_end() {
    [ "$IT_WIN" = 1 ] && it_w 0x0f "$(hex2 "$IT_BANK")"
    IT_WIN=0
    app_resume
}

SAVE0="0x04 0x58 0xce 0xe0 0xe1 0xe2 0xe3 0xe4 0xe5"
SAVE1="0x33 0x34 0x35 0x91 0x92 0x93 0x94 0x98 0x99 0x68 0x69 0x6a 0x6b 0x6c 0x6d"
it_save() { # inside a window; every value read twice
    S0=""
    S1=""
    it_b0
    for r in $SAVE0; do
        it_rv "$r" || return 1
        S0="$S0 $r=$(hex2 "$IV")"
    done
    it_b1
    for r in $SAVE1; do
        it_rv "$r" || return 1
        S1="$S1 $r=$(hex2 "$IV")"
    done
    it_b0
    log "IT66121 saved bank0:$S0"
    log "IT66121 saved bank1:$S1"
}
saved() { for kv in $S0 $S1; do [ "${kv%=*}" = "$1" ] && echo "${kv#*=}" && return 0; done; }
it_restore() { # the saved values (Reg04 among them: only saved if its reset bits were safe)
    it_b1
    for kv in $S1; do it_w "${kv%=*}" "${kv#*=}"; done
    it_b0
    for kv in $S0; do it_w "${kv%=*}" "${kv#*=}"; done
}
# Reg04 holds the RCLK (bit 5), video (bit 3) and HDCP (bit 0) resets besides the two audio resets
# (bits 4, 2); bits 7, 6, 1 are reserved. The stock init leaves 0x14 (only the audio resets set),
# HDMI Out Audio in effect leaves 0x00. Only those states are accepted, and only the constants
# 0x04 and 0x00 are written (as src/driver/it66121.c): no write here can assert a video reset.
it04_ok() { [ $(($1 & 0xeb)) -eq 0 ]; }
it_audio_on() { # the audio part of IT66121_init() in src/driver/it66121.c (no RegC5 write: default)
    it_b0
    it_w 0xe0 0x00
    it_w 0x04 0x04
    it_w 0x58 0x11
    it_b1
    it_w 0x33 0x00
    it_w 0x34 0x18
    it_w 0x35 0x00
    it_b0
    it_w 0xe0 0x00
    it_w 0xe1 0x01
    it_w 0xe2 0xe4
    it_w 0xe3 0x00
    it_w 0xe4 0x00
    it_w 0xe5 0x00
    it_b1
    it_w 0x91 0x00
    it_w 0x92 0x00
    it_w 0x93 0x01
    it_w 0x94 0x02
    it_w 0x98 0x02
    it_w 0x99 0xd2
    it_w 0x68 0x01
    it_w 0x69 0x00
    it_w 0x6a 0x00
    it_w 0x6b 0x00
    it_w 0x6c 0x00
    it_w 0x6d 0x70
    it_b0
    it_w 0xce 0x03
    it_w 0x04 0x00
    it_w 0xe0 0x01
}
it_status() { # inside a window
    it_b0
    s="04=$(it_r 0x04) 5F=$(it_r 0x5f) E0=$(it_r 0xe0) E1=$(it_r 0xe1) C0=$(it_r 0xc0) C1=$(it_r 0xc1) C5=$(it_r 0xc5) CE=$(it_r 0xce)"
    it_b1
    s="$s | 130-137: $(it_r 0x30) $(it_r 0x31) $(it_r 0x32) $(it_r 0x33) $(it_r 0x34) $(it_r 0x35) $(it_r 0x36) $(it_r 0x37) | 1A0-1A2: $(it_r 0xa0) $(it_r 0xa1) $(it_r 0xa2)"
    it_b0
    log "IT66121: $s"
}
it_undo() {
    if [ "$IT_ON" != 1 ]; then
        [ "$IT_WIN" = 1 ] && it_end
        return 0
    fi
    was_open=$IT_WIN
    if ! it_begin; then
        log "IT66121 audio registers NOT restored (app did not pause): a source switch or power cycle resets them"
        return 1
    fi
    it_b0
    # restore only what this script set: unless a window was still open (app paused all
    # along), E0/CE must still be this script's values, otherwise the app re-initialised it
    if [ "$was_open" = 1 ] || { it_rv 0xe0 && [ "$IV" -eq 1 ] && it_rv 0xce && [ "$IV" -eq 3 ]; }; then
        it_restore
        log "IT66121 audio registers restored"
    else
        log "IT66121 E0/CE are not this script's values (re-initialised by the app): left alone"
    fi
    IT_ON=0
    it_end
}

# ---- IT66021 (bus 3, 0x49): bank register 0x0F[1:0] ----
ir_r() { i2cget -y 3 0x49 "$1" 2> /dev/null; }
ir_w() { i2cset -y 3 0x49 "$1" "$2" 2> /dev/null; }
ir_begin() {
    app_pause || return 1
    [ "$IR_WIN" = 1 ] && return 0
    ir1=$(ir_r 0x0f)
    ir2=$(ir_r 0x0f)
    if is_hex "$ir1" && [ "$ir1" = "$ir2" ]; then
        IR_BANK=$((ir1))
        IR_WIN=1
        return 0
    fi
    app_resume
    return 1
}
ir_bank() { ir_w 0x0f "$(hex2 $(((IR_BANK & ~3) | $1)))"; }
ir_end() {
    [ "$IR_WIN" = 1 ] && ir_w 0x0f "$(hex2 "$IR_BANK")"
    IR_WIN=0
    app_resume
}

# ---- PH1-PH3 pin function (PH_CFG0): the strict rule of src/record/hdmi_boot.h ----
# The whole register must be one of exactly two values: PH_STOCK (as rc.sh leaves it: PH0 out,
# PH1 uart4, PH2/3 off, PH4 out, PH5/6 uart3 = ELRS backpack, PH7 uart0) or PH_AUDIO (only
# PH1-PH3 = function 3, I2S0 BCLK / LRCK / DOUT). Only these two constants are ever written,
# never a value built from a read; a write that does not read back is undone (stock value
# written and verified). PH_AUDIO found already = HDMI Out Audio is in effect for this boot
# (hdmi_audio_boot switched it): used as it is, and not undone at the end.
PH=0x0300B0FC
PH_STOCK=0x35517751
PH_AUDIO=0x35513331
CTL=0x05090000
CTL_MASK=0x60030 # BCLK_OUT, LRCK_OUT, MODE_SEL
ctl_sane() { [ $(($1 & 0xfff9fe80)) -eq 0 ] && [ $((($1 >> 4) & 3)) -ne 3 ]; } # reserved bits 0
ph_set() {
    lat_begin || return 1
    if ! lat_read $PH; then
        log "PH_CFG0 read unstable: pin mux left alone"
    elif [ "$RV" -eq $((PH_AUDIO)) ]; then
        log "PH_CFG0 $(hex "$RV"): PH1-PH3 are I2S0 already (HDMI Out Audio in effect this boot): not written, not undone"
    elif [ "$RV" -ne $((PH_STOCK)) ]; then
        log "PH_CFG0 $(hex "$RV") is neither $PH_STOCK (rc.sh) nor $PH_AUDIO: refused, pin mux left alone"
    else
        PH_SET=$PH_AUDIO # set first: a signal from here on puts the stock value back
        if lat_write $PH $PH_AUDIO; then
            log "PH_CFG0 $PH_STOCK -> $PH_AUDIO"
        else
            log "PH_CFG0 write did not read back as $PH_AUDIO: writing $PH_STOCK back"
            if lat_write $PH $PH_STOCK; then log "PH_CFG0 restored: $PH_STOCK"; else log "PH_CFG0 restore did not read back: power-cycle the goggle"; fi
            PH_SET=""
        fi
    fi
    lat_end
}
ph_restore() {
    [ -n "$PH_SET" ] || return 0
    if ! lat_begin; then
        log "PH_CFG0 NOT restored (no window): power-cycle the goggle"
        return 1
    fi
    if ! lat_read $PH; then
        log "PH_CFG0 read not trusted: NOT restored, power-cycle the goggle"
    elif [ "$RV" -eq $((PH_AUDIO)) ]; then
        if lat_write $PH $PH_STOCK; then log "PH_CFG0 restored: $PH_STOCK"; else log "PH_CFG0 restore did not read back: power-cycle the goggle"; fi
    elif [ "$RV" -eq $((PH_STOCK)) ]; then
        log "PH_CFG0 already back: $PH_STOCK"
    else
        log "PH_CFG0 is $(hex "$RV"), neither value: changed by someone else, left alone (power-cycle the goggle)"
    fi
    PH_SET=""
    lat_end
}
ctl_set() { # I2S mode + clock master, after aplay's hw_params (vendor note i2s-daudio-setup.txt)
    lat_begin || return 1
    if lat_read $CTL && ctl_sane "$RV"; then
        CTL_ORIG=$RV
        want=$(((RV & ~0x30) | 0x60010))
        if [ "$want" -ne "$RV" ]; then
            CTL_SET=$want
            lat_write $CTL "$want" || log "I2S0 CTL write did not read back"
        fi
        log "I2S0 CTL $(hex "$CTL_ORIG") -> $(hex "$want")"
    else
        log "I2S0 CTL read not trusted ($(hex "${RV:-0}")): left alone"
    fi
    lat_end
}
ctl_restore() {
    [ -n "$CTL_SET" ] || return 0
    if ! lat_begin; then
        log "I2S0 CTL NOT restored (no window)"
        return 1
    fi
    if lat_read $CTL && ctl_sane "$RV"; then
        if [ $((RV & CTL_MASK)) -eq $((CTL_SET & CTL_MASK)) ]; then
            back=$(((RV & ~CTL_MASK) | (CTL_ORIG & CTL_MASK)))
            if lat_write $CTL "$back"; then log "I2S0 CTL restored: $(hex "$back")"; else log "I2S0 CTL restore did not read back"; fi
        else
            log "I2S0 CTL is $(hex "$RV"), fields changed by someone else (driver): left alone"
        fi
    else
        log "I2S0 CTL read not trusted: NOT restored (harmless once PH1-PH3 are back)"
    fi
    CTL_SET=""
    lat_end
}
soc_log() { # $1 label, then address list: one read each, app paused
    sl=$1
    shift
    lat_begin || return 0
    sl_s=""
    for sl_a in "$@"; do
        lat_peek "$sl_a"
        sl_s="$sl_s $sl_a=$RV"
    done
    lat_end
    log "$sl:$sl_s"
}

undo_window() { # app still paused if a window was open: restore inside it
    [ "$IT_WIN" = 1 ] && it_undo
    [ "$IR_WIN" = 1 ] && ir_end
    return 0
}
undo() {
    it_undo
    ctl_restore
    ph_restore
    [ -n "$TONE_FILE" ] && rm -f "$TONE_FILE" "$TONE_FILE.2"
    return 0
}

# run "$@" in the background for at most $1 s (killed by the trap if interrupted)
run_for() {
    rf_t=$1
    shift
    "$@" >> "$OUT" 2>&1 < /dev/null &
    rf_p=$!
    BGP="$BGP $rf_p"
    RF_PID=$rf_p
    RF_T=$rf_t
}
run_wait() {
    rf_s=0
    while [ "$rf_s" -lt "$RF_T" ] && alive "$RF_PID"; do
        sleep 1
        rf_s=$((rf_s + 1))
    done
    if alive "$RF_PID"; then
        kill "$RF_PID" 2> /dev/null
        log "(stopped after ${RF_T}s)"
    fi
    wait "$RF_PID" 2> /dev/null
    bg_forget "$RF_PID"
}

part_out() {
    log "==== part A: HDMI OUT audio ===="
    if ! have aplay || ! have i2cget || ! have i2cset; then
        log "aplay / i2c-tools missing"
        return
    fi
    it_begin || return
    id="$(it_r 0x00) $(it_r 0x01) $(it_r 0x02) $(it_r 0x03)"
    it_end
    log "IT66121 id regs: $id (expect 0x54 0x49 0x12 0x?6)"
    case "$id" in 0x54*) ;; *)
        log "IT66121 not answering: HDMI out is off (source HDMI in?) - part A skipped"
        return
        ;;
    esac

    it_begin || return
    if ! it_save; then
        log "IT66121 registers did not read back the same twice: part A skipped, nothing written"
    elif ! it04_ok "$(saved 0x04)"; then
        log "IT66121 Reg04 = $(saved 0x04): a video/HDCP/RCLK reset or reserved bit is set (expected 0x14 or 0x00): part A skipped, nothing written"
    else
        IT_ON=1
        it_audio_on
        it_status
    fi
    it_end
    [ "$IT_ON" = 1 ] || return

    # PH1-PH3 -> function 3 (I2S0 BCLK/LRCK/DOUT); rc.sh leaves PH1 = uart4, PH2/PH3 = off
    ph_set

    # 15 s of tone: 192-byte period doubled 14 times = 16.4 s
    TONE_FILE="$TMPD/hdmi_tone.raw"
    if [ -r "$TONE_SRC" ]; then
        cp "$TONE_SRC" "$TONE_FILE"
        k=0
        while [ "$k" -lt 14 ]; do
            cat "$TONE_FILE" "$TONE_FILE" > "$TONE_FILE.2" && mv "$TONE_FILE.2" "$TONE_FILE"
            k=$((k + 1))
        done
    else
        log "no $TONE_SRC: using noise from /dev/urandom (LOUD - turn the TV down)"
        dd if=/dev/urandom of="$TONE_FILE" bs=192000 count=16 2> /dev/null
    fi

    log "playing tone on hw:snddaudio0 for ${TONE_S}s - listen to the TV"
    run_for "$TONE_S" aplay -D hw:snddaudio0 -t raw -f S16_LE -c 2 -r 48000 "$TONE_FILE"
    nap 1
    ctl_set
    nap 2
    soc_log "during playback: I2S0 CTL TXCNT FSTA" 0x05090000 0x05090028 0x05090018
    it_begin && it_status && it_end
    nap 3
    soc_log "I2S0 TXCNT (grows 48000/s while playing)" 0x05090028
    it_begin && it_status && it_end
    run_wait
    rm -f "$TONE_FILE" "$TONE_FILE.2"
    TONE_FILE=""

    it_undo
    ctl_restore
    ph_restore
}

part_in() {
    log "==== part B: HDMI IN audio ===="
    if ! have arecord || ! have i2cdump; then
        log "arecord / i2c-tools missing"
        return
    fi
    sig="(IT66021 not answering)"
    if ir_begin; then
        ir_bank 0
        sig=$(ir_r 0x99)
        ir_end
    fi
    log "IT66021 reg 0x99=$sig (bit3 = video signal detected, used by IT66021_Sig_det)"
    soc_log "PG_CFG1 (PG8-15) I2S2_CTL RXCNT" 0x0300B0DC 0x05092000 0x0509202c
    w="$OUTDIR/hdmi_in_test_$N.wav"
    log "recording ${REC_S}s from hw:snddaudio2 into $(basename "$w")"
    run_for $((REC_S + 5)) arecord -D hw:snddaudio2 -t wav -f S16_LE -c 2 -r 48000 -d "$REC_S" "$w"
    nap 3
    soc_log "during capture: I2S2 CTL RXCNT FSTA" 0x05092000 0x0509202c 0x05092018
    nap 3
    soc_log "I2S2 RXCNT (grows only if the IT66021 drives SCK/WS)" 0x0509202c
    run_wait
    if [ -f "$w" ]; then
        log "wav size: $(wc -c < "$w") bytes (44 = header only, no samples)"
    else
        log "no wav written"
    fi
    for b in 0 1 2; do
        ir_begin || break
        ir_bank "$b"
        log "--- IT66021 bank $b"
        i2cdump -y 3 0x49 b >> "$OUT" 2>&1
        ir_end
    done
}

case "$WHAT" in out | in | both) ;; *)
    echo "usage: sh hdmi_audio_test.sh [out|in|both]"
    exit 2
    ;;
esac
if [ "$MODE" = sd ]; then
    guard_start $((W1 + W2 + TONE_S + REC_S + 300))
    start_app_once # SD-card mode: the goggle app first, before anything else can fail
else
    guard_start 3600
fi
out_open hdmi_audio_test
log "hdmi_audio_test ($WHAT), mode $MODE, app pid '$(app_pid)'"

if [ "$WHAT" != in ]; then
    step "part A (HDMI out): HDMI display connected, source is NOT HDMI in" "$W1" 1
    part_out
    sync
    beep 2
fi
if [ "$WHAT" != out ]; then
    step "part B (HDMI in): select HDMI in now, with a source that plays sound" "$W2" 3
    part_in
    sync
fi
beep 4
log "done"
exit 0
