/*
 * Audio mixer: read-back of the codec's mixer controls.
 *
 * Straight from the ALSA control device with two ioctls, no process: the mixer's worker
 * compares the codec with the last batch every few seconds and applies again when someone
 * else has changed it, e.g. libmedia_mpp's AW_MPI_SYS_Init() when the record daemon
 * (re)starts or a DVR file is played (docs/audio.md).
 */

#include "audio_mixer.h"

#ifndef EMULATOR_BUILD

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h> // struct timespec, used by sound/asound.h
#include <unistd.h>

#include <sound/asound.h>

#include <log/log.h>

// The card amixer writes to: there is no asound.conf, so "default" is card 0 (audiocodec).
#define CODEC_CTL_DEVICE "/dev/snd/controlC0"

typedef struct {
    struct snd_ctl_elem_id id; // with name and numid
    snd_ctl_elem_type_t type;
    unsigned count;
} control_t;

static int ctl_fd = -1;
static bool ctl_failed;
static control_t controls[48];
static size_t control_count;

static bool open_codec(void) {
    if (ctl_fd >= 0)
        return true;
    if (ctl_failed)
        return false;

    ctl_fd = open(CODEC_CTL_DEVICE, O_RDONLY | O_CLOEXEC);
    if (ctl_fd < 0) {
        ctl_failed = true;
        LOGE("audio mixer: cannot open %s, not watching the codec", CODEC_CTL_DEVICE);
        return false;
    }

    struct snd_ctl_card_info card;
    memset(&card, 0, sizeof(card));
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_CARD_INFO, &card) == 0)
        LOGI("audio mixer: watching card '%s'", (const char *)card.id);
    return true;
}

static const control_t *find_control(const char *name) {
    for (size_t i = 0; i < control_count; i++)
        if (strncmp((const char *)controls[i].id.name, name, sizeof(controls[i].id.name)) == 0)
            return &controls[i];

    if (control_count >= sizeof(controls) / sizeof(controls[0]))
        return NULL;

    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof(info));
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    strncpy((char *)info.id.name, name, sizeof(info.id.name) - 1);
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
        return NULL;

    control_t *control = &controls[control_count++];
    control->id = info.id;
    control->type = info.type;
    control->count = info.count;
    return control;
}

int audio_mixer_codec_read(const char *name, int *value) {
    if (!open_codec())
        return -1;

    const control_t *control = find_control(name);
    if (!control)
        return -1;

    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof(v));
    v.id = control->id;
    if (ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0)
        return -1;

    const unsigned count = control->count < 2 ? 1 : 2;
    switch (control->type) {
    case SNDRV_CTL_ELEM_TYPE_BOOLEAN:
    case SNDRV_CTL_ELEM_TYPE_INTEGER:
        *value = (int)v.value.integer.value[0];
        // amixer sets every channel to the value; report a mismatch as -1
        if (count > 1 && v.value.integer.value[1] != v.value.integer.value[0])
            *value = -1;
        return 0;
    case SNDRV_CTL_ELEM_TYPE_ENUMERATED:
        *value = (int)v.value.enumerated.item[0];
        if (count > 1 && v.value.enumerated.item[1] != v.value.enumerated.item[0])
            *value = -1;
        return 0;
    default:
        return -1;
    }
}

#else

// The emulator has no codec to watch.
int audio_mixer_codec_read(const char *name, int *value) {
    (void)name;
    (void)value;
    return -1;
}

#endif
