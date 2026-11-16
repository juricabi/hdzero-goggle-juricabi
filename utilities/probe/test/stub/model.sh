# Sourced by the stub tools of test/run_tests.sh (host only, never on a goggle).
# A model of what the probe scripts touch on a goggle:
#  - /sys/class/sunxi_dump: ONE address latch for every process. "dump" holds the address
#    last written; reading it returns that register; "write" takes "addr value". The script
#    side is two plain files; this model gives them their meaning when the stub "cat" reads
#    "dump" (a pending "write" is applied first, so every write is seen before the next read).
#  - IT66121 (bus 1, 0x4c, bank = 0x0F bit 0) and IT66021 (bus 3, 0x49, bank = 0x0F bits 1:0).
#  - the goggle app: pids in $MODEL/app.pids; "running" = alive and not stopped.
# Violations (latch or ITE access while the app runs, forbidden commands) go to
# $MODEL/violations; every access is logged in $MODEL.
M=${MODEL:?}
R=${PROBE_ROOT:?}

st_of() { # state letter of pid $1, empty if gone
    s=""
    { read -r s < "/proc/$1/stat"; } 2> /dev/null || return 0
    s=${s##*) }
    echo "${s%% *}"
}
app_unstopped() { # 0 if some goggle app process runs (is not stopped)
    [ -f "$M/app.pids" ] || return 1
    while read -r p; do
        s=$(st_of "$p")
        case "$s" in "" | Z | T) ;; *) return 0 ;; esac
    done < "$M/app.pids"
    return 1
}
viol() { echo "$*" >> "$M/violations"; }
n32() { printf '0x%08x' "$(($1))"; }
n8() { printf '0x%02x' "$(($1))"; }

# key/value stores: one "key value" per line
kv_get() { # $1 file, $2 key, $3 default
    v=$(awk -v k="$2" '$1 == k { v = $2 } END { print v }' "$1" 2> /dev/null)
    echo "${v:-$3}"
}
kv_set() {
    awk -v k="$2" -v v="$3" 'BEGIN { done = 0 } $1 == k { print k, v; done = 1; next } { print } END { if (!done) print k, v }' "$1" 2> /dev/null > "$1.new"
    mv "$1.new" "$1"
}
counter() { # increment and print counter $1
    c=$(kv_get "$M/counters" "$1" 0)
    c=$((c + 1))
    kv_set "$M/counters" "$1" "$c"
    echo "$c"
}

# sunxi_dump: apply a pending "write" (the kernel does it at once; the model at the next read)
soc_apply_write() {
    W="$R/sys/class/sunxi_dump/write"
    [ -s "$W" ] || return 0
    read -r wa wv < "$W"
    : > "$W"
    wa=$(n32 "$wa")
    wv=$(n32 "$wv")
    if app_unstopped; then
        viol "sunxi_dump write $wa $wv while the goggle app runs"
        echo "W $wa $wv app-running" >> "$M/soc_writes"
    else
        echo "W $wa $wv" >> "$M/soc_writes"
    fi
    kv_set "$M/soc" "$wa" "$wv"
}

# the GPIO driver changes the PG0..PG2 function when the direction is set (sysfs gpio)
soc_gpio_mux() {
    v=$(kv_get "$M/soc" 0x0300b0d8 0x77777777)
    for p in 192 193 194; do
        d=$(busybox cat "$R/sys/class/gpio/gpio$p/direction" 2> /dev/null)
        last=$(kv_get "$M/gpiodir" "$p" "")
        [ "$d" = "$last" ] && continue
        kv_set "$M/gpiodir" "$p" "$d"
        sh=$(((p - 192) * 4))
        case "$d" in
            out | low | high) v=$(((v & ~(7 << sh)) | (1 << sh))) ;;
            in) v=$((v & ~(7 << sh))) ;;
        esac
    done
    kv_set "$M/soc" 0x0300b0d8 "$(n32 "$v")"
}

i2c_file() { echo "$M/i2c_$1_$2"; }
i2c_key() { # $1 bus $2 addr $3 reg -> store key (bank-independent: 0x0F; IT66121 below 0x30)
    f=$(i2c_file "$1" "$2")
    if [ "$3" = 0x0f ]; then
        echo g_0x0f
        return
    fi
    b=$(kv_get "$f" g_0x0f 0x00)
    case "$2" in
        0x4c)
            if [ $(($3)) -lt $((0x30)) ]; then echo "g_$3"; else echo "b$((b & 1))_$3"; fi
            ;;
        0x49) echo "b$((b & 3))_$3" ;;
        *) echo "g_$3" ;;
    esac
}
i2c_absent() { case " ${STUB_I2C_ABSENT:-} " in *" $1:$2 "*) return 0 ;; esac; return 1; }
app_stopped() { # 0 if some goggle app process is stopped
    [ -f "$M/app.pids" ] || return 1
    while read -r p; do
        [ "$(st_of "$p")" = T ] && return 0
    done < "$M/app.pids"
    return 1
}
# for tools the scripts run OUTSIDE register windows: the app must not be stopped then
outside_window() { app_stopped && viol "goggle app stopped while '$*' runs (outside a register window)"; }
