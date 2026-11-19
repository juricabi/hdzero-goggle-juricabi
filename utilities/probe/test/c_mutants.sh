#!/bin/bash
# Mutation check of the host tests of the C safety code: the HDMI audio boot step and its record
# (src/record/hdmi_boot.c), the shared sunxi_dump access (src/record/soc_reg.c,
# system_exec_sunxi_dump() in src/util/system.c), the IT66121 Reg04 guard (src/driver/it66121.c),
# the IT66021 EDID (src/driver/it66021.c) and the record daemon's bridge (src/record/pcmbridge.c).
# (On ripples-all the module bay's mutants, src/module/module.c, are all in
# utilities/module_bay_test/mutants.sh.) Each mutant is a copy of src/ with one safety property
# broken (a perl substitution); the test that covers it runs against the copy
# (SRC_TOP=<copy> for utilities/probe/test/*.sh) and must FAIL. A test that hangs is stopped (TEST_TIMEOUT) and counts
# as a failure. Mutants marked EQUIV cannot change any outcome (the reason is given); they are run
# and reported but do not count.
#   utilities/probe/test/c_mutants.sh [name filter]     (~20 minutes; from the git worktree, or
#   with BASE_SRC=<a tree of ripples-base 4fbb865> for the IT66121 / IT66021 tests)
# The tests only touch files of their own (stand-ins for sysfs, temporary directories); this
# script needs no root and refuses none.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
ONLY=${1:-}
W=$(mktemp -d /tmp/c_mutants.XXXXXX)
trap 'rm -rf "$W"' EXIT
export TEST_TIMEOUT=${TEST_TIMEOUT:-240} # each test program (a hang = a failure)
n=0
bad=0
killed=0
equiv=0

runtest() { # $1 test, $2 tree (with src/), $3 log
    SRC_TOP="$2" timeout -k 5 900 sh "$HERE/$1" > "$3" 2>&1
}

# the unmodified sources must pass every test first
mkdir -p "$W/m0"
cp -r "$TOP/src" "$W/m0/"
for t in hdmi_boot_test.sh it66121_seq_test.sh it66021_edid_test.sh pcmbridge_test.sh; do
    runtest $t "$W/m0" "$W/m0/$t.log"
    r=$?
    if [ $r != 0 ]; then
        echo "the unmodified sources fail $t (exit $r):"
        tail -15 "$W/m0/$t.log"
        exit 1
    fi
done
echo "unmodified: every test passes"

mut() { # [EQUIV:<why>] name file test perl-expression
    local why=""
    case $1 in EQUIV:*) why=${1#EQUIV:}; shift ;; esac
    [ -n "$ONLY" ] && case $1 in *"$ONLY"*) ;; *) return ;; esac
    n=$((n + 1))
    local d=$W/m$n
    mkdir -p "$d"
    cp -r "$TOP/src" "$d/"
    perl -0pi -e "$4" "$d/src/$2"
    if cmp -s "$d/src/$2" "$TOP/src/$2"; then
        echo "NOT APPLIED $1"
        bad=1
        return
    fi
    runtest "$3" "$d" "$d/log"
    local rc=$?
    local first
    first=$(grep -m1 -E '^FAIL|FAIL$|error:|not end within' "$d/log" | cut -c1-150)
    if [ $rc = 0 ]; then
        if [ -n "$why" ]; then
            echo "EQUIV     $1 [$3]: survives as expected ($why)"
            equiv=$((equiv + 1))
        else
            echo "SURVIVED  $1 [$3]"
            bad=1
        fi
    else
        [ $rc = 124 ] && first="stopped after 900 s"
        echo "KILLED    $1 [$3]: $first"
        killed=$((killed + 1))
    fi
    rm -rf "$d"
}

# HDMI audio boot step (hdmi_boot.c, soc_reg.c, system.c): hdmi_boot_test.sh
mut "ph_state: fields not owned not checked" record/hdmi_boot.c hdmi_boot_test.sh \
    's/    if \(\(v & ~HB_PH_OWNED_MASK\) != \(HB_PH_STOCK & ~HB_PH_OWNED_MASK\)\)\n        return HB_PINS_OTHER;.*?\n//'
mut "soc_reg_read: two of three reads" record/soc_reg.c hdmi_boot_test.sh \
    's/if \(v\[0\] != v\[1\] \|\| v\[1\] != v\[2\]\)/if (v[0] != v[1])/'
mut "soc_reg_read1: no newline check" record/soc_reg.c hdmi_boot_test.sh \
    "s/ \|\| buf\[n - 1\] != '\\\\n'\)/)/"
mut "boot step: audio_out ignored" record/hdmi_boot.c hdmi_boot_test.sh \
    's/    if \(!b.out\) \{\n        hb_detail\(&b, "audio_out is not 1/    if (0) {\n        hb_detail(&b, "audio_out is not 1/'
