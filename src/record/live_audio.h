#pragma once

/*
 * Live stream audio (RTSP AAC track).
 *
 * When rtspLive sets up the audio track of the live stream it connects the audio side of the
 * shared ring (avshare). The record process then runs a dedicated AI -> AENC (AAC, ADTS) chain,
 * independent of recording and of the "Record Audio" setting, and queues every encoded frame in
 * the ring. Both live video and live audio frames are stamped with CLOCK_MONOTONIC microseconds
 * (see liveclock_stamp(), and livetimeline_stamp() for audio), the clock rtspLive turns into
 * wall-clock presentation times. [live] audio_offset_ms (default 0) shifts the audio against the
 * video, for a pts clock that cannot be mapped exactly (see docs/live-audio.md).
 *
 * Nothing here runs unless rtspLive connects the audio stream, which it only does when
 * [live] audio=1 in the record conf (WiFi Module > System > Stream Audio).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ai2aenc.h"

typedef struct {
    bool valid;
    int64_t offsetUs;  /* encoder pts + offset = CLOCK_MONOTONIC us */
    const char *name;  /* "video" / "audio", for the log */
    bool reported;     /* what the pts clock looks like was logged for this stream */
} LiveClock_t;

/* Audio presentation times from a sample counter anchored to the first capture time: they
 * advance by exactly one frame duration per frame (smooth RTP clock), follow the capture times
 * slowly (sample clock drift) and re-anchor on a real discontinuity (lost samples, clock step). */
typedef struct {
    bool valid;
    uint32_t rate;
    int64_t anchorUs;  /* CLOCK_MONOTONIC us of sample 0 */
    uint64_t samples;  /* samples from the anchor to the current frame */
    int64_t avgErrUs;  /* smoothed capture time - timeline */
    int nOff;          /* consecutive frames far off the timeline */
} LiveTimeline_t;

typedef struct {
    Ai2Aenc_t *aa; /* dedicated AI + AENC chain for the live stream */
    bool running;
    bool enabled;              /* [live] audio, read when the reader connects */
    bool connected;            /* audio reader (rtspLive) connected, last poll */
    uint32_t tickDisconnected; /* when the reader went away */
    uint32_t tickRetry;        /* no start attempt before this tick after a failure */
    volatile uint32_t nbFrames; /* counted by the encoder thread */
    uint32_t nbDropped;
    uint32_t nbFramesSeen;     /* nbFrames at tickFrames ... */
    uint32_t tickFrames;       /* ... the last poll that saw it grow */
    int64_t offsetUs;          /* [live] audio_offset_ms: added to the audio timestamps */
    LiveClock_t clock;
    LiveTimeline_t timeline;
} LiveAudio_t;

uint64_t liveclock_nowUs(void);
void liveclock_reset(LiveClock_t *clock, const char *name);
/* Map an encoder pts (us) to CLOCK_MONOTONIC us, continuous within one stream. The first call
 * after a reset logs whether the pts looks like CLOCK_MONOTONIC and how old it was. */
uint64_t liveclock_stamp(LiveClock_t *clock, uint64_t ptsUs);
/* Presentation time (CLOCK_MONOTONIC us) of one ADTS frame captured at captureUs. */
uint64_t livetimeline_stamp(LiveTimeline_t *tl, const uint8_t *adts, int len, uint64_t captureUs);

bool liveaudio_init(LiveAudio_t *la);
void liveaudio_deinit(LiveAudio_t *la);
/* Poll from the record main loop: starts/stops the chain following the audio reader.
 * devInUse: another user of the AI device is active (recording audio, AI->AO loop). */
void liveaudio_run(LiveAudio_t *la, char *confFile, bool devInUse);
void liveaudio_stop(LiveAudio_t *la, bool devInUse);
bool liveaudio_running(LiveAudio_t *la);

#ifdef __cplusplus
}
#endif
