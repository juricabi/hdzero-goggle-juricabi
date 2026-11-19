// #define LOG_NDEBUG 0
#define LOG_TAG "live_audio"
#include <log/log.h>

#include <string.h>
#include <time.h>

#include "avshare.h"
#include "confparser.h"
#include "live_audio.h"

#ifndef LIVE_AUDIO_STOP_DELAY_MS // (host tests shorten it)
#define LIVE_AUDIO_STOP_DELAY_MS  3000 // keep the chain running across DESCRIBE -> SETUP
#endif
#define LIVE_AUDIO_RETRY_MS       5000 // after a failed start
#define LIVE_AUDIO_STALL_MS       2000 // running, but no frame for that long: restart the chain
#define LIVE_CLOCK_MAX_LATENCY_US 1000000

#define LIVE_TIMELINE_REANCHOR_US     15000 // more than capture jitter: lost samples, clock step ...
#define LIVE_TIMELINE_REANCHOR_FRAMES 8     // ... for that many frames in a row (not a late chunk)
#define LIVE_TIMELINE_SLEW_US         20    // per frame (< 1 RTP tick at 48 kHz): ~900 ppm of drift

//----------------------------------------------------------------------------------------------
// timestamps
//----------------------------------------------------------------------------------------------
uint64_t liveclock_nowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t liveclock_tickMs(void) {
    return (uint32_t)(liveclock_nowUs() / 1000);
}

void liveclock_reset(LiveClock_t *clock, const char *name) {
    clock->valid = false;
    clock->offsetUs = 0;
    clock->name = name;
    clock->reported = false;
}

uint64_t liveclock_stamp(LiveClock_t *clock, uint64_t ptsUs) {
    int64_t now = (int64_t)liveclock_nowUs();
    int64_t stamp = 0;

    if (clock->valid) {
        stamp = (int64_t)ptsUs + clock->offsetUs;
        // a frame cannot come from the future nor be very old: the encoder clock jumped
        if ((stamp > now + 200000) || (stamp < now - 2 * LIVE_CLOCK_MAX_LATENCY_US)) {
            LOGD("re-anchor live clock (pts=%llu, now=%lld)", ptsUs, now);
            clock->valid = false;
        }
    }

    if (!clock->valid) {
        int64_t delta = now - (int64_t)ptsUs;
        // MPP stamps captured frames with CLOCK_MONOTONIC: then the pts is the capture time,
        // slightly in the past, and is used as it is. Any other clock is anchored to the
        // arrival time of this frame and then follows the encoder pts.
        bool monotonic = (delta >= 0) && (delta < LIVE_CLOCK_MAX_LATENCY_US);
        clock->offsetUs = monotonic ? 0 : delta;
        clock->valid = true;
        stamp = (int64_t)ptsUs + clock->offsetUs;

        if (!clock->reported) {
            clock->reported = true;
            if (monotonic) {
                LOGI("live %s: pts clock looks like CLOCK_MONOTONIC, frame captured %.1f ms before it reached the ring",
                     clock->name ? clock->name : "?", delta / 1000.0);
            } else {
                LOGI("live %s: pts clock is not CLOCK_MONOTONIC (now - pts = %lld ms): stamped on arrival, "
                     "late by the capture-to-encoder latency ([live] audio_offset_ms can make up for it)",
                     clock->name ? clock->name : "?", (long long)(delta / 1000));
            }
        }
    }

    return (uint64_t)stamp;
}

uint64_t livetimeline_stamp(LiveTimeline_t *tl, const uint8_t *adts, int len, uint64_t captureUs) {
    static const uint32_t rates[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                       16000, 12000, 11025, 8000, 7350, 0, 0, 0};

    if ((len < 7) || (adts[0] != 0xFF) || ((adts[1] & 0xF6) != 0xF0)) {
        return captureUs; // not ADTS: nothing to count
    }
    uint32_t rate = rates[(adts[2] >> 2) & 0x0F];
    uint32_t samples = 1024 * ((adts[6] & 0x03) + 1);
    if (rate == 0) {
        return captureUs;
    }

    if (!tl->valid || (tl->rate != rate)) {
        tl->valid = true;
        tl->rate = rate;
        tl->anchorUs = (int64_t)captureUs;
        tl->samples = 0;
        tl->avgErrUs = 0;
        tl->nOff = 0;
    }

    int64_t stamp = tl->anchorUs + (int64_t)(tl->samples * 1000000ull / rate);
    int64_t err = (int64_t)captureUs - stamp;
    if ((err > LIVE_TIMELINE_REANCHOR_US) || (err < -LIVE_TIMELINE_REANCHOR_US)) {
        // a late (or early) capture time alone is jitter; many in a row are a discontinuity
        if (++tl->nOff >= LIVE_TIMELINE_REANCHOR_FRAMES) {
            LOGI("live audio: capture times %.1f ms off the sample count, timeline re-anchored", err / 1000.0);
            tl->anchorUs = (int64_t)captureUs;
            tl->samples = 0;
            tl->avgErrUs = 0;
            tl->nOff = 0;
            stamp = (int64_t)captureUs;
        }
    } else {
        // follow the mean capture time slowly: drift between the sample clock and CLOCK_MONOTONIC
        tl->nOff = 0;
        tl->avgErrUs += (err - tl->avgErrUs) / 32;
        int64_t slew = tl->avgErrUs / 32;
        if (slew > LIVE_TIMELINE_SLEW_US) {
            slew = LIVE_TIMELINE_SLEW_US;
        } else if (slew < -LIVE_TIMELINE_SLEW_US) {
            slew = -LIVE_TIMELINE_SLEW_US;
        }
        tl->anchorUs += slew;
        tl->avgErrUs -= slew;
        stamp += slew;
    }
    tl->samples += samples;

    return (uint64_t)stamp;
}

