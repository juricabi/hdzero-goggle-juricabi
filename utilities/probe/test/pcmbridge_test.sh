#!/bin/sh
# Host test of the record daemon's HDMI audio bridge (src/record/pcmbridge.c) against stub
# ALSA / MPP AI and a model of /sys/class/sunxi_dump (one address latch). Checks:
#  - capture XRUN (snd_pcm_wait() returns -EPIPE at once), a capture that cannot recover,
#    AI GetFrame failing at once: recovered with backoff, no busy loop;
#  - the bridge never writes PH_CFG0 (the pins are switched once per boot by hdmi_audio_boot);
#    a route to I2S0 is refused unless /tmp/hdmi_audio.boot says HDMI out audio is in effect
#    for this boot (missing, stale, faulted record);
#  - I2S0 CTL: verified read-modify-write of its mode / clock fields on start, exactly those put
#    back on stop; an interleaved, implausible or unsafe read, a held lock or a write that does
#    not read back: the start fails, the old value stays (or is written back and verified);
#  - a sink that does not open: state "failed", AI device disabled only if nobody else uses it;
#  - an HDMI-in source at 44.1 kHz is muted, at 48 kHz played; the request number is echoed.
# All of it for each target (Goggle 1, Goggle 2, BoxPro).
# SRC_TOP=<tree>: test another copy of the sources (test/c_mutants.sh uses it).
# The test program runs under a time limit (TEST_TIMEOUT, default 300 s): a hang is a failure.
# Usage: sh utilities/probe/test/pcmbridge_test.sh
set -eu
TL=${TEST_TIMEOUT:-300}
TOP=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=${SRC_TOP:-$TOP}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/inc/log"
printf '#pragma once\n#include <stdio.h>\n#define LOGD(...) do { fprintf(stderr, __VA_ARGS__); fputc(10, stderr); } while (0)\n#define LOGI LOGD\n#define LOGE LOGD\n#define LOGW LOGD\n' > "$T/inc/log/log.h"
cat > "$T/inc/ai2aenc.h" << 'H'
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef int ERRORTYPE;
typedef int AI_CHN;
typedef int AUDIO_SAMPLE_RATE_E;
typedef int AUDIO_BIT_WIDTH_E;
typedef int AUDIO_SOUND_MODE_E;
#define SUCCESS 0
#define MM_INVALID_CHN (-1)
#define AIO_MAX_CHN_NUM 4
#define ERR_AI_EXIST 0xa0028004
typedef struct { unsigned u32ChnCnt; AUDIO_SAMPLE_RATE_E enSamplerate; AUDIO_BIT_WIDTH_E enBitwidth; } AIO_ATTR_S;
typedef struct { AUDIO_SAMPLE_RATE_E mSamplerate; AUDIO_BIT_WIDTH_E mBitwidth; AUDIO_SOUND_MODE_E mSoundmode;
    void *mpAddr; unsigned long long mTimeStamp; unsigned mSeq; unsigned mLen; unsigned mId; } AUDIO_FRAME_S;
typedef struct { int dummy; } AEC_FRAME_S;
typedef struct { int aiDev; AI_CHN aiChn; int sampleRate; int channels; int bitsPerSample; int mFrameSize; } AiParams_t;
ERRORTYPE AW_MPI_AI_SetPubAttr(int, AIO_ATTR_S *);
ERRORTYPE AW_MPI_AI_Enable(int);
ERRORTYPE AW_MPI_AI_Disable(int);
ERRORTYPE AW_MPI_AI_CreateChn(int, AI_CHN);
ERRORTYPE AW_MPI_AI_EnableChn(int, AI_CHN);
ERRORTYPE AW_MPI_AI_DisableChn(int, AI_CHN);
ERRORTYPE AW_MPI_AI_ResetChn(int, AI_CHN);
ERRORTYPE AW_MPI_AI_DestroyChn(int, AI_CHN);
ERRORTYPE AW_MPI_AI_GetFrame(int, AI_CHN, AUDIO_FRAME_S *, AEC_FRAME_S *, int);
ERRORTYPE AW_MPI_AI_ReleaseFrame(int, AI_CHN, AUDIO_FRAME_S *, AEC_FRAME_S *);
H
cp "$TOP/lib/minIni/src/minIni.h" "$TOP/lib/minIni/src/minGlue.h" "$TOP/lib/minIni/src/minIni.c" "$T/inc/"
cat > "$T/harness.c" << 'C'
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include "ai2aenc.h"
#include "pcmbridge_alsa.h"
typedef struct PcmBridge PcmBridge_t;
PcmBridge_t *pcmbridge_initSys(void);
int pcmbridge_start(PcmBridge_t *, const char *, AiParams_t *, int aiDevFree);
void pcmbridge_stop(PcmBridge_t *, int);
void pcmbridge_deinitSys(PcmBridge_t *, int);

