#!/bin/sh
# Host check of the IT66121 driver (src/driver/it66121.c) with stub I2C:
#  - HDMI out audio not enabled: IT66121_init()/set_phase()/close() byte-for-byte as the base (ripples-base);
#  - enabled: the audio register sequence, and Reg04 (which also holds the video, HDCP and
#    RCLK resets) only ever written with the audio-reset bits changed: no write of the audio
#    path can set a video reset bit, also when an I2C read fails (each read in turn), returns
#    garbage (every value of Reg04, two reads that differ, 3000 random runs) or a Reg04 write
#    does not read back. Then: the stock video-on value 0x14 written back, audio off for the
#    rest of the boot, no audio source enabled, and the next IT66121_init() is the base's again.
#    A Reg04 that does not read as the stock init leaves it (any of bits 5, 4, 3, 2, 0 other),
#    or two reads of a check that differ: no Reg04 write but the stock 0x14.
# SRC_TOP=<tree>: test another copy of the sources (test/c_mutants.sh uses it).
# The base files come from git (this tree must be a git worktree that has the base commit)
# or from BASE_SRC=<a tree of the base commit>; without either the test is skipped (exit 77).
# Every test program runs under a time limit (TEST_TIMEOUT, default 300 s).
# Usage: sh utilities/probe/test/it66121_seq_test.sh [base-commit]   (default 4fbb865, ripples-base: the firmware this branch is built on)
set -eu
BASE=${1:-4fbb865}
TL=${TEST_TIMEOUT:-300}
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=${SRC_TOP:-$TOP}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/inc/log" "$T/inc/driver"
for d in orig new; do
    mkdir -p "$T/$d/driver" "$T/$d/core"
    echo '#include "stub_i2c.h"' > "$T/$d/driver/i2c.h"
    printf '#pragma once\nvoid DM5680_ResetHDMI_TX(int);\n' > "$T/$d/driver/dm5680.h"
    echo '#pragma once' > "$T/$d/core/common.hh"
