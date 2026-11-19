/* ===========================================================================
 * PCM bridge for HDMI audio (EXPERIMENTAL, default off). See pcmbridge.h and
 * docs/hdmi-audio.md. Nothing in here runs unless the app sends HDMIA_cmdSTART,
 * which it only does when HDMI out / in audio is in effect for this boot
 * (setting.ini [hdmi] audio_out / audio_in = 1 at boot, src/record/hdmi_boot.h).
 * ======================================================================== */
#define LOG_TAG "pcmbridge"
#include <log/log.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <minIni.h>

#include "appmsg.h"
#include "hdmi_boot.h"
#include "pcmbridge.h"
#include "pcmbridge_alsa.h"
#include "soc_reg.h"

#define PB_SECTION      "hdmi_audio"
#define PB_NAME_LEN     64
#define PB_MAX_SINKS    2
#define PB_CHANNELS     2    // sinks are always opened stereo; mono sources are duplicated
#define PB_CHUNK_FRAMES 4096 // largest block handled in one go
#define PB_WAIT_MS      200
#define PB_RATE_TOL_PCT 3    // an ALSA capture source must deliver 48 kHz within 3 %, else muted

// I2S0 control register I2S/PCM_CTL (V536 datasheet 11.1.5): bit18 BCLK_OUT, bit17 LRCK_OUT,
// bits[5:4] MODE_SEL (0 PCM, 1 left/I2S); bits 31:19, 16:12, 7 reserved; DOUT1-3 (11:9) only on I2S1.
// Vendor note mkapp/app/app/record/audio/i2s/i2s-daudio-setup.txt: "must do: aww 0x05090000 0x00060010".
// A register of the I2S0 block alone (no other function shares it); the sunxi-daudio driver sets
// it up again at every open, so the fields are set after each hw_params and put back at the stop.
// The pin functions of PH1-PH3 are NOT touched here: hdmi_audio_boot switches them once per boot
// (src/record/hdmi_boot.h), and this bridge plays into I2S0 only if that is in effect.
#define PB_I2S0_CTL       0x05090000u
#define PB_I2S0_MODE_I2S  0x00000010u
#define PB_I2S0_MASTER    0x00060000u
#define PB_I2S0_FIELDS    0x00060030u // the fields this bridge changes (and puts back)
#define PB_I2S0_RESERVED  0xfff9fe80u

struct PcmBridge {
    pthread_t tid;
    volatile bool bExit;
    bool running;       // started and not yet stopped
    bool threadStarted; // tid is valid

    long seq; // request number (HDMIA_REQ_FILE), echoed in HDMIA_STATE_FILE
    char src[PB_NAME_LEN];
    char sink[PB_MAX_SINKS][PB_NAME_LEN];
    bool i2s0Fix;
    unsigned latencyUs;

    unsigned rate;
    unsigned srcChannels;

    bool usesAi;
    int aiDev;
    AI_CHN aiChn;
    AIO_ATTR_S aiAttr;

    snd_pcm_t *cap;
    snd_pcm_t *out[PB_MAX_SINKS];

    // what this bridge changed on the SoC (I2S0 CTL only); pb_i2s0_undo() puts back exactly these fields
    bool ctlSet;
    uint32_t ctlOrig, ctlNew;

    // ALSA capture source (I2S2, clocked by the HDMI source): reopen backoff, rate check, drift
    unsigned reopenMs;
    unsigned errRun; // consecutive errors: they back off, never spin
    bool muted;      // the source's rate is not (yet) known to be 48 kHz
    int rateGood, rateBad;
    uint64_t winStartUs;
    unsigned long long winFrames;
    long winAvail0;
    unsigned measuredRate;
    long delayAvg; // sink fill, smoothed (frames)

    int16_t buf[(PB_CHUNK_FRAMES + 1) * PB_CHANNELS];
    int16_t silence[PB_CHUNK_FRAMES * PB_CHANNELS];
    unsigned long long framesIn, framesDropped, framesAdjusted, xruns;
};

static uint64_t pb_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ---- the state the app reads back (HDMIA_STATE_FILE, written atomically) ---- */
static void pb_report(PcmBridge_t *pb, const char *state, const char *err) {
    const char *tmp = HDMIA_STATE_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fprintf(f, "[%s]\nstate=%s\nseq=%ld\npid=%d\nsrc=%s\nsink=%s\nerr=%s\n", PB_SECTION, state,
            pb ? pb->seq : 0L, (int)getpid(), pb ? pb->src : "", pb ? pb->sink[0] : "", err ? err : "");
    fclose(f);
    rename(tmp, HDMIA_STATE_FILE);
}

