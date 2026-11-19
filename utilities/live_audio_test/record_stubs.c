// Host stand-in for the layers below src/record/main.c (MPP VI/VENC/AI/AENC/AO, the ffmpeg
// muxer, the SD card, the log), so that the real record main loop - live_run(), the AI device
// users, vvLive_onFrame(), record start/stop, the live audio chain (live_audio.c) and the ring
// writer (avshare.c) - runs on a PC, driven by goggle messages (gmsg) and fed in real time from
// media files.
//
// Media (directory $LAT_MEDIA, see gen_media.sh): video.h264 / video.h265 (Annex B, 30 fps) and
// audio.aac (ADTS, 1024 samples per frame), both looped every LAT_LOOP_S (32) seconds. Both are
// cut from one timeline: video frame n is "captured" at T0 + n/30 s and AAC frame k at
// T0 + k*1024/rate (CLOCK_MONOTONIC), like MPP stamps them.
//
// Knobs (environment):
//   LAT_AUDIO_JITTER_US  each AAC frame is delivered and stamped 0..J us late (MPP stamps PCM
//                        chunks when its capture thread reads them)            default 0
//   LAT_VIDEO_JITTER_US  each video frame is delivered 0..J us late (pts stays the capture time)
//   LAT_VIDEO_CLOCK      "mono": video pts on CLOCK_MONOTONIC; "foreign": pts on another clock
//                        (capture - T0 + 7 days), as if the V4L2 timestamps were not monotonic
//   LAT_TRAILING_SEI     1: every video frame ends with an SEI NAL unit (H.265: suffix SEI)
//   LAT_VIDEO_FROM_START 1: each start of an encoder plays the video from its first frame (the
//                        same frames for every run, to compare two rtspLive builds)
//   LAT_CTRL             directory: if "$LAT_CTRL/ai_fail" exists, AI chains stop producing and
//                        ai2aenc_start() fails (an audio chain failing for good); the HDMI audio
//                        bridge's route comes from "$LAT_CTRL/hdmia_src" and "$LAT_CTRL/hdmia_fail"
//                        makes its start fail (see the bridge below)
// Every AI device misuse (device disabled while another channel still uses it, device left
// enabled with no channel, also at exit) is logged as "AIDEV VIOLATION" / "AIDEV LEAK".
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ai2aenc.h"
#include "ai2ao.h"
#include "pcmbridge.h"
#include "disk.h"
#include "ffpack.h"
#include "jpegenc.h"
#include "log.h"
#include <log/log.h>
#include "vi2venc.h"
#include "vi2venc_live.h"

#define VIDEO_FPS 30

//----------------------------------------------------------------------------------------------
// clock, log
//----------------------------------------------------------------------------------------------
static uint64_t gT0;
static pthread_mutex_t gLogLock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void sleep_until(uint64_t t) {
    uint64_t n = now_us();
    if (t > n) usleep((useconds_t)(t - n));
}

static long env_long(const char *name, long def) {
    const char *v = getenv(name);
    return (v && *v) ? strtol(v, NULL, 0) : def;
}

static void stub_log(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&gLogLock);
    printf("%9.3f [stub] ", (now_us() - gT0) / 1e6);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    pthread_mutex_unlock(&gLogLock);
}

// ripples' recorder writes its own log lines too (src/record/log.c): into the stub log
int log_write(LOGLEVEL loglevel, char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    stub_log("log_write(%d): %s", (int)loglevel, buf);
    return 0;
}

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    va_list ap;
    (void)file;
    (void)line;
    (void)level;
    pthread_mutex_lock(&gLogLock);
    printf("%9.3f [rec %s] ", (now_us() - gT0) / 1e6, func);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    pthread_mutex_unlock(&gLogLock);
    return 0;
}

__attribute__((constructor)) static void stub_init(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    gT0 = now_us();
    // the log's time 0 on CLOCK_MONOTONIC: a test can put an event of its own on the log's clock (t_kill.sh)
    stub_log("log clock: T0 %llu us CLOCK_MONOTONIC", (unsigned long long)gT0);
}

int log_open(void) { return 0; }
void log_close(void) {}