done
echo '#pragma once' > "$T/inc/driver/hardware.h"
printf '#pragma once\n#include <stdio.h>\n#define LOGI(...) do { fprintf(stderr, __VA_ARGS__); fputc(10, stderr); } while (0)\n#define LOGE LOGI\n' > "$T/inc/log/log.h"
cat > "$T/inc/stub_i2c.h" << 'H'
#pragma once
#include <stdint.h>
uint8_t stub_read(int port, uint8_t s, uint8_t a);
int stub_read_checked(int port, uint8_t s, uint8_t a, uint8_t *v);
void stub_write(int port, uint8_t s, uint8_t a, uint8_t d);
#define ADDR_IT66121 0x4C
#define ADDR_FPGA 0x64
#define I2C_Write(s, a, d) stub_write(2, s, a, d)
#define I2C_Read(s, a) stub_read(2, s, a)
#define I2C_R_Write(s, a, d) stub_write(1, s, a, d)
#define I2C_R_Read(s, a) stub_read(1, s, a)
#define I2C_R_Read_checked(s, a, v) stub_read_checked(1, s, a, v)
H
cat > "$T/harness.c" << 'C'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "stub_i2c.h"
static uint8_t regs[2][256];
static int quiet;
/* fault injection (checked reads, i.e. the audio path's reads) */
static int n_checked, fail_at = -1, garbage04 = -1, disagree04, rnd, stuck04, injected;
static int phase_audio;            // the stock init's last write (C6=03) was seen
static int r04_writes, bad_r04, e0_on, last_r04 = -1;
static int r04_other;              // Reg04 writes of the audio path other than the stock 0x14
static int bank(void) { return regs[0][0x0f] & 1; }
uint8_t stub_read(int port, uint8_t s, uint8_t a) {
    uint8_t v = (port == 1 && s == ADDR_IT66121) ? ((a < 0x30) ? regs[0][a] : regs[bank()][a]) : 0;
    if (!quiet) printf("R %d %02x %02x -> %02x\n", port, s, a, v);
    return v;
}
int stub_read_checked(int port, uint8_t s, uint8_t a, uint8_t *v) {
    n_checked++;
    if (n_checked == fail_at) { injected = 1; if (!quiet) printf("R %d %02x %02x -> FAIL\n", port, s, a); return -1; }
    uint8_t x = stub_read(port, s, a);
    if (a == 0x04 && garbage04 >= 0) { x = (uint8_t)garbage04; injected = 1; }
    // two reads that differ: 1 = the first of each pair another value, 2 = the second
    if (a == 0x04 && disagree04 && (n_checked & 1) == (disagree04 == 1)) { x ^= 0x08; injected = 1; }
    if (rnd) { injected = 1; x = (uint8_t)(rand() & 0xff); if (rand() % 8 == 0) return -1; }
    *v = x;
    return 0;
}
void stub_write(int port, uint8_t s, uint8_t a, uint8_t d) {
    if (!quiet) printf("W %d %02x %02x %02x\n", port, s, a, d);
    if (port == 1 && s == ADDR_IT66121) {
        if (phase_audio && a == 0x04) {
            r04_writes++; last_r04 = d;
            if (d & ~0x14) bad_r04++; // anything but the two audio-reset bits: a video / HDCP / RCLK reset or a reserved bit
            if (d != 0x14) r04_other++;
        }
        if (phase_audio && a == 0xe0 && (d & 0x0f)) e0_on = 1;
        if (phase_audio && a == 0x04 && stuck04) return;
        if (a < 0x30) regs[0][a] = regs[1][a] = d; else regs[bank()][a] = d;
        if (a == 0xc6 && d == 0x03) phase_audio = 1;
    }
}
void DM5680_ResetHDMI_TX(int v) { if (!quiet) printf("RESET %d\n", v); }
int usleep(unsigned u) { if (!quiet) printf("SLEEP %u\n", u); return 0; }
void IT66121_init(); void IT66121_close(); void IT66121_set_phase(uint8_t, uint8_t);
#ifdef NEWAPI
#include "it66121.h"
#endif
static void chip_reset(void) {
    memset(regs, 0, sizeof(regs));
    regs[0][0x02] = 0x12; regs[0][0xc5] = 0x04; regs[0][0x5f] = 0xe0; regs[0][0xe0] = 0xc0;
}
#ifdef NEWAPI
static int fails;
/* one injected run in a child (the driver's state is static): the invariants */
static int run_injected(const char *what, int f_at, int g04, int dis, int seed, int stuck) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        quiet = 1; fail_at = f_at; garbage04 = g04; disagree04 = dis; stuck04 = stuck;
        n_checked = 0; injected = 0; phase_audio = 0; r04_writes = 0; bad_r04 = 0; e0_on = 0; last_r04 = -1; r04_other = 0;
        if (seed) { srand((unsigned)seed); rnd = 1; }
        chip_reset();
        IT66121_audio_config(true);
        IT66121_init();
        int ok = bad_r04 == 0;
        // Reg04 never in the state the stock init leaves (its defined bits 0x3d other than 0x14), or
        // the two reads of a check differ: the precondition fails, so nothing but the stock 0x14
        if ((g04 >= 0 && (g04 & 0x3d) != 0x14) || dis)
            ok = ok && r04_other == 0;
        if (IT66121_audio_failed())
            ok = ok && !e0_on && last_r04 == 0x14 && !IT66121_audio_is_on() && (stuck || rnd || regs[0][0x04] == 0x14) && regs[0][0xe0] == 0xc0;
        else
            ok = ok && IT66121_audio_is_on() && e0_on && (rnd || !injected);
        // a failure keeps audio off for this boot: the next init writes no audio register
        int was_failed = IT66121_audio_failed();
        phase_audio = 0; r04_writes = 0; e0_on = 0; fail_at = -1; garbage04 = -1; disagree04 = 0; rnd = 0; stuck04 = 0;
        IT66121_init();
        if (was_failed)
            ok = ok && r04_writes == 0 && !e0_on && !IT66121_audio_is_on();
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
    return ok;
}
#endif
int main(int argc, char **argv) {
    chip_reset();
#ifdef NEWAPI
    if (argc > 1 && strcmp(argv[1], "inject") == 0) {
        char w[96]; int n = 0, healthy = 0;
        // a healthy chip first: how many checked reads the audio path makes
        n_checked = 0; quiet = 1; IT66121_audio_config(true); IT66121_init();
        const int reads = n_checked;
        if (!IT66121_audio_is_on() || bad_r04 || !e0_on) { printf("FAIL healthy chip: audio not on\n"); return 1; }
        for (int i = 1; i <= reads + 4; i++) { snprintf(w, sizeof(w), "read %d failing", i); run_injected(w, i, -1, 0, 0, 0); n++; }
        for (int g = 0; g < 256; g++) { snprintf(w, sizeof(w), "Reg04 reads 0x%02x", g); run_injected(w, -1, g, 0, 0, 0); n++; }
        run_injected("two Reg04 reads that differ (the first another value)", -1, -1, 1, 0, 0); n++;
        run_injected("two Reg04 reads that differ (the second another value)", -1, -1, 2, 0, 0); n++;
        run_injected("Reg04 write not taken", -1, -1, 0, 0, 1); n++;
        for (int s = 1; s <= 3000; s++) { snprintf(w, sizeof(w), "random reads, seed %d", s); run_injected(w, -1, -1, 0, s, 0); n++; }
        (void)healthy;
        printf("%s %d injected runs (%d checked reads in a healthy run): no Reg04 write set a video/HDCP/RCLK reset bit; every failure: 0x14 written back, no audio source enabled, audio off for the boot, no audio register at the next init\n",
               fails ? "FAIL" : "PASS", n, reads);
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "twice") == 0) { // a failed first init (Reg04 unreadable), then the next init
        quiet = 1; fail_at = 1; IT66121_audio_config(true); IT66121_init();
        fail_at = -1;
        if (!IT66121_audio_failed()) { printf("NOT FAILED\n"); return 1; }
        chip_reset(); quiet = 0;
        IT66121_init(); IT66121_set_phase(1, 1); IT66121_close();
        return 0;
    }
    if (argc > 1) IT66121_audio_config(true);
