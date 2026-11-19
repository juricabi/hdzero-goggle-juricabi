#pragma once
/* Minimal alsa-lib declarations for pcmbridge.c (the SDK ships libasound.so
 * in lib/softwinner/lib but no headers). Values match alsa/pcm.h. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct _snd_pcm snd_pcm_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

typedef enum _snd_pcm_stream {
    SND_PCM_STREAM_PLAYBACK = 0,
    SND_PCM_STREAM_CAPTURE = 1,
} snd_pcm_stream_t;

typedef enum _snd_pcm_format {
    SND_PCM_FORMAT_S16_LE = 2,
} snd_pcm_format_t;

typedef enum _snd_pcm_access {
    SND_PCM_ACCESS_RW_INTERLEAVED = 3,
} snd_pcm_access_t;

typedef enum _snd_pcm_state {
    SND_PCM_STATE_OPEN = 0,
    SND_PCM_STATE_SETUP,
    SND_PCM_STATE_PREPARED,
    SND_PCM_STATE_RUNNING,
    SND_PCM_STATE_XRUN,
    SND_PCM_STATE_DRAINING,
    SND_PCM_STATE_PAUSED,
    SND_PCM_STATE_SUSPENDED,
    SND_PCM_STATE_DISCONNECTED,
} snd_pcm_state_t;

#define SND_PCM_NONBLOCK 0x00000001

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode);
int snd_pcm_close(snd_pcm_t *pcm);
int snd_pcm_set_params(snd_pcm_t *pcm, snd_pcm_format_t format, snd_pcm_access_t access,
                       unsigned int channels, unsigned int rate, int soft_resample, unsigned int latency);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size);
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size);
int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent);
int snd_pcm_wait(snd_pcm_t *pcm, int timeout);
int snd_pcm_start(snd_pcm_t *pcm);
int snd_pcm_drop(snd_pcm_t *pcm);
int snd_pcm_prepare(snd_pcm_t *pcm);
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm);
snd_pcm_sframes_t snd_pcm_avail(snd_pcm_t *pcm);
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delayp);
const char *snd_strerror(int errnum);

#ifdef __cplusplus
}
#endif