static bool ctrl_flag(const char *name) {
    const char *dir = getenv("LAT_CTRL");
    char path[512];
    struct stat st;
    if (!dir) return false;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return stat(path, &st) == 0;
}

//----------------------------------------------------------------------------------------------
// media
//----------------------------------------------------------------------------------------------
typedef struct { uint8_t *p; int len; int idr; } Au_t;
typedef struct {
    bool loaded;
    Au_t *au;
    int nbAu;
    uint8_t hdr[1024]; // first VPS/SPS/PPS: what vi2venc_getSpsPpsInfo() returns
    int hdrLen;
} Video_t;
static Video_t gVideo[2]; // [0] H.264, [1] H.265
static pthread_mutex_t gMediaLock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { uint8_t *p; int len; } Chunk_t;
static Chunk_t *gAudio;
static int gNbAudio;
static int gAudioRate = 48000;

static uint8_t *load(const char *name, size_t *len) {
    const char *dir = getenv("LAT_MEDIA");
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir ? dir : ".", name);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len + 64);
    if (fread(b, 1, *len, f) != *len) { perror("read"); exit(1); }
    fclose(f);
    return b;
}

// access units: the VCL NAL units (slices) of one picture with the SEI units in front of them;
// parameter sets go to the header (the encoder reports them apart, see vvLive_onFrame())
static void load_video(int h265) {
    pthread_mutex_lock(&gMediaLock);
    Video_t *v = &gVideo[h265];
    if (v->loaded) { pthread_mutex_unlock(&gMediaLock); return; }
    size_t len;
    uint8_t *b = load(h265 ? "video.h265" : "video.h264", &len);
    int cap = 1 << 14, n = 0;
    size_t *start = malloc(cap * sizeof(size_t));
    for (size_t i = 0; i + 3 < len; i++) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            size_t s = (i > 0 && b[i - 1] == 0) ? i - 1 : i;
            if (n == cap) { cap *= 2; start = realloc(start, cap * sizeof(size_t)); }
            start[n++] = s;
            i += 2;
        }
    }
    v->au = calloc(n + 1, sizeof(Au_t));
    bool seen[64] = {false};
    size_t pending = (size_t)-1; // first SEI waiting for its VCL unit
    for (int j = 0; j < n; j++) {
        size_t s = start[j], e = (j + 1 < n) ? start[j + 1] : len;
        size_t h = s + ((b[s + 2] == 1) ? 3 : 4);
        int type = h265 ? ((b[h] >> 1) & 0x3F) : (b[h] & 0x1F);
        bool param = h265 ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
        bool vcl = h265 ? (type < 32) : (type >= 1 && type <= 5);
        bool sei = h265 ? (type == 39) : (type == 6);
        if (param) {
            if (!seen[type] && v->hdrLen + (int)(e - s) <= (int)sizeof(v->hdr)) {
                memcpy(v->hdr + v->hdrLen, b + s, e - s);
                v->hdrLen += (int)(e - s);
            }
            seen[type] = true;
        } else if (sei) {
            if (pending == (size_t)-1) pending = s;
        } else if (vcl) {
            // first slice of a picture: first_mb_in_slice == 0 (ue(v) "1") / first_slice_segment_in_pic_flag
            bool first = h265 ? ((b[h + 2] & 0x80) != 0) : ((b[h + 1] & 0x80) != 0);
            if (first || v->nbAu == 0) {
                size_t from = (pending != (size_t)-1) ? pending : s;
                v->au[v->nbAu].p = b + from;
                v->au[v->nbAu].len = (int)(e - from);
                v->au[v->nbAu].idr = h265 ? (type >= 16 && type <= 23) : (type == 5);
                v->nbAu++;
            } else {
                v->au[v->nbAu - 1].len = (int)(e - (size_t)(v->au[v->nbAu - 1].p - b)); // next slice of the same picture
            }
            pending = (size_t)-1;
        }
    }
    free(start);
    v->loaded = true;
    stub_log("video %s: %d frames, parameter sets %d bytes", h265 ? "H.265" : "H.264", v->nbAu, v->hdrLen);
    pthread_mutex_unlock(&gMediaLock);
}