#endif
    IT66121_init();
    IT66121_set_phase(1, 1);
    IT66121_close();
    return 0;
}
C
base_file() { # $1 path in the tree, $2 where: from BASE_SRC, or from git
    if [ -n "${BASE_SRC:-}" ]; then
        cp "$BASE_SRC/$1" "$2" 2> /dev/null
    else
        git -C "$TOP" show "$BASE:$1" > "$2" 2> /dev/null
    fi
}
if ! base_file src/driver/it66121.c "$T/orig/driver/it66121.c" || ! base_file src/driver/it66121.h "$T/orig/driver/it66121.h"; then
    echo "SKIP it66121_seq_test.sh: the base $BASE files are not available (${BASE_SRC:-$TOP is not a git worktree with $BASE}); set BASE_SRC=<a tree of $BASE>"
    exit 77
fi
[ -n "${BASE_SRC:-}" ] && BASE="$BASE (BASE_SRC)"
cp "$SRC/src/driver/it66121.c" "$SRC/src/driver/it66121.h" "$T/new/driver/"
rc=0
for v in HDZGOGGLE HDZGOGGLE2 HDZBOXPRO; do
    gcc -w -pthread -D$v -I"$T/inc" -I"$T/orig/driver" -o "$T/o" "$T/harness.c" "$T/orig/driver/it66121.c"
    gcc -Wall -Wno-unused-function -pthread -D$v -DNEWAPI -I"$T/inc" -I"$T/new/driver" -o "$T/n" "$T/harness.c" "$T/new/driver/it66121.c"
    timeout -k 5 "$TL" "$T/o" > "$T/o.txt"
    timeout -k 5 "$TL" "$T/n" > "$T/n.txt" 2> /dev/null
    if cmp -s "$T/o.txt" "$T/n.txt"; then
        echo "PASS $v: audio off: register sequence identical to $BASE ($(grep -c '^W' "$T/o.txt") writes)"
    else
        echo "FAIL $v: sequence differs"; diff "$T/o.txt" "$T/n.txt" | head; rc=1
    fi
    timeout -k 5 "$TL" "$T/n" twice > "$T/t.txt" 2> /dev/null || { echo "FAIL $v: the injected first init did not fail"; rc=1; }
    if cmp -s "$T/o.txt" "$T/t.txt"; then
        echo "PASS $v: after a failed audio check the next IT66121_init() is identical to $BASE again"
    else
        echo "FAIL $v: the init after a failed audio check differs"; diff "$T/o.txt" "$T/t.txt" | head; rc=1
    fi
    irc=0
    timeout -k 5 "$TL" "$T/n" inject > "$T/i.txt" 2> /dev/null || irc=$?
    [ "$irc" = 0 ] || rc=1
    [ "$irc" = 124 ] && echo "FAIL $v: the injected runs did not end within $TL s"
    grep '^FAIL' "$T/i.txt" | head -5
    tail -1 "$T/i.txt" | sed "s/^\(PASS\|FAIL\) /\1 $v: /"
    # audio on (HDMI Out Audio in effect): what it adds, and Reg04 only ever as two constants
    timeout -k 5 "$TL" "$T/n" on > "$T/a.txt" 2> /dev/null
    echo "$v: audio on adds $(($(grep -c '^W' "$T/a.txt") - $(grep -c '^W' "$T/n.txt"))) writes after IT66121_init():"
    diff "$T/n.txt" "$T/a.txt" | sed -n 's/^> W 1 4c /  /p' | tr '\n' ' '
    echo
    r04=$(diff "$T/n.txt" "$T/a.txt" | sed -n 's/^> W 1 4c 04 //p' | tr '\n' ' ')
    if [ "$r04" = "04 00 " ]; then echo "PASS $v: audio on: Reg04 written only as 04, 00 (audio resets released, video/HDCP/RCLK as stock)"; else echo "FAIL $v: audio on: Reg04 writes '$r04'"; rc=1; fi
done
exit $rc