//----------------------------------------------------------------------------------------------
// audio chain
//----------------------------------------------------------------------------------------------
static int liveaudio_onFrame(void *aaPtr, uint8_t *frameData, int frameLen, uint64_t pts, void *context) {
    LiveAudio_t *la = (LiveAudio_t *)context;
    uint64_t capture = liveclock_stamp(&la->clock, pts);
    uint64_t ts = livetimeline_stamp(&la->timeline, frameData, frameLen, capture) + la->offsetUs;

    if (la->nbFrames == 0) {
        LOGD("first frame (len=%d, pts=%llu, ts=%llu)", frameLen, pts, ts);
    }
    la->nbFrames++;

    int ret = avshare_addAudioFrame(frameData, frameLen, ts);
    if (ret == 1) {
        // ring full: the reader is not keeping up (or paused), drop this frame
        if ((la->nbDropped++ % 100) == 0) {
            LOGD("audio ring full, dropped %u frames", la->nbDropped);
        }
    } else if ((ret != 0) && (ret != -3)) {
        LOGE("queue audio frame failed: %d (len=%d)", ret, frameLen);
    }

    return 0;
}

bool liveaudio_init(LiveAudio_t *la) {
    memset(la, 0, sizeof(LiveAudio_t));
    la->aa = ai2aenc_initSys(liveaudio_onFrame, la);
    return (la->aa != NULL);
}

void liveaudio_deinit(LiveAudio_t *la) {
    if (la->aa != NULL) {
        ai2aenc_deinitSys(la->aa);
        la->aa = NULL;
    }
}

bool liveaudio_running(LiveAudio_t *la) {
    return la->running;
}

static bool liveaudio_start(LiveAudio_t *la, char *confFile, bool devInUse) {
    AiParams_t aiParams;
    AencParams_t aeParams;
    ERRORTYPE ret;

    memset(&aiParams, 0, sizeof(aiParams));
    memset(&aeParams, 0, sizeof(aeParams));
    conf_loadAiParams(confFile, &aiParams);
    conf_loadAencParams(confFile, &aeParams);
    aeParams.codecType = PT_AAC; // the RTSP track is AAC (MPEG4-GENERIC), whatever the recording uses

    liveclock_reset(&la->clock, "audio");
    memset(&la->timeline, 0, sizeof(la->timeline));
    la->offsetUs = (int64_t)conf_loadLiveAudioOffset(confFile) * 1000;
    la->nbFrames = 0;
    la->nbDropped = 0;
    la->nbFramesSeen = 0;
    la->tickFrames = liveclock_tickMs();

    ret = ai2aenc_prepare(la->aa, &aiParams, &aeParams);
    if (ret == SUCCESS) {
        ret = ai2aenc_start(la->aa);
    }
    if (ret != SUCCESS) {
        LOGE("start failed: %x", ret);
        ai2aenc_stop(la->aa, !devInUse);
        return false;
    }

    LOGD("started: dev=%d, %d Hz, %d ch, %d bits, %d bps, audio offset %lld ms",
         aiParams.aiDev, aiParams.sampleRate, aiParams.channels, aiParams.bitsPerSample, aeParams.bitRate,
         (long long)(la->offsetUs / 1000));
    la->running = true;
    return true;
}

void liveaudio_stop(LiveAudio_t *la, bool devInUse) {
    if (!la->running || (la->aa == NULL)) {
        return;
    }

    ai2aenc_stop(la->aa, !devInUse);
    la->running = false;
    LOGD("stopped (%u frames, %u dropped)", la->nbFrames, la->nbDropped);
}

void liveaudio_run(LiveAudio_t *la, char *confFile, bool devInUse) {
    if (la->aa == NULL) {
        return;
    }

    uint32_t tickNow = liveclock_tickMs();
    bool connected = avshare_connected(MEDIA_AUDIO);

    if (connected) {
        if (!la->connected) {
            la->connected = true;
            // rtspLive only connects the audio track when the setting is on; check it anyway
            la->enabled = conf_loadLiveAudio(confFile);
            if (!la->enabled) {
                LOGD("audio reader connected, but [live] audio is off");
            }
            la->tickRetry = tickNow;
        }

        if (la->running) {
            // a chain that stops producing (capture or encoder stuck) is restarted
            uint32_t nbFrames = la->nbFrames;
            if (nbFrames != la->nbFramesSeen) {
                la->nbFramesSeen = nbFrames;
                la->tickFrames = tickNow;
            } else if ((uint32_t)(tickNow - la->tickFrames) >= LIVE_AUDIO_STALL_MS) {
                LOGE("no audio frame for %d ms, restarting the chain", LIVE_AUDIO_STALL_MS);
                liveaudio_stop(la, devInUse);
                la->tickRetry = tickNow;
            }
        }

        if (la->enabled && !la->running && ((int32_t)(tickNow - la->tickRetry) >= 0)) {
            if (!liveaudio_start(la, confFile, devInUse)) {
                la->tickRetry = tickNow + LIVE_AUDIO_RETRY_MS;
            }
        }
    } else {
        if (la->connected) {
            la->connected = false;
            la->tickDisconnected = tickNow;
        }

        if (la->running && ((tickNow - la->tickDisconnected) >= LIVE_AUDIO_STOP_DELAY_MS)) {
            liveaudio_stop(la, devInUse);
        }
    }
}