static void load_audio(void) {
    pthread_mutex_lock(&gMediaLock);
    if (gAudio == NULL) {
        size_t len;
        uint8_t *b = load("audio.aac", &len);
        gAudio = calloc(len / 7 + 1, sizeof(Chunk_t));
        size_t pos = 0;
        while (pos + 7 <= len && b[pos] == 0xFF && (b[pos + 1] & 0xF0) == 0xF0) {
            int fl = ((b[pos + 3] & 3) << 11) | (b[pos + 4] << 3) | (b[pos + 5] >> 5);
            if (fl < 7 || pos + fl > len) break;
            gAudio[gNbAudio].p = b + pos;
            gAudio[gNbAudio].len = fl;
            gNbAudio++;
            pos += fl;
        }
        static const int rates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        if (gNbAudio > 0) gAudioRate = rates[(gAudio[0].p[2] >> 2) & 0x0F];
        stub_log("audio: %d AAC frames, %d Hz", gNbAudio, gAudioRate);
    }
    pthread_mutex_unlock(&gMediaLock);
}

//----------------------------------------------------------------------------------------------
// AI device 0 (channel list, like the MPP AI device)
//----------------------------------------------------------------------------------------------
static pthread_mutex_t gAiLock = PTHREAD_MUTEX_INITIALIZER;
static int gAiChannels = 0;
static bool gAiEnabled = false;

static void aidev_open(const char *who) {
    pthread_mutex_lock(&gAiLock);
    gAiChannels++;
    gAiEnabled = true;
    stub_log("AIDEV %s: channel created (%d in use)", who, gAiChannels);
    pthread_mutex_unlock(&gAiLock);
}

// at exit (after main() returns): no channel may be left, nor the device enabled
__attribute__((destructor)) static void aidev_atExit(void) {
    pthread_mutex_lock(&gAiLock);
    if (gAiChannels != 0 || gAiEnabled) {
        stub_log("AIDEV LEAK at exit: %d channel(s), device %s", gAiChannels, gAiEnabled ? "enabled" : "disabled");
    } else {
        stub_log("AIDEV at exit: no channel, device disabled");
    }
    pthread_mutex_unlock(&gAiLock);
}

static void aidev_close(const char *who, bool disableDev) {
    pthread_mutex_lock(&gAiLock);
    gAiChannels--;
    if (disableDev) {
        if (gAiChannels > 0) {
            stub_log("AIDEV VIOLATION %s: device disabled while %d other channel(s) use it", who, gAiChannels);
        }
        gAiEnabled = false;
    } else if (gAiChannels == 0) {
        stub_log("AIDEV LEAK %s: last channel gone, device left enabled", who);
    }
    stub_log("AIDEV %s: channel destroyed (%d in use, disable=%d, device %s)", who, gAiChannels, disableDev,
             gAiEnabled ? "on" : "off");
    pthread_mutex_unlock(&gAiLock);
}

//----------------------------------------------------------------------------------------------
// AI -> AENC
//----------------------------------------------------------------------------------------------
static void *aenc_thread(void *arg) {
    Ai2Aenc_t *aa = arg;
    double frameUs = 1024.0 * 1e6 / gAudioRate;
    long jitter = env_long("LAT_AUDIO_JITTER_US", 0);
    long encLatency = env_long("LAT_AUDIO_LATENCY_US", 30000);
    unsigned seed = (unsigned)(uintptr_t)aa;
    int loop = (int)(env_long("LAT_LOOP_S", 32) * gAudioRate / 1024); // the media timeline repeats
    if (loop > gNbAudio) loop = gNbAudio;
    int k = (int)((now_us() - gT0) / frameUs) + 1;
    while (!aa->bExit) {
        uint64_t capture = gT0 + (uint64_t)(k * frameUs);
        uint64_t late = jitter ? (uint64_t)(rand_r(&seed) % (jitter + 1)) : 0;
        sleep_until(capture + (uint64_t)frameUs + encLatency + late);
        if (aa->bExit) break;
        if (ctrl_flag("ai_fail")) { k++; continue; }
        Chunk_t *c = &gAudio[k % loop];
        aa->cbOnFrame(aa, c->p, c->len, capture + late, aa->contextOfOnFrame);
        k++;
    }
    return NULL;
}

