#!/bin/sh
# Host check of the EDID the IT66021 (HDMI in) serves to a source. Compiles
# src/driver/it66021.c with stub I2C, runs IT66021_edid() with [hdmi] audio_in off and on,
# and builds the EDID as the chip serves it: EDID RAM (I2C 0x54), except that with
# RegC0[1:0] = 00 (shadow on, as IT66021_edid() leaves it) the block checksums come from
# RegC4 (byte 0x7F) and RegC5 (byte 0xFF) - ITE's IT6602 driver EDIDRAMInitial() /
# UpdateEDIDReg() write only bytes 0..0x7E of each block to RAM and the checksums there.
# (RegC1-C3, the physical-address replacement, are not written by the firmware; assumed
# inactive.) Checks: both block checksums, the CEA-861 extension structure (tag, revision,
# data block collection ending exactly at the DTD offset, DTDs), and the audio claims;
# with audio_in off the I2C sequence and the served EDID equal the base commit's.
# Usage: sh utilities/probe/test/it66021_edid_test.sh [base-commit]   (default 3f0461f, ripples 1.2.2: the firmware this branch is built on)
#   IT66021_SRC=<file>: check another it66021.c instead of the working tree's (SRC_TOP=<tree>:
#   that tree's). The base file comes from git (this tree must be a git worktree that has
#   the base commit) or from BASE_SRC=<a tree of the base commit>; without either: skipped
#   (exit 77). The test programs run under a time limit (TEST_TIMEOUT, default 300 s).
set -eu
BASE=${1:-3f0461f}
TL=${TEST_TIMEOUT:-300}
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=${IT66021_SRC:-${SRC_TOP:-$TOP}/src/driver/it66021.c}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/inc/log"
for d in orig new; do
    mkdir -p "$T/$d/driver" "$T/$d/core"
    echo '#pragma once' > "$T/$d/core/common.hh"
    for h in defines.h hardware.h screen.h; do echo '#pragma once' > "$T/$d/driver/$h"; done
    printf '#pragma once\nvoid DM5680_ResetHDMI_RX(int);\n' > "$T/$d/driver/dm5680.h"
    cat > "$T/$d/driver/i2c.h" << 'H'
#pragma once
#include <stdint.h>
uint8_t stub_read(int port, uint8_t s, uint8_t a);
void stub_write(int port, uint8_t s, uint8_t a, uint8_t d);
#define ADDR_IT66021 0x49
#define I2C_L_Write(s, a, d) stub_write(3, s, a, d)
#define I2C_L_Read(s, a) stub_read(3, s, a)
H
done
printf '#pragma once\n#include <stdio.h>\n#define LOGI(...) do { } while (0)\n#define LOGE LOGI\n#define LOGD LOGI\n' > "$T/inc/log/log.h"
cat > "$T/harness.c" << 'C'
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t ram[256], reg[256];
static FILE *seq;
uint8_t stub_read(int port, uint8_t s, uint8_t a) { (void)port; return s == 0x49 ? reg[a] : 0; }
void stub_write(int port, uint8_t s, uint8_t a, uint8_t d) {
    fprintf(seq, "W %d %02x %02x %02x\n", port, s, a, d);
    if (s == 0x54) ram[a] = d; else if (s == 0x49) reg[a] = d;
}
void DM5680_ResetHDMI_RX(int v) { (void)v; }
int usleep(unsigned u) { fprintf(seq, "SLEEP %u\n", u); return 0; }
void IT66021_edid(void);
#ifdef NEWAPI
void IT66021_audio_config(bool on);
#endif
int main(int argc, char **argv) { // argv[1] = sequence file, argv[2] = served EDID file, argv[3] = "on"
    seq = fopen(argv[1], "w");
#ifdef NEWAPI
    IT66021_audio_config(argc > 3);
#endif
    IT66021_edid();
    fclose(seq);
    uint8_t e[256];
    memcpy(e, ram, sizeof(e));
    if ((reg[0xc0] & 3) == 0) { e[0x7f] = reg[0xc4]; e[0xff] = reg[0xc5]; } // shadow: checksums from RegC4/C5
    FILE *f = fopen(argv[2], "w");
    for (int i = 0; i < 256; i++) fprintf(f, "%02x%c", e[i], (i % 16 == 15) ? '\n' : ' ');
    fclose(f);
    return 0;
}
C
cat > "$T/check.c" << 'C'
/* validate a served EDID (256 bytes, hex); prints the audio claims; exit 1 on an error */
#include <stdio.h>
static unsigned e[256];
static int err = 0;
#define BAD(...) do { printf("  ERROR: " __VA_ARGS__); printf("\n"); err = 1; } while (0)
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "r");
    for (int i = 0; i < 256; i++) if (fscanf(f, "%x", &e[i]) != 1) return 2;
    static const unsigned hdr[8] = {0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0};
    for (int i = 0; i < 8; i++) if (e[i] != hdr[i]) BAD("block 0 header byte %d", i);
    for (int b = 0; b < 2; b++) {
        unsigned s = 0;
        for (int i = 0; i < 128; i++) s += e[128 * b + i];
        if (s & 0xff) BAD("block %d checksum: sum = 0x%02x", b, s & 0xff);
    }
    if (e[0x7e] != 1) BAD("extension count %u", e[0x7e]);
    const unsigned *c = e + 128;
    if (c[0] != 0x02 || c[1] != 0x03) BAD("not a CEA-861 rev 3 extension (%02x %02x)", c[0], c[1]);
    unsigned d = c[2];
    if (d < 4 || d > 127) BAD("DTD offset %u", d);
    int sads = 0, adb = 0, rates = -1, chans = -1, fmt = -1;
    unsigned p = 4;
    while (p < d) {
        unsigned tag = c[p] >> 5, len = c[p] & 31;
        if (p + 1 + len > d) { BAD("data block at %u (tag %u, len %u) runs past the DTD offset %u", p, tag, len, d); break; }
        if (tag == 1) {
            adb++;
            if (len % 3) BAD("audio data block length %u", len);
            for (unsigned k = 0; k + 2 < len; k += 3) {
                sads++;
                fmt = (c[p + 1 + k] >> 3) & 15;
                chans = (c[p + 1 + k] & 7) + 1;
                rates = c[p + 2 + k];
            }
        }
        p += 1 + len;
    }
    if (p != d) BAD("data block collection ends at %u, DTDs start at %u", p, d);
    for (unsigned q = d; q + 18 <= 127; q += 18) {
        if (c[q] == 0 && c[q + 1] == 0) break; // padding
        unsigned hact = c[q + 2] | ((c[q + 4] & 0xf0) << 4), vact = c[q + 5] | ((c[q + 7] & 0xf0) << 4);
        if (!hact || !vact) BAD("DTD at %u: %ux%u", q, hact, vact);
    }
    printf("  basic_audio=%u ycbcr444=%u ycbcr422=%u native_dtds=%u audio_blocks=%d sads=%d fmt=%d ch=%d rates=0x%02x\n",
           (c[3] >> 6) & 1, (c[3] >> 5) & 1, (c[3] >> 4) & 1, c[3] & 15, adb, sads, fmt, chans, rates & 0xff);
    return err;
}
C
# only the EDID part of the driver: from the audio_in flag (if any) to the end of IT66021_edid()
extract() {
    printf '#include <stdbool.h>\n#include <stdint.h>\n#include <unistd.h>\n#include "i2c.h"\n'
    awk '/^(static bool it66021_edid_48k_only|void IT66021_edid\(\))/ { on = 1 }
        on { print }
        /^void IT66021_edid\(\)/ { seen = 1 }
        on && seen && /^}/ { exit }' "$1"
}
if [ -n "${BASE_SRC:-}" ]; then
    cp "$BASE_SRC/src/driver/it66021.c" "$T/orig.c" 2> /dev/null && BASE="$BASE (BASE_SRC)"