/* ---- I2S0 CTL through /sys/class/sunxi_dump (src/record/soc_reg.h: the shared lock, three
 * agreeing reads, verified writes). pb_i2s0_apply() returns NULL when the fields are set, else why
 * not; a reason starting with "refused" is not worth a retry in this boot. ---- */
static bool pb_ctl_sane(uint32_t v) {
    return (v & PB_I2S0_RESERVED) == 0 && ((v >> 4) & 3u) != 3u;
}

static const char *pb_i2s0_apply(PcmBridge_t *pb) {
    char diag[160], d2[160];
    uint32_t v = 0;
    const char *why = NULL;
    const int lk = soc_reg_lock();
    if (lk < 0) {
        LOGE("i2s0 fix: %s busy for 1 s, nothing written", HDMIA_LOCK_FILE);
        return "I2S0 CTL: sunxi_dump lock busy";
    }
    if (soc_reg_read(PB_I2S0_CTL, &v, diag, sizeof(diag)) < 0) {
        LOGE("i2s0 fix: I2S0 CTL read not trusted (%s), nothing written", diag);
        why = "I2S0 CTL read not trusted";
    } else if (!pb_ctl_sane(v)) {
        LOGE("i2s0 fix: I2S0 CTL 0x%08x: reserved bits set or an invalid mode, nothing written", v);
        why = "refused: I2S0 CTL not as the driver leaves it";
    } else {
        const uint32_t w = (v & ~PB_I2S0_FIELDS) | PB_I2S0_MODE_I2S | PB_I2S0_MASTER;
        if (w == v) {
            LOGD("i2s0 fix: CTL 0x%08x is I2S mode and clock master already", v);
        } else if (soc_reg_write(PB_I2S0_CTL, w, diag, sizeof(diag)) == 0) {
            pb->ctlOrig = v;
            pb->ctlNew = w;
            pb->ctlSet = true;
            LOGD("i2s0 fix: CTL 0x%08x -> 0x%08x", v, w);
        } else { // it did not read back: the value it had, verified
            LOGE("i2s0 fix: CTL 0x%08x did not read back (%s)", w, diag);
            if (soc_reg_write(PB_I2S0_CTL, v, d2, sizeof(d2)) == 0)
                LOGE("i2s0 fix: CTL 0x%08x written back, verified", v);
            else
                LOGE("i2s0 fix: CTL 0x%08x written back, not verified (%s)", v, d2);
            why = "refused: I2S0 CTL did not read back";
        }
    }
    soc_reg_unlock(lk);
    return why;
}

// put back exactly the fields pb_i2s0_apply() changed, if they still hold what it wrote
static void pb_i2s0_undo(PcmBridge_t *pb) {
    char diag[160];
    uint32_t v = 0;
    if (!pb->ctlSet)
        return;
    pb->ctlSet = false;
    const int lk = soc_reg_lock();
    if (lk < 0) {
        LOGE("i2s0 undo: %s busy: I2S0 CTL not restored (I2S0 is idle; the next start sets it again)", HDMIA_LOCK_FILE);
        return;
    }
    if (soc_reg_read(PB_I2S0_CTL, &v, diag, sizeof(diag)) == 0 && pb_ctl_sane(v) &&
        (v & PB_I2S0_FIELDS) == (pb->ctlNew & PB_I2S0_FIELDS)) {
        const uint32_t w = (v & ~PB_I2S0_FIELDS) | (pb->ctlOrig & PB_I2S0_FIELDS);
        if (soc_reg_write(PB_I2S0_CTL, w, diag, sizeof(diag)) == 0)
            LOGD("i2s0 undo: CTL 0x%08x -> 0x%08x", v, w);
        else
            LOGE("i2s0 undo: CTL 0x%08x did not read back (%s)", w, diag);
    } else {
        LOGE("i2s0 undo: I2S0 CTL not as set (%s): left alone", diag);
    }
    soc_reg_unlock(lk);
}