/* ---- sunxi_dump model: fds 900 (dump) and 901 (write) ---- */
static uint32_t regs_a[8], regs_v[8];
static int nregs;
static uint32_t latch;
static int race_addr_reads, race_at = -1, echo_addr, nwrites, ph_writes, ctl_stuck;
static uint32_t race_addr;
static uint32_t reg(uint32_t a) { for (int i = 0; i < nregs; i++) if (regs_a[i] == a) return regs_v[i]; return 0; }
static void reg_set(uint32_t a, uint32_t v) {
    for (int i = 0; i < nregs; i++) if (regs_a[i] == a) { regs_v[i] = v; return; }
    regs_a[nregs] = a; regs_v[nregs++] = v;
}
int t_open(const char *p, int fl, ...) {
    if (strcmp(p, "/sys/class/sunxi_dump/dump") == 0) return 900;
    if (strcmp(p, "/sys/class/sunxi_dump/write") == 0) return 901;
    va_list ap; va_start(ap, fl); int m = va_arg(ap, int); va_end(ap);
    return open(p, fl, m);
}
ssize_t t_write(int fd, const void *b, size_t n) {
    char s[64]; snprintf(s, sizeof(s), "%.*s", (int)n, (const char *)b);
    if (fd == 900) { latch = (uint32_t)strtoul(s, NULL, 16); return (ssize_t)n; }
    if (fd == 901) {
        char *e; uint32_t a = (uint32_t)strtoul(s, &e, 16), v = (uint32_t)strtoul(e, NULL, 16);
        nwrites++;
        if (a == 0x0300b0fc) ph_writes++;
        if (!(ctl_stuck && a == 0x05090000 && v != 0x00060000)) reg_set(a, v); // ctl_stuck: only the reset value is taken
        return (ssize_t)n;
    }
    return write(fd, b, n);
}
ssize_t t_read(int fd, void *b, size_t n) {
    if (fd != 900) return read(fd, b, n);
    uint32_t a = latch;
    if (a == race_addr && race_addr_reads++ == race_at) a = 0x0300b0f8; // another process moved the latch
    return snprintf((char *)b, n, "0x%x\n", echo_addr ? latch : reg(a));
}
int t_close(int fd) { return (fd == 900 || fd == 901) ? 0 : close(fd); }
int t_flock(int fd, int op) { return flock(fd, op); }

