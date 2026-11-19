#!/bin/sh
# Host test of the HDMI audio boot step and its per-boot record (src/record/hdmi_boot.c,
# src/record/soc_reg.c) and of system_exec_sunxi_dump() (src/util/system.c), against a model
# of /sys/class/sunxi_dump (one address latch, PH_CFG0) shared by forked processes. Checks:
#  - PH1-PH3 are switched at most once per boot (app restarts, a second run, stale, deleted or
#    corrupt records), never with [hdmi] audio_out other than exactly "1" (then no sunxi_dump
#    access at all); every check below runs for each target (Goggle 1, Goggle 2, BoxPro), and a
#    build for no known target reads and writes nothing whatever setting.ini says;
#  - every refuse path (fields not owned not as rc.sh leaves them, PH1-PH3 in neither known
#    state, reads that do not agree, garbage or other formats, lock not available, /tmp not
#    writable) leaves PH_CFG0 untouched and reports "pin check failed";
#  - a write that does not read back: the stock value written back and verified, fault;
#  - killed (SIGKILL) at every point of the boot step and of the recovery: the next app start
#    ends with PH_CFG0 = stock (fault) or = I2S0 and HDMI audio on, never anything else;
#  - later faults (HDMI chip, audio path) keep HDMI out audio off across app restarts;
#  - system_exec_sunxi_dump(): runs under the lock; a held lock delays it at most ~1 s, then
#    it runs anyway; within 10 s it does not wait again.
# SRC_TOP=<tree>: test another copy of the sources (test/c_mutants.sh uses it).
# Every test program runs under a time limit (TEST_TIMEOUT, default 300 s): a hang (e.g. a lock
# left held) is a failure, not a test that never ends.
# Usage: sh utilities/probe/test/hdmi_boot_test.sh
set -eu
TL=${TEST_TIMEOUT:-300}
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=${SRC_TOP:-$TOP}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/inc/log" "$T/inc/record" "$T/inc/util" "$T/run"
printf '#pragma once\n#include <stdio.h>\n#define LOGD(...) do { fprintf(stderr, __VA_ARGS__); fputc(10, stderr); } while (0)\n#define LOGI LOGD\n#define LOGE LOGD\n#define LOGW LOGD\n' > "$T/inc/log/log.h"
printf '#pragma once\nconst char *fs_basename(const char *p);\n' > "$T/inc/util/filesystem.h"
cp "$TOP/lib/minIni/src/minIni.h" "$TOP/lib/minIni/src/minGlue.h" "$TOP/lib/minIni/src/minIni.c" "$T/inc/"
cp "$SRC/src/record/appmsg.h" "$SRC/src/record/soc_reg.h" "$SRC/src/record/soc_reg.c" "$SRC/src/record/hdmi_boot.h" "$SRC/src/record/hdmi_boot.c" "$T/inc/record/"
cp "$SRC/src/util/system.c" "$SRC/src/util/system.h" "$T/inc/util/"
cp "$TOP/mkapp/app/setting.ini" "$T/stock_setting.ini"

cat > "$T/harness.c" << 'C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "record/hdmi_boot.h"
#include "record/soc_reg.h"
#include "util/system.h"

/* ---- the sunxi_dump model, shared by forked processes ---- */
#define PH 0x0300b0fcu
typedef struct {
    uint32_t latch, ph, other;
    int dump_reads, node_writes, node_opens, ph_writes;
    uint32_t written[16];
    int race_at;      // this dump read (1-based) returns another register
    int disagree;     // every read returns another value
    int garbage;      // the answer's format: 0 good, 1 non-hex, 2 cut short, 3 awr's format, 4 empty, 5 no newline, 6 nine digits, 7 spaces
    int stick;        // 1 a write takes effect; 0 the register keeps its value
    uint32_t mangle;  // != 0: a write of the I2S0 value reads back as this
    int restore_fails;// the write of the stock value does not take effect either
    int kill_at;      // checkpoint at which the process kills itself
    int hit;          // ... and it was reached
} model_t;
static model_t *M;
static const char *DIR_;
void hb_test_checkpoint(int n) { if (M->kill_at == n) { M->hit = 1; raise(SIGKILL); } }