Ai2Aenc_t *ai2aenc_initSys(CB_onAencFrame cbOnFrame, void *context) {
    Ai2Aenc_t *aa = calloc(1, sizeof(Ai2Aenc_t));
    aa->aiChn = MM_INVALID_CHN;
    aa->cbOnFrame = cbOnFrame;
    aa->contextOfOnFrame = context;
    return aa;
}

void ai2aenc_deinitSys(Ai2Aenc_t *aa) { free(aa); }

ERRORTYPE ai2aenc_prepare(Ai2Aenc_t *aa, AiParams_t *ai, AencParams_t *ae) {
    load_audio();
    aa->aiChn = 0;
    aidev_open("ai2aenc");
    stub_log("ai2aenc_prepare %p: dev %d, %d Hz, %d ch, %d bits, codec %s, %d bps", (void *)aa, ai->aiDev,
             ai->sampleRate, ai->channels, ai->bitsPerSample, (ae->codecType == PT_AAC) ? "AAC" : "other", ae->bitRate);
    return SUCCESS;
}

ERRORTYPE ai2aenc_start(Ai2Aenc_t *aa) {
    if (ctrl_flag("ai_fail")) {
        stub_log("ai2aenc_start %p: failing (ai_fail)", (void *)aa);
        return FAILURE;
    }
    usleep(120000); // AI + AENC start-up
    aa->bExit = false;
    pthread_create(&aa->threadId, NULL, aenc_thread, aa);
    stub_log("ai2aenc_start %p", (void *)aa);
    return SUCCESS;
}

ERRORTYPE ai2aenc_stop(Ai2Aenc_t *aa, bool disableDev) {
    aa->bExit = true;
    if (aa->threadId) {
        pthread_join(aa->threadId, NULL);
        aa->threadId = 0;
    }
    if (aa->aiChn != MM_INVALID_CHN) {
        aa->aiChn = MM_INVALID_CHN;
        aidev_close("ai2aenc", disableDev);
    }
    stub_log("ai2aenc_stop %p (disableDev=%d)", (void *)aa, disableDev);
    return SUCCESS;
}

//----------------------------------------------------------------------------------------------
// AI -> AO loop (the main app's audio monitor)
//----------------------------------------------------------------------------------------------
Ai2Ao_t *ai2ao_initSys(void) {
    Ai2Ao_t *ao = calloc(1, sizeof(Ai2Ao_t));
    ao->clkChn = MM_INVALID_CHN;
    return ao;
}
void ai2ao_deinitSys(Ai2Ao_t *ao) { free(ao); }
ERRORTYPE ai2ao_start(Ai2Ao_t *ao, AiParams_t *ai) {
    (void)ai;
    if (ao->clkChn == MM_INVALID_CHN) {
        aidev_open("ai2ao");
        ao->clkChn = 0;
    }
    stub_log("ai2ao_start");
    return SUCCESS;
}
ERRORTYPE ai2ao_stop(Ai2Ao_t *ao, bool disableDev) {
    if (ao->clkChn != MM_INVALID_CHN) {
        ao->clkChn = MM_INVALID_CHN;
        aidev_close("ai2ao", disableDev);
    }
    stub_log("ai2ao_stop (disableDev=%d)", disableDev);
    return SUCCESS;
}
bool ai2ao_playing(Ai2Ao_t *ao) { return ao->clkChn > MM_INVALID_CHN; }

//----------------------------------------------------------------------------------------------
// HDMI audio bridge (src/record/pcmbridge.c), its use of the AI device as the real one does it:
// one route at a time, a start replaces the running route (pb_stop(pb, aiDevFree) first); the
// route "ai" (HDMI out) has a channel on the AI device, any other source (hw:snddaudio2, HDMI in)
// none. The route is the content of $LAT_CTRL/hdmia_src ("ai" when missing); with
// $LAT_CTRL/hdmia_fail the start fails after its channel was created (a sink that does not open)
// and closes it again with aiDevFree as the disable decision, as pb_fail() does.
//----------------------------------------------------------------------------------------------
struct PcmBridge {
    bool running;
    bool usesAi;
};