/* ---- ALSA stubs ---- */
struct _snd_pcm { int cap; snd_pcm_state_t st; };
static int cap_mode; // 0 healthy (rate cap_rate), 1 XRUN storm, 2 cannot recover
static unsigned cap_rate = 48000;
static long waits, recovers, opens, readis, nonzero_out, out_frames, sink_fail;
static struct timespec t0;
static double since(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9; }
static long delivered;
int snd_pcm_open(snd_pcm_t **p, const char *name, snd_pcm_stream_t dir, int mode) {
    (void)mode;
    if (dir == SND_PCM_STREAM_PLAYBACK && sink_fail) return -EBUSY;
    *p = calloc(1, sizeof(**p)); (*p)->cap = dir == SND_PCM_STREAM_CAPTURE; (*p)->st = SND_PCM_STATE_PREPARED;
    if ((*p)->cap) opens++;
    (void)name; return 0;
}
int snd_pcm_close(snd_pcm_t *p) { free(p); return 0; }
int snd_pcm_set_params(snd_pcm_t *p, snd_pcm_format_t f, snd_pcm_access_t a, unsigned c, unsigned r, int s, unsigned l) { (void)p; (void)f; (void)a; (void)c; (void)r; (void)s; (void)l; return 0; }
static long due(void) { return (long)(since() * cap_rate); }
int snd_pcm_wait(snd_pcm_t *p, int ms) {
    (void)ms; waits++;
    if (cap_mode != 0) return -EPIPE; // XRUN: alsa-lib returns at once
    usleep(5000); (void)p; return 1;
}
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *p, void *buf, snd_pcm_uframes_t n) {
    (void)p; readis++;
    long d = due() - delivered; if (d <= 0) return -EAGAIN;
    if ((unsigned long)d > n) d = (long)n;
    int16_t *s = buf; for (long i = 0; i < 2 * d; i++) s[i] = 1000;
    delivered += d; return d;
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *p, const void *buf, snd_pcm_uframes_t n) {
    (void)p; const int16_t *s = buf;
    for (unsigned long i = 0; i < 2 * n; i++) if (s[i]) { nonzero_out++; break; }
    out_frames += (long)n; return (snd_pcm_sframes_t)n;
}
int snd_pcm_recover(snd_pcm_t *p, int e, int s) { (void)e; (void)s; recovers++; if (cap_mode == 2) return -ENODEV; p->st = SND_PCM_STATE_PREPARED; return 0; }
int snd_pcm_start(snd_pcm_t *p) { p->st = SND_PCM_STATE_RUNNING; return 0; }
int snd_pcm_drop(snd_pcm_t *p) { (void)p; return 0; }
int snd_pcm_prepare(snd_pcm_t *p) { p->st = SND_PCM_STATE_PREPARED; return 0; }
snd_pcm_state_t snd_pcm_state(snd_pcm_t *p) { return p->st; }
snd_pcm_sframes_t snd_pcm_avail(snd_pcm_t *p) { (void)p; long d = due() - delivered; return d > 0 ? d : 0; }
int snd_pcm_delay(snd_pcm_t *p, snd_pcm_sframes_t *d) { (void)p; *d = 1440; return 0; } // at the target: no drift action
const char *snd_strerror(int e) { (void)e; return "stub"; }

/* ---- MPP AI stubs ---- */
static int ai_fail_now, ai_enabled, ai_disables, ai_chans;
static int16_t ai_buf[1024 * 2];
ERRORTYPE AW_MPI_AI_SetPubAttr(int d, AIO_ATTR_S *a) { (void)d; (void)a; return 0; }
ERRORTYPE AW_MPI_AI_Enable(int d) { (void)d; ai_enabled = 1; return 0; }
ERRORTYPE AW_MPI_AI_Disable(int d) { (void)d; ai_enabled = 0; ai_disables++; return 0; }
ERRORTYPE AW_MPI_AI_CreateChn(int d, AI_CHN c) { (void)d; (void)c; ai_chans++; return 0; }
ERRORTYPE AW_MPI_AI_EnableChn(int d, AI_CHN c) { (void)d; (void)c; return 0; }
ERRORTYPE AW_MPI_AI_DisableChn(int d, AI_CHN c) { (void)d; (void)c; return 0; }
ERRORTYPE AW_MPI_AI_ResetChn(int d, AI_CHN c) { (void)d; (void)c; return 0; }
ERRORTYPE AW_MPI_AI_DestroyChn(int d, AI_CHN c) { (void)d; (void)c; ai_chans--; return 0; }
static long getframes;
ERRORTYPE AW_MPI_AI_GetFrame(int d, AI_CHN c, AUDIO_FRAME_S *f, AEC_FRAME_S *a, int ms) {
    (void)d; (void)c; (void)a; (void)ms; getframes++;
    if (ai_fail_now) return -1; // fails at once (not a timeout)
    usleep(21333); // 1024 frames at 48 kHz
    f->mpAddr = ai_buf; f->mLen = sizeof(ai_buf); return 0;
}
ERRORTYPE AW_MPI_AI_ReleaseFrame(int d, AI_CHN c, AUDIO_FRAME_S *f, AEC_FRAME_S *a) { (void)d; (void)c; (void)f; (void)a; return 0; }