/* ---- ALSA helpers ---- */
static snd_pcm_t *pb_open(const char *name, snd_pcm_stream_t dir, unsigned channels, unsigned rate, unsigned latencyUs) {
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, name, dir, SND_PCM_NONBLOCK);
    if (err < 0) {
        LOGE("open %s %s failed: %s", dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", name, snd_strerror(err));
        return NULL;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate, 1, latencyUs);
    if (err < 0) {
        LOGE("%s: S16_LE %uch %uHz not accepted: %s", name, channels, rate, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }
    LOGD("opened %s (%s, %uch, %uHz)", name, dir == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", channels, rate);
    return pcm;
}

static void pb_write_sinks(PcmBridge_t *pb, const int16_t *data, unsigned frames) {
    for (int i = 0; i < PB_MAX_SINKS; i++) {
        if (pb->out[i] == NULL)
            continue;
        snd_pcm_sframes_t r = snd_pcm_writei(pb->out[i], data, frames);
        if (r == -EPIPE || r == -ESTRPIPE || r == -EBADFD) {
            snd_pcm_recover(pb->out[i], r == -EBADFD ? -EPIPE : (int)r, 1);
            if (snd_pcm_state(pb->out[i]) != SND_PCM_STATE_PREPARED)
                snd_pcm_prepare(pb->out[i]);
            r = snd_pcm_writei(pb->out[i], data, frames);
        }
        if (r < 0)
            pb->framesDropped += frames; // -EAGAIN: sink not running (no clock) or buffer full
        else if ((unsigned)r < frames)
            pb->framesDropped += frames - (unsigned)r;
    }
}

static void pb_write_silence(PcmBridge_t *pb, long frames) {
    while (frames > 0) {
        unsigned n = frames > PB_CHUNK_FRAMES ? PB_CHUNK_FRAMES : (unsigned)frames;
        pb_write_sinks(pb, pb->silence, n);
        frames -= n;
    }
}

// Stereo-ize (mono source) into pb->buf and forward.
static void pb_forward(PcmBridge_t *pb, const int16_t *data, unsigned frames) {
    if (pb->srcChannels == PB_CHANNELS) {
        pb_write_sinks(pb, data, frames);
        return;
    }
    while (frames > 0) {
        unsigned n = frames > PB_CHUNK_FRAMES ? PB_CHUNK_FRAMES : frames;
        for (unsigned k = 0; k < n; k++)
            pb->buf[2 * k] = pb->buf[2 * k + 1] = data[k];
        pb_write_sinks(pb, pb->buf, n);
        data += n;
        frames -= n;
    }
}

/* Incoming rate, from the frames that arrived per second of CLOCK_MONOTONIC. For the I2S2 path
 * (a clock slave: the HDMI source sets the rate) a rate other than 48 kHz mutes the output
 * instead of playing it at the wrong speed; the output starts muted until one second at
 * 48 kHz was seen. For the AI path it only checks the frame-length interpretation. */
static void pb_rate_update(PcmBridge_t *pb, unsigned frames, bool canMute) {
    uint64_t now = pb_now_us();
    long avail = 0;
    if (pb->cap != NULL) {
        snd_pcm_sframes_t a = snd_pcm_avail(pb->cap);
        avail = a > 0 ? (long)a : 0;
    }
    pb->winFrames += frames;
    if (pb->winStartUs == 0) {
        pb->winStartUs = now;
        pb->winFrames = 0;
        pb->winAvail0 = avail;
        return;
    }
    if (now - pb->winStartUs < 1000000u)
        return;
    long long arrived = (long long)pb->winFrames + avail - pb->winAvail0;
    unsigned r = (unsigned)(arrived * 1000000LL / (long long)(now - pb->winStartUs));
    bool first = pb->measuredRate == 0;
    pb->measuredRate = r;
    pb->winStartUs = now;
    pb->winFrames = 0;
    pb->winAvail0 = avail;
    bool ok = r * 100u > pb->rate * (100u - PB_RATE_TOL_PCT) && r * 100u < pb->rate * (100u + PB_RATE_TOL_PCT);
    if (first)
        LOGD("input delivers about %u frames/s (expected %u)%s", r, pb->rate,
             ok ? "" : (pb->usesAi ? ": AI frame length not as assumed (mLen), see docs/hdmi-audio.md" : ""));
    if (!canMute)
        return;
    if (ok) {
        pb->rateBad = 0;
        if (++pb->rateGood >= 1 && pb->muted) {
            pb->muted = false;
            LOGI("HDMI in audio at %u Hz: unmuted", r);
        }
    } else {
        pb->rateGood = 0;
        if (++pb->rateBad >= 2 && !pb->muted) {
            pb->muted = true;
            LOGE("HDMI in sends about %u Hz, not %u: muted (the EDID offers 48 kHz only)", r, pb->rate);
        }
    }
}

/* Clock drift between the HDMI source (I2S2 clock) and the codec DAC (PLL_AUDIO): keep the sink
 * buffer half full by dropping or repeating one frame per block (at most ~1 frame per 15 ms, i.e.
 * about 1400 ppm, far more than two crystal clocks differ). While muted the buffer is set to the
 * target in one step. Returns the new frame count of pb->buf (which has room for one more). */
static unsigned pb_drift(PcmBridge_t *pb, unsigned n) {
    snd_pcm_sframes_t d = 0;
    const long target = (long)((unsigned long long)pb->rate * pb->latencyUs / 2000000u);
    const long hyst = target / 4;
    if (pb->out[0] == NULL || n < 2 || target <= 0 || snd_pcm_delay(pb->out[0], &d) < 0)
        return n;
    if (pb->delayAvg < 0)
        pb->delayAvg = d;
    else
        pb->delayAvg += ((long)d - pb->delayAvg) / 16;
    if (d < target / 4) { // start or after an underrun: refill with silence (one gap, no slow creep)
        pb_write_silence(pb, target - d);
        pb->delayAvg = target;
        return n;
    }
    if (pb->muted && d > target + 2 * hyst) { // muted: skip the excess at once
        long skip = (long)d - target;
        pb->delayAvg = target;
        return skip >= (long)n ? 0 : n - (unsigned)skip;
    }
    if (pb->delayAvg > target + hyst) { // source faster: drop the last frame
        pb->framesAdjusted++;
        pb->delayAvg--;
        return n - 1;
    }
    if (pb->delayAvg < target - hyst) { // source slower: repeat the last frame
        pb->buf[2 * n] = pb->buf[2 * n - 2];
        pb->buf[2 * n + 1] = pb->buf[2 * n - 1];
        pb->framesAdjusted++;
        pb->delayAvg++;
        return n + 1;
    }
    return n;
}

static void pb_backoff(PcmBridge_t *pb) { // consecutive failures sleep, longer each time
    pb->errRun++;
    unsigned ms = pb->errRun > 8 ? 500u : 2u << pb->errRun;
    usleep(ms * 1000u);
}

// capture error (XRUN = -EPIPE, suspend, ...): recover and restart; if that fails, close and reopen later
static void pb_cap_error(PcmBridge_t *pb, int err) {
    pb->xruns++;
    int r = snd_pcm_recover(pb->cap, err, 1);
    if (r == 0 && snd_pcm_state(pb->cap) == SND_PCM_STATE_PREPARED)
        r = snd_pcm_start(pb->cap); // a capture stream needs an explicit start after prepare
    if (r < 0) {
        LOGE("capture %s: %s (recover: %s), reopening", pb->src, snd_strerror(err), snd_strerror(r));
        snd_pcm_close(pb->cap);
        pb->cap = NULL;
        pb->reopenMs = 100;
    }
    pb->winStartUs = 0; // the rate window restarts after a gap
    pb_backoff(pb);
}

static void pb_cap_step(PcmBridge_t *pb) {
    if (pb->cap == NULL) {
        usleep(pb->reopenMs * 1000u);
        pb->reopenMs = pb->reopenMs >= 1000 ? 2000 : pb->reopenMs * 2;
        pb->cap = pb_open(pb->src, SND_PCM_STREAM_CAPTURE, PB_CHANNELS, pb->rate, pb->latencyUs);
        if (pb->cap != NULL) {
            snd_pcm_start(pb->cap);
            pb->winStartUs = 0;
        }
        return;
    }
    int w = snd_pcm_wait(pb->cap, PB_WAIT_MS);
    if (w == 0) { // timeout: no clock from the I2S master (no HDMI audio)
        pb->winStartUs = 0;
        return;
    }
    if (w < 0) { // -EPIPE right away while in XRUN: must recover here, or the loop would spin
        pb_cap_error(pb, w);
        return;
    }
    snd_pcm_sframes_t n = snd_pcm_readi(pb->cap, pb->buf, PB_CHUNK_FRAMES / 2);
    if (n == -EAGAIN || n == 0) { // woke up without data: never spin
        pb_backoff(pb);
        return;
    }
    if (n < 0) {
        pb_cap_error(pb, (int)n);
        return;
    }
    pb->errRun = 0;
    pb->framesIn += (unsigned)n;
    pb_rate_update(pb, (unsigned)n, true);
    if (pb->muted)
        memset(pb->buf, 0, (size_t)n * PB_CHANNELS * sizeof(int16_t));
    unsigned m = pb_drift(pb, (unsigned)n);
    if (m > 0)
        pb_write_sinks(pb, pb->buf, m);
}

static void pb_ai_step(PcmBridge_t *pb, bool *firstFrame) {
    AUDIO_FRAME_S frm;
    uint64_t t0 = pb_now_us();
    if (AW_MPI_AI_GetFrame(pb->aiDev, pb->aiChn, &frm, NULL, PB_WAIT_MS) != SUCCESS) {
        if (pb_now_us() - t0 < 20000u) // failed at once (not a timeout): back off, never spin
            pb_backoff(pb);
        return;
    }
    pb->errRun = 0;
    /* mLen is the byte count of the whole interleaved frame, all channels, although
     * mm_comm_aio.h calls it "data lenth per channel": libmedia_mpp's audioHw_AI_CapThread
     * (0x2c54c) sets mLen to the device's chunk bytes = chunk frames * channels * physical
     * width / 8 (alsaSetPcmParams, 0x2ad40-0x2ad88: the size of the buffer one snd_pcm_readi
     * fills), and AIChannel_EmptyThisBuffer (0x1e8d4) memcpy()s mLen bytes. The rate check
     * below logs it on the device ("input delivers about N frames/s"). */
    unsigned frames = frm.mLen / (2u * pb->srcChannels);
    if (*firstFrame) {
        LOGD("first AI frame: len=%u bytes, bitwidth=%d, soundmode=%d -> %u frames",
             frm.mLen, (int)frm.mBitwidth, (int)frm.mSoundmode, frames);
        *firstFrame = false;
    }
    pb->framesIn += frames;
    pb_rate_update(pb, frames, false);
    pb_forward(pb, (const int16_t *)frm.mpAddr, frames);
    AW_MPI_AI_ReleaseFrame(pb->aiDev, pb->aiChn, &frm, NULL);
}

static void *pb_thread(void *arg) {
    PcmBridge_t *pb = (PcmBridge_t *)arg;
    bool firstFrame = true;
    unsigned long long lastLog = 0;

    LOGD("bridge thread: %s -> %s %s", pb->src, pb->sink[0], pb->sink[1]);
    while (!pb->bExit) {
        if (pb->usesAi)
            pb_ai_step(pb, &firstFrame);
        else
            pb_cap_step(pb);
        if (pb->framesIn - lastLog >= 10ULL * pb->rate) { // every ~10 s of audio
            LOGD("bridge: %llu frames in (~%u/s), %llu dropped, %llu drift-adjusted, %llu xruns%s",
                 pb->framesIn, pb->measuredRate, pb->framesDropped, pb->framesAdjusted, pb->xruns,
                 pb->muted ? ", muted" : "");
            lastLog = pb->framesIn;
        }
    }
    LOGD("bridge thread exit: %llu frames in, %llu dropped, %llu adjusted", pb->framesIn, pb->framesDropped, pb->framesAdjusted);
    return NULL;
}

/* ---- MPP AI channel (shares AI device 0 with ai2aenc / ai2ao) ---- */
static int pb_ai_open(PcmBridge_t *pb, AiParams_t *ai) {
    memset(&pb->aiAttr, 0, sizeof(pb->aiAttr));
    pb->aiAttr.u32ChnCnt = ai->channels;
    pb->aiAttr.enSamplerate = (AUDIO_SAMPLE_RATE_E)ai->sampleRate;
    pb->aiAttr.enBitwidth = (AUDIO_BIT_WIDTH_E)(ai->bitsPerSample / 8 - 1);
    pb->aiDev = ai->aiDev;
    AW_MPI_AI_SetPubAttr(pb->aiDev, &pb->aiAttr); // fails harmlessly if the DVR already enabled the device
    AW_MPI_AI_Enable(pb->aiDev);

    for (AI_CHN chn = 0; chn < AIO_MAX_CHN_NUM; chn++) {
        ERRORTYPE ret = AW_MPI_AI_CreateChn(pb->aiDev, chn);
        if (ret == SUCCESS) {
            pb->aiChn = chn;
            if (AW_MPI_AI_EnableChn(pb->aiDev, chn) != SUCCESS) {
                LOGE("AI chn %d enable failed", chn);
                AW_MPI_AI_DestroyChn(pb->aiDev, chn);
                pb->aiChn = MM_INVALID_CHN;
                return -1;
            }
            LOGD("AI dev %d chn %d for HDMI audio", pb->aiDev, chn);
            return 0;
        }
        if (ret != ERR_AI_EXIST)
            LOGD("AI chn %d create ret 0x%x, trying next", chn, ret);
    }
    LOGE("no free AI channel");
    return -1;
}

static void pb_ai_close(PcmBridge_t *pb, bool disableAiDev) {
    if (pb->aiChn != MM_INVALID_CHN) {
        AW_MPI_AI_DisableChn(pb->aiDev, pb->aiChn);
        AW_MPI_AI_ResetChn(pb->aiDev, pb->aiChn);
        AW_MPI_AI_DestroyChn(pb->aiDev, pb->aiChn);
        pb->aiChn = MM_INVALID_CHN;
    }
    if (disableAiDev) // nobody else (DVR, ai2ao) uses it: also after a start that failed half-way
        AW_MPI_AI_Disable(pb->aiDev);
}

/* ---- public API ---- */
PcmBridge_t *pcmbridge_initSys(void) {
    PcmBridge_t *pb = (PcmBridge_t *)calloc(1, sizeof(PcmBridge_t));
    if (pb != NULL)
        pb->aiChn = MM_INVALID_CHN;
    pb_report(pb, "stopped", "record daemon started"); // the app re-sends its request when it sees this
    return pb;
}

static void pb_close_all(PcmBridge_t *pb, bool disableAiDev) {
    for (int i = 0; i < PB_MAX_SINKS; i++) {
        if (pb->out[i]) {
            snd_pcm_drop(pb->out[i]);
            snd_pcm_close(pb->out[i]);
            pb->out[i] = NULL;
        }
    }
    if (pb->cap) {
        snd_pcm_close(pb->cap);
        pb->cap = NULL;
    }
    if (pb->usesAi)
        pb_ai_close(pb, disableAiDev);
    pb->usesAi = false;
    pb_i2s0_undo(pb); // I2S0 CTL fields as they were before the start (pin functions: never touched)
}

static void pb_stop(PcmBridge_t *pb, bool disableAiDev) {
    if (!pb->running)
        return;
    pb->bExit = true;
    if (pb->threadStarted)
        pthread_join(pb->tid, NULL);
    pb->threadStarted = false;
    pb_close_all(pb, disableAiDev);
    pb->running = false;
    LOGD("stopped");
}

void pcmbridge_stop(PcmBridge_t *pb, bool disableAiDev) {
    if (pb == NULL)
        return;
    pb_stop(pb, disableAiDev);
    pb->seq = ini_getl(PB_SECTION, "seq", pb->seq, HDMIA_REQ_FILE);
    pb_report(pb, "stopped", "");
}

void pcmbridge_deinitSys(PcmBridge_t *pb, bool disableAiDev) {
    if (pb == NULL)
        return;
    pb_stop(pb, disableAiDev);
    pb_report(pb, "stopped", "record daemon exited");
    free(pb);
}

bool pcmbridge_usesAi(PcmBridge_t *pb) {
    return pb != NULL && pb->running && pb->usesAi;
}

static int pb_fail(PcmBridge_t *pb, bool disableAiDev, const char *why) {
    LOGE("start failed: %s", why);
    pb_close_all(pb, disableAiDev);
    pb_report(pb, "failed", why);
    return -1;
}

int pcmbridge_start(PcmBridge_t *pb, const char *confFile, AiParams_t *aiParams, bool aiDevFree) {
    if (pb == NULL)
        return -1;
    pb_stop(pb, aiDevFree); // a running route is replaced

    // the request the app wrote (RAM); latency_ms stays a user setting in record.conf
    pb->seq = ini_getl(PB_SECTION, "seq", 0, HDMIA_REQ_FILE);
    if (ini_getl(PB_SECTION, "active", 0, HDMIA_REQ_FILE) == 0) {
        pb_report(pb, "stopped", "");
        return 0;
    }
    ini_gets(PB_SECTION, "src", "ai", pb->src, PB_NAME_LEN, HDMIA_REQ_FILE);
    ini_gets(PB_SECTION, "sink", "hw:snddaudio0", pb->sink[0], PB_NAME_LEN, HDMIA_REQ_FILE);
    ini_gets(PB_SECTION, "sink2", "", pb->sink[1], PB_NAME_LEN, HDMIA_REQ_FILE);
    pb->i2s0Fix = ini_getl(PB_SECTION, "i2s0_fix", 0, HDMIA_REQ_FILE) != 0;
    pb->latencyUs = (unsigned)ini_getl(PB_SECTION, "latency_ms", 60, confFile) * 1000u;
    pb->rate = aiParams->sampleRate > 0 ? (unsigned)aiParams->sampleRate : 48000u;
    if (strcmp(pb->src, "ai") != 0)
        pb->rate = 48000u; // the codec DAC and the EDID: 48 kHz
    pb->usesAi = strcmp(pb->src, "ai") == 0;
    pb->framesIn = pb->framesDropped = pb->framesAdjusted = pb->xruns = 0;
    pb->errRun = 0;
    pb->reopenMs = 100;
    pb->winStartUs = 0;
    pb->measuredRate = 0;
    pb->rateGood = pb->rateBad = 0;
    pb->delayAvg = -1;
    pb->muted = !pb->usesAi; // an I2S2 source plays only once its rate was seen to be 48 kHz

    LOGD("start #%ld: src=%s sink=%s sink2=%s i2s0_fix=%d", pb->seq, pb->src, pb->sink[0], pb->sink[1], pb->i2s0Fix);

    // I2S0 (HDMI out) only while HDMI out audio is in effect for this boot: PH1-PH3 switched by
    // hdmi_audio_boot and no fault since (src/record/hdmi_boot.h)
    if ((pb->i2s0Fix || strstr(pb->sink[0], "snddaudio0") != NULL || strstr(pb->sink[1], "snddaudio0") != NULL) &&
        !hdmi_boot_out_ready()) {
        pb->usesAi = false;
        LOGE("start #%ld refused: HDMI out audio is not in effect this boot (%s)", pb->seq, HDMIA_BOOT_FILE);
        pb_report(pb, "failed", "refused: HDMI out audio is not in effect this boot");
        return -1;
    }

    if (pb->usesAi) {
        pb->srcChannels = (aiParams->channels == 1) ? 1 : 2;
        if (aiParams->bitsPerSample != 16) {
            pb->usesAi = false;
            pb_report(pb, "failed", "AI is not 16 bit");
            return -1;
        }
        if (pb_ai_open(pb, aiParams) < 0)
            return pb_fail(pb, aiDevFree, "no AI channel");
    } else {
        pb->srcChannels = PB_CHANNELS;
        // playback and capture of the codec share one sample-rate setting (AIF1_FS): playing at
        // 48 kHz into it while the DVR captures at another rate would change the DVR's rate
        if (strstr(pb->sink[0], "audiocodec") != NULL && aiParams->sampleRate > 0 && aiParams->sampleRate != 48000) {
            pb_report(pb, "failed", "record.conf [ai] sample_rate is not 48000");
            return -1;
        }
        pb->cap = pb_open(pb->src, SND_PCM_STREAM_CAPTURE, PB_CHANNELS, pb->rate, pb->latencyUs);
        if (pb->cap == NULL)
            return pb_fail(pb, aiDevFree, "capture device did not open");
        snd_pcm_start(pb->cap);
    }

    int nSinks = 0;
    for (int i = 0; i < PB_MAX_SINKS; i++) {
        if (pb->sink[i][0] == 0)
            continue;
        pb->out[i] = pb_open(pb->sink[i], SND_PCM_STREAM_PLAYBACK, PB_CHANNELS, pb->rate, pb->latencyUs);
        if (pb->out[i] == NULL)
            continue;
        nSinks++;
        if (pb->i2s0Fix && strstr(pb->sink[i], "snddaudio0") != NULL) {
            const char *why = pb_i2s0_apply(pb); // after hw_params, before the first write starts the stream
            if (why != NULL)
                return pb_fail(pb, aiDevFree, why);
        }
    }

    if (nSinks == 0)
        return pb_fail(pb, aiDevFree, "no sink could be opened (missing or busy)");

    pb->bExit = false;
    if (pthread_create(&pb->tid, NULL, pb_thread, pb) != 0)
        return pb_fail(pb, aiDevFree, "thread create failed");
    pb->threadStarted = true;
    pb->running = true;
    pb_report(pb, "running", "");
    return 0;
}
/* ===== end PCM bridge ===== */