static void ctrl_read(const char *name, char *buf, size_t len, const char *def) {
    const char *dir = getenv("LAT_CTRL");
    char path[512];
    snprintf(buf, len, "%s", def);
    if (!dir) return;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, (int)len, f)) buf[strcspn(buf, "\r\n")] = 0;
    fclose(f);
}

PcmBridge_t *pcmbridge_initSys(void) { return calloc(1, sizeof(PcmBridge_t)); }

static void pb_stub_stop(PcmBridge_t *pb, bool disableAiDev) {
    if (!pb->running) return;
    if (pb->usesAi) aidev_close("pcmbridge", disableAiDev);
    stub_log("pcmbridge stopped (%s, disableAiDev=%d)", pb->usesAi ? "ai" : "alsa source", disableAiDev);
    pb->running = pb->usesAi = false;
}

int pcmbridge_start(PcmBridge_t *pb, const char *confFile, AiParams_t *aiParams, bool aiDevFree) {
    char src[64];
    (void)confFile;
    (void)aiParams;
    if (pb == NULL) return -1;
    pb_stub_stop(pb, aiDevFree); // a running route is replaced
    ctrl_read("hdmia_src", src, sizeof(src), "ai");
    pb->usesAi = strcmp(src, "ai") == 0;
    if (pb->usesAi) aidev_open("pcmbridge");
    if (ctrl_flag("hdmia_fail")) {
        if (pb->usesAi) aidev_close("pcmbridge", aiDevFree);
        pb->usesAi = false;
        stub_log("pcmbridge start failed (src=%s, aiDevFree=%d)", src, aiDevFree);
        return -1;
    }
    pb->running = true;
    stub_log("pcmbridge started (src=%s, aiDevFree=%d)", src, aiDevFree);
    return 0;
}

void pcmbridge_stop(PcmBridge_t *pb, bool disableAiDev) {
    if (pb) pb_stub_stop(pb, disableAiDev);
}

void pcmbridge_deinitSys(PcmBridge_t *pb, bool disableAiDev) {
    if (pb == NULL) return;
    pb_stub_stop(pb, disableAiDev);
    free(pb);
}

bool pcmbridge_usesAi(PcmBridge_t *pb) { return pb != NULL && pb->running && pb->usesAi; }

//----------------------------------------------------------------------------------------------
// VI -> VENC (recording and live)
//----------------------------------------------------------------------------------------------
typedef struct {
    bool live;
    int h265;
    volatile bool iFrameRequested;
} VvStub_t;