/* ---- the tests ---- */
static const char *REQ, *STATE;
static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static void req(long seq, const char *src, const char *sink, int fix) {
    FILE *f = fopen(REQ, "w");
    fprintf(f, "[hdmi_audio]\nseq=%ld\nactive=1\nsrc=%s\nsink=%s\nsink2=\ni2s0_fix=%d\n", seq, src, sink, fix);
    fclose(f);
}
static int state_is(const char *st, long seq) {
    char buf[512] = "", want[64]; FILE *f = fopen(STATE, "r"); if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    snprintf(want, sizeof(want), "state=%s\nseq=%ld\n", st, seq);
    return strstr(buf, want) != NULL;
}
static void regs_init(void) {
    nregs = 0; nwrites = 0; ph_writes = 0; ctl_stuck = 0; race_at = -1; race_addr = 0; race_addr_reads = 0; echo_addr = 0;
    reg_set(0x0300b0fc, 0x35513331); reg_set(0x05090000, 0x00060000);
}
static const char *BOOTF, *BOOTID;
static void boot_record(const char *pins, const char *fault, const char *ph) { // as hdmi_audio_boot leaves it
    char id[64] = ""; FILE *f = fopen(BOOTID, "r"); if (f) { if (fgets(id, sizeof(id), f)) id[strcspn(id, "\n")] = 0; fclose(f); }
    f = fopen(BOOTF, "w");
    fprintf(f, "version=1\nboot_id=%s\nphase=done\nsupported=1\naudio_out=1\naudio_in=0\npins=%s\nfault=%s\nph_cfg0=%s\ndetail=test\n", id, pins, fault, ph);
    fclose(f);
}
static void reset(void) { waits = recovers = opens = readis = nonzero_out = out_frames = getframes = 0; delivered = 0; clock_gettime(CLOCK_MONOTONIC, &t0); }
static int state_err(const char *e) {
    char buf[512] = ""; FILE *f = fopen(STATE, "r"); if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    return strstr(buf, e) != NULL;
}
int main(int argc, char **argv) {
    REQ = argv[1]; STATE = argv[2]; BOOTF = argv[3]; BOOTID = argv[4]; (void)argc;
    AiParams_t ai = {0, 0, 48000, 2, 16, 0};
    PcmBridge_t *pb = pcmbridge_initSys();
    CHECK(state_is("stopped", 0), "daemon start reports 'stopped'");

    /* H3: XRUN storm on the I2S2 capture (HDMI in: needs no boot record, writes no register) */
    unlink(BOOTF);
    regs_init(); reset(); cap_mode = 1;
    req(3, "hw:snddaudio2", "plughw:audiocodec", 0);
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("running", 3), "I2S2 route running, request 3 echoed");
    usleep(1000000);
    pcmbridge_stop(pb, 1);
    CHECK(waits > 0 && waits < 60 && recovers >= waits - 1, "capture XRUN for 1 s: %ld waits, %ld recovers (no busy loop)", waits, recovers);
    /* a capture that cannot be recovered: closed, reopened with backoff */
    reset(); cap_mode = 2;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    usleep(1500000);
    pcmbridge_stop(pb, 1);
    CHECK(opens >= 2 && opens < 15 && waits < 60, "unrecoverable capture for 1.5 s: %ld opens, %ld waits (reopened with backoff)", opens, waits);

    /* H2: 44.1 kHz source muted, 48 kHz played */
    reset(); cap_mode = 0; cap_rate = 44100;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    usleep(3000000);
    pcmbridge_stop(pb, 1);
    CHECK(nonzero_out == 0 && out_frames > 0, "44.1 kHz source: muted (%ld frames written, all silent)", out_frames);
    reset(); cap_rate = 48000;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    usleep(2500000);
    pcmbridge_stop(pb, 1);
    CHECK(nonzero_out > 0, "48 kHz source: played after the rate check (%ld non-silent blocks)", nonzero_out);
    CHECK(nwrites == 0, "I2S2 route: no SoC register written");
    ai.sampleRate = 44100;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 3), "I2S2 -> codec refused while the DVR captures at 44.1 kHz (one codec rate)");
    ai.sampleRate = 48000;

    /* AI -> I2S0 only while HDMI out audio is in effect for this boot */
    regs_init(); reset(); ai_fail_now = 0; ai_chans = 0;
    req(4, "ai", "hw:snddaudio0", 1);
    unlink(BOOTF);
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 4) && state_err("err=refused") && nwrites == 0 && ai_chans == 0, "no boot record: AI -> I2S0 refused, nothing opened or written");
    boot_record("audio", "HDMI chip check failed", "0x35513331");
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 4) && state_err("err=refused") && nwrites == 0, "boot record with a fault: refused");
    boot_record("stock", "pin check failed", "0x35517751");
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 4) && nwrites == 0, "boot record: pins stock (refused at boot): refused");
    req(4, "ai", "hw:snddaudio0", 0);
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 4) && nwrites == 0, "a request to hw:snddaudio0 without the I2S0 fix: refused too");

    /* O2 / O4: AI -> I2S0 with the I2S0 CTL fix, undone on stop; PH_CFG0 never written */
    req(4, "ai", "hw:snddaudio0", 1);
    boot_record("audio", "", "0x35513331");
    regs_init(); reset();
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("running", 4) && reg(0x05090000) == 0x00060010 && ph_writes == 0, "AI route: CTL I2S mode (CTL %08x), PH_CFG0 not written", reg(0x05090000));
    usleep(300000);
    pcmbridge_stop(pb, 1);
    CHECK(reg(0x05090000) == 0x00060000 && nwrites == 2 && ph_writes == 0 && reg(0x0300b0fc) == 0x35513331, "stop: CTL put back (%08x, %d writes), PH1-PH3 left as the boot step set them", reg(0x05090000), nwrites);
    CHECK(out_frames >= 1024 && ai_disables > 0 && !ai_enabled && ai_chans == 0, "AI frames forwarded (%ld frames: mLen = total bytes), channel destroyed, device disabled", out_frames);
    for (int i = 0; i < 20; i++) { pcmbridge_start(pb, "/nonexistent", &ai, 1); pcmbridge_stop(pb, 1); }
    CHECK(ph_writes == 0 && reg(0x05090000) == 0x00060000, "20 more starts and stops (HDMI out on/off, source switches): PH_CFG0 never written");
    /* an interleaved latch access during the CTL read: the start fails, nothing written */
    regs_init(); race_addr = 0x05090000; race_at = 1;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 4) && nwrites == 0 && ai_chans == 0, "interleaved CTL read: start failed, nothing written, channel destroyed");
    regs_init();
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("running", 4) && reg(0x05090000) == 0x00060010, "the retry runs");
    pcmbridge_stop(pb, 1);
    /* a latch that answers with the address (wrong register): nothing written */
    regs_init(); echo_addr = 1;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(nwrites == 0 && state_is("failed", 4) && state_err("err=refused"), "implausible CTL (reserved bits set): refused, no SoC write (%d)", nwrites);
    pcmbridge_stop(pb, 1);
    /* the CTL write does not read back: the old value written back, refused */
    regs_init(); ctl_stuck = 1;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 4) && state_err("err=refused") && reg(0x05090000) == 0x00060000 && ph_writes == 0, "CTL write not taken: old value written back, refused");
    /* the lock held by another process: failed after about 1 s, nothing written */
    regs_init();
    { int lk = open("LOCKPATH", O_RDWR | O_CREAT, 0644); flock(lk, LOCK_EX);
      struct timespec a0, a1; clock_gettime(CLOCK_MONOTONIC, &a0);
      pcmbridge_start(pb, "/nonexistent", &ai, 1);
      clock_gettime(CLOCK_MONOTONIC, &a1); close(lk);
      double dt = (a1.tv_sec - a0.tv_sec) + (a1.tv_nsec - a0.tv_nsec) / 1e9;
      CHECK(state_is("failed", 4) && nwrites == 0 && ai_chans == 0 && dt < 1.6, "sunxi_dump lock held: failed after %.2f s, nothing written", dt); }
    /* AI GetFrame failing at once: backoff */
    regs_init(); reset(); ai_fail_now = 1;
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    usleep(1000000);
    pcmbridge_stop(pb, 1);
    CHECK(getframes > 0 && getframes < 60, "AI GetFrame failing at once for 1 s: %ld calls (no busy loop)", getframes);
    ai_fail_now = 0;

    /* sink missing / busy: failed, AI device disabled only when nobody else uses it */
    regs_init(); boot_record("audio", "", "0x35513331"); sink_fail = 1; ai_enabled = 0; ai_disables = 0;
    req(5, "ai", "hw:snddaudio0", 1);
    pcmbridge_start(pb, "/nonexistent", &ai, 0);
    CHECK(state_is("failed", 5) && ai_enabled && ai_chans == 0 && nwrites == 0, "sink busy, DVR uses AI: failed, channel destroyed, device kept");
    pcmbridge_start(pb, "/nonexistent", &ai, 1);
    CHECK(state_is("failed", 5) && !ai_enabled, "sink busy, nobody else: AI device disabled");
    sink_fail = 0;
    pcmbridge_deinitSys(pb, 1);
    CHECK(state_is("stopped", 5), "daemon exit reports 'stopped'");
    CHECK(ph_writes == 0, "PH_CFG0 never written by the bridge in any of these runs");
    return fails ? 1 : 0;
}
C
cp "$SRC/src/record/pcmbridge.c" "$SRC/src/record/pcmbridge.h" "$SRC/src/record/pcmbridge_alsa.h" "$SRC/src/record/appmsg.h" \
    "$SRC/src/record/soc_reg.c" "$SRC/src/record/soc_reg.h" "$SRC/src/record/hdmi_boot.c" "$SRC/src/record/hdmi_boot.h" "$T/inc/"