else
    git -C "$TOP" show "$BASE:src/driver/it66021.c" > "$T/orig.c" 2> /dev/null || rm -f "$T/orig.c"
fi
if [ ! -s "$T/orig.c" ]; then
    echo "SKIP it66021_edid_test.sh: the base $BASE file is not available (${BASE_SRC:-$TOP is not a git worktree with $BASE}); set BASE_SRC=<a tree of $BASE>"
    exit 77
fi
extract "$T/orig.c" > "$T/orig/driver/edid.c"
extract "$SRC" > "$T/new/driver/edid.c"
grep -q "^void IT66021_edid()" "$T/new/driver/edid.c" || { echo "FAIL no IT66021_edid() in $SRC"; exit 1; }
gcc -Wall -I"$T/inc" -I"$T/orig/driver" -o "$T/o" "$T/harness.c" "$T/orig/driver/edid.c"
gcc -Wall -DNEWAPI -I"$T/inc" -I"$T/new/driver" -o "$T/n" "$T/harness.c" "$T/new/driver/edid.c"
gcc -w -o "$T/chk" "$T/check.c"
timeout -k 5 "$TL" "$T/o" "$T/o.seq" "$T/o.edid"
timeout -k 5 "$TL" "$T/n" "$T/n.seq" "$T/n.edid"
timeout -k 5 "$TL" "$T/n" "$T/a.seq" "$T/a.edid" on
rc=0
pass() { echo "PASS $*"; }
fail() {
    echo "FAIL $*"
    rc=1
}
if cmp -s "$T/o.seq" "$T/n.seq"; then pass "audio_in=0: I2C sequence identical to $BASE ($(grep -c '^W' "$T/o.seq") writes)"; else
    fail "audio_in=0: I2C sequence differs from $BASE"
    diff "$T/o.seq" "$T/n.seq" | head
fi
echo "audio_in=0 served EDID:"
if "$T/chk" "$T/n.edid" && cmp -s "$T/o.edid" "$T/n.edid" && "$T/chk" "$T/n.edid" | grep -q "basic_audio=1 .*rates=0x07"; then
    pass "audio_in=0: valid, identical to $BASE (Basic Audio, L-PCM 32/44.1/48 kHz)"
else
    fail "audio_in=0 EDID"
fi
echo "audio_in=1 served EDID:"
if "$T/chk" "$T/a.edid" > "$T/a.txt"; then pass "audio_in=1: both block checksums and the CEA structure valid"; else fail "audio_in=1: invalid EDID"; fi
cat "$T/a.txt"
if grep -q "basic_audio=0 .*audio_blocks=1 sads=1 fmt=1 ch=2 rates=0x04" "$T/a.txt"; then
    pass "audio_in=1: no Basic Audio, one SAD: L-PCM 2 ch 48 kHz only"
else
    fail "audio_in=1: audio claims not 48 kHz L-PCM only"
fi
nd=$(awk 'NR == FNR { for (i = 1; i <= NF; i++) a[(FNR - 1) * 16 + i - 1] = $i; next }
    { for (i = 1; i <= NF; i++) if (a[(FNR - 1) * 16 + i - 1] != $i) printf "%02x ", (FNR - 1) * 16 + i - 1 }' "$T/n.edid" "$T/a.edid")
if [ "$nd" = "83 8b ff " ]; then pass "audio_in=1: differs from audio_in=0 only in bytes 0x83, 0x8B, 0xFF"; else fail "audio_in=1 changes bytes: $nd"; fi
exit $rc
