#!/bin/sh
# Mutation check of test/run_tests.sh (host only): each mutant is a copy of the probe kit with
# one safety property broken; run_tests.sh must FAIL on every one. Also runs the review's
# mutant of the previous kit (MUT_OLD=<dir with its probe/ copy>), which must fail too.
# Usage: sh utilities/probe/test/mutants.sh   (~30 minutes; dash only, few kill points)
# Not as root (run_tests.sh refuses root, and every mutant would then look killed): as root use
# utilities/dev/run_probe.sh mutants.
set -u
if [ "$(id -u)" = 0 ]; then
    echo "mutants.sh: refusing to run as root (run_tests.sh refuses root, so every mutant would look killed)." >&2
    echo "Run it as an unprivileged user, or as root through utilities/dev/run_probe.sh mutants." >&2
    exit 2
fi
HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(cd "$HERE/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0

# name @ script(s) ("all" = the three) @ sed expression @ test groups to run
MUTANTS='
resume-is-a-no-op@all@s/^    \[ -n "\$PAUSED" \] \&\& kill -CONT \$PAUSED 2> \/dev\/null$/    :/@test probe
no-trap-on-signals@all@/^trap .on_signal [A-Z]*. [A-Z]*$/d@test probe bay
start-app-without-running-check@all@/^    \[ -n "\$(app_pid)" \] \&\& return 0$/d@bay
single-latch-read@all@s/is_hex "\$lr_1" \&\& \[ "\$lr_1" = "\$lr_2" \] \&\& \[ "\$lr_2" = "\$lr_3" \] || return 1/is_hex "$lr_1" || return 1/@test
pause-ignores-aww@all@s/\[ "\$pz_ok" = 1 \] \&\& \[ -z "\$(latch_users)" \] \&\& return 0/[ "$pz_ok" = 1 ] \&\& return 0/@test
no-guardian@all@s/^guard_start() {$/guard_start() { return 0/@test probe
ph-write-not-strict@hdmi_audio_test.sh@s/^    elif \[ "\$RV" -ne \$((PH_STOCK)) \]; then$/    elif false; then/@test
ph-found-i2s0-undone@hdmi_audio_test.sh@s/^        log "PH_CFG0 \$(hex "\$RV"): PH1-PH3 are I2S0 already/        PH_SET=$PH_AUDIO; &/@test
it04-unchecked@hdmi_audio_test.sh@s/^it04_ok() {.*$/it04_ok() { true; }/@test
it-restore-skipped@hdmi_audio_test.sh@s/^        it_restore$/        :/@test
probe-devmem-writes@hdmi_audio_probe.sh@s/devmem 0x0300B0FC 32 > /devmem 0x0300B0FC 32 0x35517751 > /@probe
probe-reads-fifos@hdmi_audio_probe.sh@s/^DAUDIO_FIFOS=.*/DAUDIO_FIFOS=""/@probe
probe-bank-before-stop@hdmi_audio_probe.sh@/^ite_begin() {$/,/^}$/s/^    app_pause || return 1$/    i2cget -y "$1" "$2" 0x0f > \/dev\/null 2>\&1; app_pause || return 1/@probe
bay-refuses-on-sdmmc-owner@bay_gpio_pattern.sh@s/^    \[ -r "\${d}pinmux-pins" \] \&\& grep -E .*$/    grep -q sdmmc "${d}pinmux-pins" \&\& refuse "owned by sdmmc"/@bay
bay-no-function-restore@bay_gpio_pattern.sh@s/^        if lat_write \$PG_CFG0 "\$back"; then$/        if true; then/@bay
bay-all-three-outputs@bay_gpio_pattern.sh@s/^    echo in > "\$G\/gpio\$p\/direction" 2> \/dev\/null || refuse/    echo low > "$G\/gpio$p\/direction" 2> \/dev\/null || refuse/@bay
bay-pin-left-output@bay_gpio_pattern.sh@s/^        echo in > "\$G\/gpio\$p\/direction" 2> \/dev\/null$/        :/@bay
start-app-in-ssh-mode@all@s/^    \[ "\$MODE" = sd \] || return 0$/    :/@bay
'

# (printf, not echo: dash's echo turns a backslash and a digit into a control character)
printf '%s\n' "$MUTANTS" | while IFS='@' read -r name files expr groups; do
    [ -n "$name" ] || continue
    d="$T/$name"
    mkdir -p "$d/test"
    cp "$KIT"/*.sh "$KIT"/*.raw "$d/"
    cp -r "$HERE/stub" "$d/test/"
    [ "$files" = all ] && files="hdmi_audio_probe.sh bay_gpio_pattern.sh hdmi_audio_test.sh"
    changed=0
    for f in $files; do
        sed -i "$expr" "$d/$f"
        cmp -s "$d/$f" "$KIT/$f" || changed=1
    done
    if [ "$changed" = 0 ]; then
        echo "BROKEN MUTANT $name: the expression changed nothing"
        echo 1 > "$T/fail"
        continue
    fi
    if KIT_DIR=$d SHELLS=dash QUICK=1 ONLY="$groups" sh "$HERE/run_tests.sh" > "$T/$name.log" 2>&1; then
        echo "SURVIVED $name: run_tests.sh passed"
        echo 1 > "$T/fail"
    else
        echo "KILLED   $name: $(grep '^FAIL' "$T/$name.log" | grep -v 'common safety block' | head -1)"
    fi
done
[ -f "$T/fail" ] && fail=1

if [ -n "${MUT_OLD:-}" ]; then
    mkdir -p "$T/old/test"
    cp "$MUT_OLD"/*.sh "$MUT_OLD"/*.raw "$T/old/"
    cp -r "$HERE/stub" "$T/old/test/"
    if KIT_DIR=$T/old SHELLS=dash QUICK=1 ONLY="test probe" sh "$HERE/run_tests.sh" > "$T/old.log" 2>&1; then
        echo "SURVIVED the review's mutant ($MUT_OLD)"
        fail=1
    else
        echo "KILLED   the review's mutant: $(grep -c '^FAIL' "$T/old.log") failing checks, e.g.:"
        grep '^FAIL' "$T/old.log" | grep -i 'app\|kill' | head -3
    fi
fi
[ "$fail" = 0 ] && echo "ALL MUTANTS KILLED" || echo "SOME MUTANTS SURVIVED"
exit $fail