mut "recover: stock never written back" record/hdmi_boot.c hdmi_boot_test.sh \
    's/        \(void\)soc_reg_put\(HB_PH_CFG0, HB_PH_STOCK\);\n        HB_CHECKPOINT\(7\);/        HB_CHECKPOINT(7);/'
mut "app start: PH_CFG0 not re-read" record/hdmi_boot.c hdmi_boot_test.sh \
    's/if \(r == 0 && v == HB_PH_AUDIO\) \{\n        \*out_active = true;/if (1) {\n        *out_active = true;/'
mut "switch: no read-back verify" record/hdmi_boot.c hdmi_boot_test.sh \
    's/if \(soc_reg_read\(HB_PH_CFG0, &v, diag, sizeof\(diag\)\) == 0 && v == HB_PH_AUDIO\) \{/if (1) {/'
mut "switch: lock not required" record/hdmi_boot.c hdmi_boot_test.sh \
    's/    const int lk = soc_reg_lock\(\);\n    if \(lk < 0\) \{\n        hb_detail\(b, "sunxi_dump lock not available within 1 s: PH_CFG0 not read, not written"\);\n        return;\n    \}/    const int lk = -1;/'
mut "system: lock leaked (no unlock)" util/system.c hdmi_boot_test.sh \
    's/    soc_reg_unlock\(fd\);\n    return r;/    return r;/'
mut "system: always waits 1 s" util/system.c hdmi_boot_test.sh \
    's/soc_reg_lock_n\(recent \? 1 : SOC_REG_LOCK_TRIES\)/soc_reg_lock_n(SOC_REG_LOCK_TRIES)/'
mut "system: command not run when lock busy" util/system.c hdmi_boot_test.sh \
    's/    const int r = system\(command\);/    const int r = fd < 0 ? -1 : system(command);/'
mut "supported: BoxPro left out again" record/hdmi_boot.c hdmi_boot_test.sh \
    's/defined\(HDZGOGGLE2\) \|\| defined\(HDZBOXPRO\)/defined(HDZGOGGLE2)/'
mut "supported: any target" record/hdmi_boot.c hdmi_boot_test.sh \
    's/    return false; \/\/ a target whose files were not compared/    return true; \/\/ a target whose files were not compared/'

# IT66121 Reg04 guard (it66121.c): it66121_seq_test.sh
mut "EQUIV:the write uses the value of a check that just required bits 5, 3, 0 to be 0" \
    "it66121: Reg04 read-modify-write again" driver/it66121.c it66121_seq_test.sh \
    's/    I2C_R_Write\(ADDR_IT66121, 0x04, to\);/    I2C_R_Write(ADDR_IT66121, 0x04, (uint8_t)(to | (seen > 0 ? (seen & 0x29) : 0)));/'
mut "it66121: abort not called" driver/it66121.c it66121_seq_test.sh \
    's/            it66121_audio_abort\(\);\n//'
mut "it66121: Reg04 only audio bits compared" driver/it66121.c it66121_seq_test.sh \
    's/#define IT66121_R04_DEFINED    0x3d/#define IT66121_R04_DEFINED    0x14/'
mut "it66121: one Reg04 read" driver/it66121.c it66121_seq_test.sh \
    's/ \|\| I2C_R_Read_checked\(ADDR_IT66121, 0x04, &b\) < 0 \|\| a != b\)/)/'
mut "it66121: failure not sticky" driver/it66121.c it66121_seq_test.sh \
    's/            it66121_audio_fail = true; \/\/ off/            \/\/ off/'

# IT66021 EDID (it66021.c): it66021_edid_test.sh
mut "it66021: block 1 checksum not adjusted" driver/it66021.c it66021_edid_test.sh \
    's/const uint8_t sum1 = \(uint8_t\)\(edid\[0xff\] \+ \(edid\[0x83\] - cea3\) \+ \(edid\[0x8b\] - sad_rates\)\);/const uint8_t sum1 = edid[0xff];/'

# the record daemon's bridge (pcmbridge.c): pcmbridge_test.sh
mut "pcmbridge: boot record not checked" record/pcmbridge.c pcmbridge_test.sh \
    's/ &&\n        !hdmi_boot_out_ready\(\)\)/ \&\& 0)/'
mut "pcmbridge: I2S0 apply failure ignored" record/pcmbridge.c pcmbridge_test.sh \
    's/            if \(why != NULL\)\n                return pb_fail\(pb, aiDevFree, why\);/            (void)why;/'

# (the module bay's register read and status, module.c: utilities/module_bay_test/mutants.sh)

echo "$n mutants: $killed killed, $equiv equivalent (survive by design)$([ $bad = 0 ] || echo ", SOME SURVIVED OR DID NOT APPLY")"
[ $bad = 0 ] && echo "ALL C MUTANTS KILLED" || echo "SOME C MUTANTS SURVIVED"
exit $bad