static void *venc_thread(void *arg) {
    Vi2Venc_t *vv = arg;
    VvStub_t *st = (VvStub_t *)vv->spsppsBuff;
    Video_t *v = &gVideo[st->h265];
    long jitter = env_long("LAT_VIDEO_JITTER_US", 0);
    long encLatency = env_long("LAT_VIDEO_LATENCY_US", 15000);
    bool foreign = getenv("LAT_VIDEO_CLOCK") && strcmp(getenv("LAT_VIDEO_CLOCK"), "foreign") == 0;
    bool trailingSei = env_long("LAT_TRAILING_SEI", 0) != 0;
    unsigned seed = 12345u + (unsigned)st->live;
    uint8_t *buf = malloc(2 * 1024 * 1024);
    int loop = (int)env_long("LAT_LOOP_S", 32) * VIDEO_FPS; // the media timeline repeats
    if (loop > v->nbAu) loop = v->nbAu;

    usleep(80000); // VI + VENC start-up
    int n = (int)((now_us() - gT0) * VIDEO_FPS / 1e6) + 1;
    int first = n; // LAT_VIDEO_FROM_START: every encoder start plays the media from its frame 0
    bool fromStart = env_long("LAT_VIDEO_FROM_START", 0) != 0;
    while (!fromStart && !v->au[n % loop].idr) n++; // the encoder starts with an I frame
    while (!vv->bExit) {
        uint64_t capture = gT0 + (uint64_t)n * 1000000 / VIDEO_FPS;
        uint64_t late = jitter ? (uint64_t)(rand_r(&seed) % (jitter + 1)) : 0;
        sleep_until(capture + encLatency + late);
        if (vv->bExit) break;
        Au_t *au = &v->au[(fromStart ? n - first : n) % loop];
        if (st->iFrameRequested && !au->idr) { n++; continue; }
        st->iFrameRequested = false;
        int len = au->len;
        memcpy(buf, au->p, len);
        if (trailingSei) {
            // SEI, user data unregistered (16-byte UUID + 4 bytes), as the last NAL unit of the frame
            static const uint8_t sei264[] = {0, 0, 0, 1, 0x06, 0x05, 20, 0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7,
                                             0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef, 'T', 'E', 'S', 'T', 0x80};
            static const uint8_t sei265[] = {0, 0, 0, 1, 0x50, 0x01, 0x05, 20, 0xdc, 0x45, 0xe9, 0xbd, 0xe6, 0xd9, 0x48, 0xb7,
                                             0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef, 'T', 'E', 'S', 'T', 0x80};
            const uint8_t *sei = st->h265 ? sei265 : sei264;
            int seiLen = st->h265 ? (int)sizeof(sei265) : (int)sizeof(sei264);
            memcpy(buf + len, sei, seiLen);
            len += seiLen;
        }
        uint64_t pts = foreign ? (capture - gT0 + 7ull * 86400 * 1000000) : capture;
        vv->cbOnFrame(vv, buf, len, au->idr ? FRAME_typeI : FRAME_typeP, pts, vv->contextOfOnFrame);
        n++;
    }
    free(buf);
    return NULL;
}

static Vi2Venc_t *vv_new(CB_onFrame cb, void *ctx, bool live) {
    Vi2Venc_t *vv = calloc(1, sizeof(Vi2Venc_t));
    VvStub_t *st = calloc(1, sizeof(VvStub_t));
    st->live = live;
    vv->spsppsBuff = (uint8_t *)st; // stub state
    vv->cbOnFrame = cb;
    vv->contextOfOnFrame = ctx;
    return vv;
}

Vi2Venc_t *vi2venc_initSys(CB_onFrame cb, void *ctx) { return vv_new(cb, ctx, false); }
Vi2Venc_t *vi2live_initSys(CB_onFrame cb, void *ctx) { return vv_new(cb, ctx, true); }
void vi2venc_deinitSys(Vi2Venc_t *vv) { free(vv->spsppsBuff); free(vv); }
void vi2live_deinitSys(Vi2Venc_t *vv) { free(vv->spsppsBuff); free(vv); }
bool vi2live_started(Vi2Venc_t *vv) { return vv->threadId > 0; }

ERRORTYPE vi2venc_prepare(Vi2Venc_t *vv, ViParams_t *vi, VencParams_t *ve, VencParams_t *jpg) {
    VvStub_t *st = (VvStub_t *)vv->spsppsBuff;
    (void)jpg;
    vv->viParams = *vi;
    vv->veParams = *ve;
    st->h265 = (ve->codecType == PT_H265);
    load_video(st->h265);
    stub_log("vi2venc_prepare (%s): %s %dx%d %d fps", st->live ? "live" : "record", st->h265 ? "H.265" : "H.264",
             ve->width, ve->height, ve->fps);
    return SUCCESS;
}

ERRORTYPE vi2venc_start(Vi2Venc_t *vv, vi2venc_threadProc proc) {
    VvStub_t *st = (VvStub_t *)vv->spsppsBuff;
    (void)proc;
    vv->bExit = false;
    pthread_create(&vv->threadId, NULL, venc_thread, vv);
    stub_log("vi2venc_start (%s)", st->live ? "live" : "record");
    return SUCCESS;
}

ERRORTYPE vi2venc_stop(Vi2Venc_t *vv, bool all) {
    VvStub_t *st = (VvStub_t *)vv->spsppsBuff;
    vv->bExit = true;
    if (vv->threadId) {
        pthread_join(vv->threadId, NULL);
        vv->threadId = 0;
    }
    stub_log("vi2venc_stop (%s, all=%d)", st->live ? "live" : "record", all);
    return SUCCESS;
}