int t_open(const char *p, int fl, ...) {
    if (strcmp(p, "/sys/class/sunxi_dump/dump") == 0) { M->node_opens++; return 900; }
    if (strcmp(p, "/sys/class/sunxi_dump/write") == 0) { M->node_opens++; return 901; }
    va_list ap; va_start(ap, fl); int m = va_arg(ap, int); va_end(ap);
    return open(p, fl, m);
}
ssize_t t_write(int fd, const void *b, size_t n) {
    char s[64]; snprintf(s, sizeof(s), "%.*s", (int)n, (const char *)b);
    if (fd == 900) { M->latch = (uint32_t)strtoul(s, NULL, 16); return (ssize_t)n; }
    if (fd == 901) {
        char *e; uint32_t a = (uint32_t)strtoul(s, &e, 16), v = (uint32_t)strtoul(e, NULL, 16);
        M->node_writes++;
        if (a == PH) {
            if (M->ph_writes < 16) M->written[M->ph_writes] = v;
            M->ph_writes++;
            if (v == 0x35517751u && M->restore_fails) return (ssize_t)n;
            if (M->mangle && v == 0x35513331u) v = M->mangle;
            if (M->stick || v == 0x35517751u) M->ph = v;
        } else M->other = v;
        return (ssize_t)n;
    }
    return write(fd, b, n);
}
ssize_t t_read(int fd, void *b, size_t n) {
    if (fd != 900) return read(fd, b, n);
    M->dump_reads++;
    uint32_t v = (M->latch == PH) ? M->ph : M->other;
    if (M->race_at == M->dump_reads) v = 0x00000000u;     // another process moved the latch
    if (M->disagree) v = M->ph ^ (uint32_t)M->dump_reads;
    switch (M->garbage) {
    case 1: return snprintf((char *)b, n, "0xZZ51775g\n");
    case 2: return snprintf((char *)b, n, "0x3551");
    case 3: return snprintf((char *)b, n, "read 0x0300b0fc:0x%08x\n", v);
    case 4: return 0;
    case 5: return snprintf((char *)b, n, "0x%08x", v);
    case 6: return snprintf((char *)b, n, "0x1%08x\n", v);
    case 7: return snprintf((char *)b, n, " 0x%08x \n", v);
    case 8: return snprintf((char *)b, n, "0x%08xx", v); // eight digits and a stray character, no newline
    default: return snprintf((char *)b, n, "0x%08x\n", v);
    }
}
int t_close(int fd) { return (fd == 900 || fd == 901) ? 0 : close(fd); }
int t_flock(int fd, int op) { return flock(fd, op); }
const char *fs_basename(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* ---- files ---- */
static char P_SET[512], P_BOOT[512], P_ID[512], P_LOCK[512];
static void put(const char *f, const char *s) { FILE *o = fopen(f, "w"); fputs(s, o); fclose(o); }
static void new_boot(int n) { // rc.sh: stock PH_CFG0; a new boot id
    char id[64]; snprintf(id, sizeof(id), "%08x-1234-4abc-8def-0123456789%02x\n", 0x5eed0000u + (unsigned)n, n & 0xff);
    put(P_ID, id);
    M->ph = 0x35517751u;
}
static void reset(const char *setting, const char *mutate) {
    memset(M, 0, sizeof(*M)); M->stick = 1; M->ph = 0x35517751u;
    unlink(P_BOOT);
    if (setting) put(P_SET, setting); else unlink(P_SET);
    (void)mutate;
}
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static int run_child(int (*fn)(void)) { // in a forked process (it may be killed): -signal or the exit status
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) _exit(fn());
    int st = 0; waitpid(p, &st, 0);
    return WIFSIGNALED(st) ? -WTERMSIG(st) : WEXITSTATUS(st);
}
static int boot_step(void) { return hdmi_boot_run(P_SET); }
static void do_boot(void) { (void)hdmi_boot_run(P_SET); }
static bool A_out, A_in; static hb_fault_t A_fault;
static void app(bool enabled) { hdmi_boot_app_start(enabled, &A_out, &A_in, &A_fault); }
static int app_step(void) { app(true); return 0; }
static int rec(hdmi_boot_t *b) { return hdmi_boot_read(b); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static const char *ON = "[hdmi]\naudio_out=1\naudio_in=0\n";

int main(int argc, char **argv) {
    DIR_ = argv[1]; (void)argc;
    snprintf(P_SET, sizeof(P_SET), "%s/setting.ini", DIR_);
    snprintf(P_BOOT, sizeof(P_BOOT), "%s/boot", DIR_);
    snprintf(P_ID, sizeof(P_ID), "%s/boot_id", DIR_);
    snprintf(P_LOCK, sizeof(P_LOCK), "%s/lock", DIR_);
    M = mmap(NULL, sizeof(*M), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    hdmi_boot_t b;
    new_boot(1);

    /* 1. audio_out other than exactly 1: no sunxi_dump access at all, record "off" */
    static const char *offs[] = {
        NULL, "", "[hdmi]\naudio_out=0\n", "[hdmi]\naudio_out=01\n", "[hdmi]\naudio_out=2\n", "[hdmi]\naudio_out=true\n",
        "[hdmi]\naudio_out=1x\n", "[hdmi]\naudio_out=-1\n", "[hdmi]\naudio_out=\n", "[hdmi]\naudio_out\n", "[hdmi]\naudio_ou",
        "[hdmi]\naudio_out=+1\n", "[hdmi]\naudio_out=1.0\n", "[hdmi]\naudio_out=on\n", "[other]\naudio_out=1\n", "audio_out=1\n",
        "[hdmi]\n;audio_out=1\n", "[hdmi]\n#audio_out=1\n", "\x01\x02\xff\xfe[hdmi\naudio_out=1", "[hdmi]\naudio_out=11\n",
        "[hdmi]\naudio_out=0x1\n", "[hdmi]\naudio_in=1\n"};
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        reset(offs[i], NULL); new_boot(1);
        do_boot();
        app(false);
        CHECK(M->node_opens == 0 && rec(&b) == HB_REC_OK && !b.out && b.pins == HB_PINS_UNTOUCHED && !A_out && A_fault == HB_FAULT_NONE,
              "setting #%u (%s): off, no sunxi_dump access (%d opens)", i, offs[i] ? "given" : "no file", M->node_opens);
    }
    { // the stock setting.ini and an unreadable one
        char s[600]; snprintf(s, sizeof(s), "%s/../stock_setting.ini", DIR_);
        reset(NULL, NULL); new_boot(1); hdmi_boot_run(s); app(false);
        CHECK(M->node_opens == 0 && rec(&b) == HB_REC_OK && !b.out, "stock setting.ini: off, no sunxi_dump access");
        reset(ON, NULL); new_boot(1); chmod(P_SET, 0); if (getuid() == 0) { unlink(P_SET); mkdir(P_SET, 0700); }
        do_boot(); app(false);
        CHECK(M->node_opens == 0 && rec(&b) == HB_REC_OK && !b.out, "unreadable setting.ini: off, no sunxi_dump access");
        rmdir(P_SET); unlink(P_SET);
    }
    reset("[hdmi]\naudio_in=1\naudio_out=0\n", NULL); new_boot(1); do_boot(); app(false);
    CHECK(A_in && !A_out && M->node_opens == 0, "audio_in=1 alone: HDMI in audio on this boot, no SoC access");
    put(P_SET, "[hdmi]\naudio_in=0\naudio_out=0\n"); app(false);
    CHECK(A_in, "audio_in switched off without a restart: still on for this boot (boot-only)");

    /* 2. the switch: once per boot */
    static const char *ons[] = {"[hdmi]\naudio_out=1\n", "[hdmi]\naudio_out = 1 \n", "[HDMI]\nAUDIO_OUT=1\n",
                                "[hdmi]\naudio_out=1 ; comment\n", "[hdmi]\r\naudio_out=1\r\n", "[scan]\nchannel=1\n[hdmi]\naudio_in=1\naudio_out=1\n"};
    for (unsigned i = 0; i < sizeof(ons) / sizeof(ons[0]); i++) {
        reset(ons[i], NULL); new_boot(1); do_boot(); app(true);
        CHECK(M->ph == 0x35513331u && M->ph_writes == 1 && M->written[0] == 0x35513331u && A_out && A_fault == HB_FAULT_NONE,
              "audio_out=1 variant #%u: PH_CFG0 0x35517751 -> 0x35513331 (one write), HDMI out audio on", i);
    }
    reset(ON, NULL); new_boot(1); do_boot();
    for (int i = 0; i < 30; i++) app(true);
    for (int i = 0; i < 5; i++) do_boot();
    CHECK(M->ph_writes == 1 && A_out && hdmi_boot_out_ready(), "30 app restarts and 5 more runs in the same boot: still one write, on");
    put(P_SET, "[hdmi]\naudio_out=0\n"); app(false);
    CHECK(A_out && M->ph_writes == 1 && M->ph == 0x35513331u, "switched Off without a restart: still on for this boot, nothing undone");
    put(P_SET, ON); new_boot(2); do_boot(); app(true);
    CHECK(M->ph_writes == 2 && A_out && rec(&b) == HB_REC_OK, "next boot (rc.sh wrote stock, new boot id): switched again, once");
    new_boot(3); put(P_SET, "[hdmi]\naudio_out=0\n"); do_boot(); app(false);
    CHECK(M->ph == 0x35517751u && M->ph_writes == 2 && !A_out && A_fault == HB_FAULT_NONE, "Off + restart: stock (rc.sh), nothing written, no fault");

    /* 3. record deleted / stale / corrupt */
    reset(ON, NULL); new_boot(1); do_boot(); unlink(P_BOOT); app(true);
    CHECK(!A_out && A_fault == HB_FAULT_PINS && M->ph_writes == 1, "record deleted, app restart: off, 'pin check failed', nothing written");
    reset(ON, NULL); new_boot(1); do_boot(); new_boot(9); M->ph = 0x35513331u; app(true);
    CHECK(!A_out && A_fault == HB_FAULT_PINS && M->ph_writes == 1, "record of another boot: off, fault, nothing written");
    reset(ON, NULL); new_boot(1); do_boot(); unlink(P_BOOT); app(false);
    CHECK(!A_out && A_fault == HB_FAULT_NONE, "record deleted, setting now 0: off, no fault");
    static const char *bad[] = {
        "", "garbage\n", "version=1\n", "version=2\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\ndetail=x\ndetail=y\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=bogus\nph_cfg0=0x35513331\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=pin check failed\nph_cfg0=0x35513331\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35517751\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=0\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\ndetail=x", // no final newline
        "version=1\nboot_id=%s\nphase=maybe\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=yes\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x3551333\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=0\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\ndetail=x\n",
        "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=audio\nfault=\nph_cfg0=0x35513331\nextra=1\ndetail=x\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char id[64] = "", txt[1200];
        reset(ON, NULL); new_boot(1);
        FILE *f = fopen(P_ID, "r"); if (fgets(id, sizeof(id), f)) id[strcspn(id, "\n")] = 0; fclose(f);
        snprintf(txt, sizeof(txt), bad[i], id);
        M->ph = 0x35513331u; put(P_BOOT, txt);
        app(true);
        CHECK(rec(&b) != HB_REC_OK && !A_out && A_fault == HB_FAULT_PINS && M->node_opens == 0, "corrupt record #%u: off, fault, no SoC access", i);
    }
    reset(ON, NULL); new_boot(1); put(P_BOOT, "garbage\n"); do_boot(); app(true);
    CHECK(M->node_opens == 0 && rec(&b) == HB_REC_OK && b.fault == HB_FAULT_PINS && !A_out, "unreadable record before the boot step: not touched, fault");
    { char big[3000]; memset(big, 'x', sizeof(big) - 2); big[sizeof(big) - 2] = '\n'; big[sizeof(big) - 1] = 0;
      reset(ON, NULL); new_boot(1); put(P_BOOT, big); app(true);
      CHECK(!A_out && A_fault == HB_FAULT_PINS && M->node_opens == 0, "oversized record: off, fault"); }
    reset(ON, NULL); new_boot(1); put(P_ID, "not-a-boot-id\n"); do_boot(); app(true);
    CHECK(A_out && M->ph_writes == 1, "boot id unreadable for both: the record (tmpfs) still counts, one write");

    /* 4. refuse paths: PH_CFG0 untouched, fault */
    static const uint32_t others[] = {0x35517750u, 0x35517752u, 0x35507751u, 0x35417751u, 0x34517751u, 0x25517751u, 0x35517351u,
                                      0x35513351u, 0x35513751u, 0x35510001u, 0x00000000u, 0xffffffffu, 0x35513330u, 0x35517771u};
    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        reset(ON, NULL); new_boot(1); M->ph = others[i]; do_boot(); app(true);
        CHECK(M->ph_writes == 0 && M->ph == others[i] && rec(&b) == HB_REC_OK && b.pins == HB_PINS_OTHER && b.ph_cfg0 == others[i] &&
              strstr(b.detail, "refused") && !A_out && A_fault == HB_FAULT_PINS, "PH_CFG0 0x%08x: refused, untouched, value logged, fault", others[i]);
    }
    for (int r = 1; r <= 3; r++) {
        reset(ON, NULL); new_boot(1); M->race_at = r; do_boot(); app(true);
        CHECK(M->ph_writes == 0 && !A_out && A_fault == HB_FAULT_PINS, "read %d of 3 interleaved: untouched, fault", r);
    }
    reset(ON, NULL); new_boot(1); M->disagree = 1; do_boot(); app(true);
    CHECK(M->ph_writes == 0 && !A_out && A_fault == HB_FAULT_PINS, "reads that never agree: untouched, fault");
    for (int g = 1; g <= 8; g++) {
        reset(ON, NULL); new_boot(1); M->garbage = g; do_boot(); app(true);
        CHECK(M->ph_writes == 0 && !A_out && A_fault == HB_FAULT_PINS, "dump answer format %d (garbage / cut / awr / empty / no newline / 9 digits / spaces / 8 digits + a stray character): untouched, fault", g);
    }
    { // lock held by another process for the whole boot step
        reset(ON, NULL); new_boot(1);
        int lk = open(P_LOCK, O_RDWR | O_CREAT, 0644); flock(lk, LOCK_EX);
        double t0 = now_s(); int rc = run_child(boot_step); double dt = now_s() - t0;
        CHECK(rc == 1 && M->node_opens == 0 && dt > 0.9 && dt < 1.6, "lock held: gave up after %.2f s, no SoC access", dt);
        app(true);
        CHECK(!A_out && A_fault == HB_FAULT_PINS, "lock held at boot: off for this boot, fault");
        close(lk);
        reset(ON, NULL); new_boot(1); do_boot(); lk = open(P_LOCK, O_RDWR | O_CREAT, 0644); flock(lk, LOCK_EX);
        app(true); close(lk);
        CHECK(!A_out && A_fault == HB_FAULT_PINS && M->ph_writes == 1, "lock held at app start: off, fault, nothing written");
        app(true);
        CHECK(!A_out && A_fault == HB_FAULT_PINS, "... and stays off at the next app start (this boot)");
    }
    { // /tmp not writable: the record cannot be written -> nothing touched
        char tmpn[600]; snprintf(tmpn, sizeof(tmpn), "%s.tmp", P_BOOT);
        reset(ON, NULL); new_boot(1); mkdir(tmpn, 0700); do_boot(); app(true);
        CHECK(M->node_opens == 0 && !A_out && A_fault == HB_FAULT_PINS, "/tmp not writable: PH_CFG0 not touched, fault");
        rmdir(tmpn);
    }

    /* 5. a write that does not read back: stock back, verified */
    reset(ON, NULL); new_boot(1); M->stick = 0; do_boot(); app(true);
    CHECK(M->ph == 0x35517751u && M->ph_writes == 2 && M->written[1] == 0x35517751u && rec(&b) == HB_REC_OK && b.pins == HB_PINS_STOCK &&
          !A_out && A_fault == HB_FAULT_PINS, "write not taken: stock written back and verified, fault");
    reset(ON, NULL); new_boot(1); M->mangle = 0x35513330u; do_boot(); app(true);
    CHECK(M->ph == 0x35517751u && M->ph_writes == 2 && rec(&b) == HB_REC_OK && b.pins == HB_PINS_STOCK && !A_out && A_fault == HB_FAULT_PINS,
          "write reads back wrong (0x35513330): stock written back, verified, fault");
    reset(ON, NULL); new_boot(1); M->mangle = 0x35513330u; M->restore_fails = 1; do_boot(); app(true);
    CHECK(M->ph_writes == 2 && M->written[1] == 0x35517751u && rec(&b) == HB_REC_OK && b.pins == HB_PINS_UNKNOWN && !A_out &&
          A_fault == HB_FAULT_PINS && M->ph_writes == 2, "the stock value does not verify either: fault, logged, no further write");
    reset(ON, NULL); new_boot(1); do_boot(); M->ph = 0x35517751u; app(true);
    CHECK(!A_out && A_fault == HB_FAULT_PINS && M->ph_writes == 1, "pins put back by someone else before the app start: off, nothing written");

    /* 6. killed at every point of the boot step, then the app starts (twice) */
    int kills = 0;
    for (int k = 1; k <= 6; k++) {
        for (int variant = 0; variant < 2; variant++) {
            reset(ON, NULL); new_boot(1); M->kill_at = k; if (variant) M->stick = 0;
            int rc = run_child(boot_step);
            const int hit = M->hit;
            kills += hit;
            M->kill_at = 0;
            app(true); bool o1 = A_out; hb_fault_t f1 = A_fault; int w1 = M->ph_writes;
            app(true);
            bool okstate = (M->ph == 0x35513331u && o1 && A_out && f1 == HB_FAULT_NONE) ||
                           (M->ph == 0x35517751u && !o1 && !A_out && f1 == HB_FAULT_PINS && A_fault == HB_FAULT_PINS);
            CHECK((hit ? rc == -SIGKILL : rc >= 0) && okstate && M->ph_writes <= 3 && M->ph_writes == w1 && rec(&b) == HB_REC_OK && b.phase == HB_PHASE_DONE,
                  "%s at point %d%s: PH_CFG0 0x%08x, HDMI out audio %s, %d writes, nothing at the 2nd app start",
                  hit ? "killed" : "not reached", k, variant ? " (write not taken)" : "", M->ph, A_out ? "on" : "off", M->ph_writes);
        }
    }
    CHECK(kills >= 10, "%d of the 12 kill points were reached", kills);
    reset(ON, NULL); new_boot(1); M->kill_at = 3; run_child(boot_step);
    M->kill_at = 7; M->hit = 0; int rc7 = run_child(app_step);
    CHECK(rc7 == -SIGKILL && M->hit && M->ph == 0x35517751u, "app killed in the recovery, right after writing the stock value");
    M->kill_at = 0; app(true); bool o7 = A_out; app(true);
    CHECK(M->ph == 0x35517751u && !o7 && !A_out && A_fault == HB_FAULT_PINS && M->ph_writes == 2 && rec(&b) == HB_REC_OK && b.phase == HB_PHASE_DONE,
          "... the next app start finishes it (stock, fault, no further write)");

    /* 7. later faults stay for the boot; the pins are not touched */
    reset(ON, NULL); new_boot(1); do_boot(); app(true);
    hdmi_boot_set_fault(HB_FAULT_CHIP); app(true);
    CHECK(!A_out && A_fault == HB_FAULT_CHIP && M->ph == 0x35513331u && M->ph_writes == 1 && !hdmi_boot_out_ready(),
          "HDMI chip check failed: off across app restarts, pins untouched");
    hdmi_boot_set_fault(HB_FAULT_PATH); app(true);
    CHECK(A_fault == HB_FAULT_CHIP, "the first fault is kept");
    reset(ON, NULL); new_boot(1); do_boot(); hdmi_boot_set_fault(HB_FAULT_PATH); app(true);
    CHECK(!A_out && A_fault == HB_FAULT_PATH, "audio path failed: off across app restarts");
    reset(ON, NULL); new_boot(1); do_boot();
    CHECK(hdmi_boot_out_ready(), "record daemon: ready when switched and no fault");
    unlink(P_BOOT);
    CHECK(!hdmi_boot_out_ready(), "record daemon: not ready without a record");

    /* 8. faults reported only when asked for */
    reset("[hdmi]\naudio_out=0\n", NULL); new_boot(1); do_boot(); put(P_SET, ON); app(true);
    CHECK(!A_out && A_fault == HB_FAULT_NONE, "switched On after boot: not active (Restart), no fault");
    CHECK(strcmp(hdmi_boot_fault_text(HB_FAULT_PINS), "pin check failed") == 0 && strcmp(hdmi_boot_fault_text(HB_FAULT_CHIP), "HDMI chip check failed") == 0 &&
          strcmp(hdmi_boot_fault_text(HB_FAULT_PATH), "audio path failed") == 0 && hdmi_boot_fault_text(HB_FAULT_NONE) == NULL, "fault texts");

    /* 9. system_exec_sunxi_dump(): under the lock; a held lock costs at most ~1 s, then once per 10 s */
    {
        char cmd[1200];
        reset(NULL, NULL);
        snprintf(cmd, sizeof(cmd), "flock -n %s true && echo free > %s/res || echo held > %s/res", P_LOCK, DIR_, DIR_);
        system_exec_sunxi_dump(cmd);
        char res[16] = ""; char rf[600]; snprintf(rf, sizeof(rf), "%s/res", DIR_); FILE *f = fopen(rf, "r"); if (f) { if (!fgets(res, sizeof(res), f)) res[0] = 0; fclose(f); }
        CHECK(strncmp(res, "held", 4) == 0, "system_exec_sunxi_dump: the command runs while the lock is held");
        int lk = open(P_LOCK, O_RDWR | O_CREAT, 0644); flock(lk, LOCK_EX);
        snprintf(cmd, sizeof(cmd), "echo ran > %s/res", DIR_);
        double t0 = now_s(); system_exec_sunxi_dump(cmd); double dt1 = now_s() - t0;
        f = fopen(rf, "r"); res[0] = 0; if (f) { if (!fgets(res, sizeof(res), f)) res[0] = 0; fclose(f); }
        t0 = now_s(); system_exec_sunxi_dump(cmd); double dt2 = now_s() - t0;
        CHECK(strncmp(res, "ran", 3) == 0 && dt1 > 0.9 && dt1 < 1.6 && dt2 < 0.3, "lock held: ran anyway after %.2f s; the next call %.2f s", dt1, dt2);
        close(lk);
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
C

mk() { # $1 output, $2 target macro
    # shellcheck disable=SC2089,SC2090 # the quotes are part of the macro values
    D="-DHDMIA_REQ_FILE=\"$T/run/req\" -DHDMIA_STATE_FILE=\"$T/run/state\" -DHDMIA_LOCK_FILE=\"$T/run/lock\" -DHDMIA_BOOT_FILE=\"$T/run/boot\" -DHDMIA_BOOT_ID_FILE=\"$T/run/boot_id\""
    W="-Dopen=t_open -Dread=t_read -Dwrite=t_write -Dclose=t_close -Dflock=t_flock"
    # shellcheck disable=SC2086,SC2090 # D and W hold several options
    gcc -Wall -Wextra -Wno-unused-parameter -Werror -c -I"$T/inc" -I"$T/inc/record" $D $W -DHB_TEST "-D$2" -o "$T/hb.o" "$T/inc/record/hdmi_boot.c"
    # shellcheck disable=SC2086,SC2090
    gcc -Wall -Wextra -Werror -c -I"$T/inc" -I"$T/inc/record" $D $W "-D$2" -o "$T/sr.o" "$T/inc/record/soc_reg.c"
    # shellcheck disable=SC2086,SC2090
    gcc -Wall -Werror -c -I"$T/inc" $D "-D$2" -o "$T/sys.o" "$T/inc/util/system.c"
    gcc -w -c -I"$T/inc" -o "$T/minIni.o" "$T/inc/minIni.c"
    # shellcheck disable=SC2086,SC2090
    gcc -Wall -I"$T/inc" $D "-D$2" -o "$1" "$T/harness.c" "$T/hb.o" "$T/sr.o" "$T/sys.o" "$T/minIni.o"
}
# every target the same: the BoxPro ships the Goggle 1's device tree, sys_config and rc.sh writes
# (docs/hdmi-audio.md), so it gets the same switch and every refuse path
for tgt in HDZGOGGLE HDZGOGGLE2 HDZBOXPRO; do
    echo "== $tgt"
    rm -rf "$T/run"
    mkdir -p "$T/run"
    mk "$T/t_$tgt" "$tgt"
    rc=0
    timeout -k 5 "$TL" "$T/t_$tgt" "$T/run" 2> "$T/log.txt" || rc=$?
    if [ "$rc" != 0 ]; then
        [ "$rc" = 124 ] && echo "FAIL $tgt: the test program did not end within $TL s (a hang: a lock left held?)"
        echo "--- log:"
        tail -30 "$T/log.txt"
        exit 1
    fi
done

# a build for no known target: HDMI audio not supported, nothing read or written, whatever
# setting.ini says
cat > "$T/notarget.c" << 'C'
#include <stdio.h>
#include "record/hdmi_boot.h"
int main(int argc, char **argv) {
    bool o, i; hb_fault_t f; hdmi_boot_t b;
    (void)argc;
    int r = hdmi_boot_run(argv[1]);
    hdmi_boot_app_start(true, &o, &i, &f);
    int ok = r == 0 && !o && !i && f == HB_FAULT_NONE && hdmi_boot_read(&b) == HB_REC_OK && !b.supported && !hdmi_boot_supported() &&
             !hdmi_boot_out_ready();
    printf("%s no known target: audio_out=1 audio_in=1 in setting.ini: not supported, nothing touched, no fault\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
C
rm -rf "$T/run"
mkdir -p "$T/run"
printf '[hdmi]\naudio_out=1\naudio_in=1\n' > "$T/run/setting.ini"
echo "5eed0001-1234-4abc-8def-012345678901" > "$T/run/boot_id"
# shellcheck disable=SC2089,SC2090
D="-DHDMIA_REQ_FILE=\"$T/run/req\" -DHDMIA_STATE_FILE=\"$T/run/state\" -DHDMIA_LOCK_FILE=\"$T/run/lock\" -DHDMIA_BOOT_FILE=\"$T/run/boot\" -DHDMIA_BOOT_ID_FILE=\"$T/run/boot_id\""
# no target macro; the real sysfs nodes do not exist on a PC: any access would fail this run (it must make none)
# shellcheck disable=SC2086,SC2090
gcc -Wall -Werror -I"$T/inc" $D -o "$T/nt" "$T/notarget.c" "$T/inc/record/hdmi_boot.c" "$T/inc/record/soc_reg.c" "$T/minIni.o"
timeout -k 5 "$TL" "$T/nt" "$T/run/setting.ini" 2>> "$T/log.txt" || exit 1
echo "ALL HDMI BOOT TESTS PASSED"