sed -i "s|\"LOCKPATH\"|\"$T/lock\"|" "$T/harness.c"
echo "1b6d6f8a-3c2e-4f10-9a7b-0123456789ab" > "$T/boot_id"
# shellcheck disable=SC2089,SC2090 # the quotes are part of the macro values
D="-DHDMIA_REQ_FILE=\"$T/req\" -DHDMIA_STATE_FILE=\"$T/state\" -DHDMIA_LOCK_FILE=\"$T/lock\" -DHDMIA_BOOT_FILE=\"$T/boot\" -DHDMIA_BOOT_ID_FILE=\"$T/boot_id\""
W="-Dopen=t_open -Dread=t_read -Dwrite=t_write -Dclose=t_close -Dflock=t_flock"
gcc -w -c -I"$T/inc" -o "$T/minIni.o" "$T/inc/minIni.c"
# every target (the target decides whether the boot record counts: hdmi_boot_supported())
for v in HDZGOGGLE HDZGOGGLE2 HDZBOXPRO; do
    echo "== $v"
    # shellcheck disable=SC2086,SC2090 # D and W hold several options
    gcc -Wall -Wno-unused-parameter -c -I"$T/inc" $D $W -D$v -o "$T/pb.o" "$T/inc/pcmbridge.c"
    # shellcheck disable=SC2086,SC2090
    gcc -Wall -Wextra -Werror -c -I"$T/inc" $D $W -D$v -o "$T/sr.o" "$T/inc/soc_reg.c"
    # shellcheck disable=SC2086,SC2090
    gcc -Wall -Wextra -Werror -c -I"$T/inc" $D $W -D$v -o "$T/hb.o" "$T/inc/hdmi_boot.c"
    # shellcheck disable=SC2086,SC2090
    gcc -Wall -I"$T/inc" $D -D$v -o "$T/t" "$T/harness.c" "$T/pb.o" "$T/sr.o" "$T/hb.o" "$T/minIni.o" -lpthread
    rm -f "$T/req" "$T/state" "$T/boot" "$T/lock"
    timeout -k 5 "$TL" "$T/t" "$T/req" "$T/state" "$T/boot" "$T/boot_id" 2> "$T/log.txt" || {
        echo "--- $v (exit status $?: 124 = not ended within $TL s) bridge log:"
        tail -30 "$T/log.txt"
        exit 1
    }
done
echo "ALL PCMBRIDGE TESTS PASSED"