ERRORTYPE vi2venc_getSpsPpsInfo(Vi2Venc_t *vv, VencSpspps_t *info, bool patch) {
    VvStub_t *st = (VvStub_t *)vv->spsppsBuff;
    (void)patch;
    info->pBuffer = gVideo[st->h265].hdr;
    info->nLength = gVideo[st->h265].hdrLen;
    return SUCCESS;
}

ERRORTYPE vi2venc_requestIFrame(Vi2Venc_t *vv) {
    ((VvStub_t *)vv->spsppsBuff)->iFrameRequested = true;
    return SUCCESS;
}

char *vi2venc_getRcModeName(VencRateControlMode_e rcMode) { (void)rcMode; return "stub"; }
char *vi2venc_getProfileName(VencProfile_e profile, bool h265) { (void)profile; (void)h265; return "stub"; }

int jpegenc_takePicture(JpegEncIO_t *io, JpegEncConfig_t *config, int32_t ms) {
    (void)io; (void)config; (void)ms;
    return 0;
}

//----------------------------------------------------------------------------------------------
// muxer (counts what the recording would write)
//----------------------------------------------------------------------------------------------
static uint32_t gMuxFrames[2];

FFPack_t *ffpack_openFile(char *sName, void *context) {
    (void)context;
    FFPack_t *ff = calloc(1, sizeof(FFPack_t));
    ff->ofmtContext = calloc(1, sizeof(AVFormatContext));
    stub_log("ffpack_openFile %s", sName);
    return ff;
}
int ffpack_newVideoStream(FFPack_t *ff, uint16_t programId, FFStreamParameters_t *param) { (void)ff; (void)programId; (void)param; return 0; }
int ffpack_newAudioStream(FFPack_t *ff, uint16_t programId, FFStreamParameters_t *param) { (void)ff; (void)programId; (void)param; return 1; }
int ffpack_start(FFPack_t *ff) { (void)ff; return 0; }
int ffpack_input(FFPack_t *ff, int streamIndex, uint8_t *frameData, int frameLen, bool keyFrame, uint64_t pts) {
    (void)frameData; (void)keyFrame; (void)pts;
    ff->nbTotalSize += frameLen;
    if (streamIndex >= 0 && streamIndex < 2) gMuxFrames[streamIndex]++;
    return 0;
}
FFPackCloseResult_t ffpack_close(FFPack_t *ff) {
    FFPackCloseResult_t result = {0};
    stub_log("ffpack_close: %llu bytes (muxed so far: %u video, %u audio frames)", (unsigned long long)ff->nbTotalSize,
             gMuxFrames[0], gMuxFrames[1]);
    free(ff->ofmtContext);
    free(ff);
    return result;
}
int av_dict_set(AVDictionary **pm, const char *key, const char *value, int flags) {
    (void)pm; (void)key; (void)value; (void)flags;
    return 0;
}

//----------------------------------------------------------------------------------------------
// SD card
//----------------------------------------------------------------------------------------------
bool disk_checkFile(char *sPath) {
    struct stat st;
    return sPath && stat(sPath, &st) == 0 && S_ISREG(st.st_mode);
}
bool disk_checkPath(char *sPath) { (void)sPath; return true; }
int disk_countMovies(char *sPath, char *sPrefix, char *sExts[], int nExts, int nIndexLen) {
    (void)sPath; (void)sPrefix; (void)sExts; (void)nExts; (void)nIndexLen;
    return 0;
}
// ripples' recorder: free space for the rolling reclaim (plenty: nothing to reclaim here)
uint32_t disk_availableSize(char *sPath) { (void)sPath; return 32000; }
void disk_sdstat_create(char *sPath, bool bThread) { (void)sPath; (void)bThread; }
void disk_sdstat_delete(void) {}
bool disk_sdstat(uint32_t mbFull, SdcardStatus_t *sds) {
    (void)mbFull;
    bool changed = !sds->mounted;
    sds->inserted = true;
    sds->mounted = true;
    sds->full = false;
    return changed;
}
