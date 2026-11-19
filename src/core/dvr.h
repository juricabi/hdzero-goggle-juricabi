#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "msp_displayport.h"

typedef enum {
    DVR_TOGGLE,
    DVR_STOP,
    DVR_START,
} osd_dvr_cmd_t;

extern bool dvr_is_recording;
extern bool record_pending;

void dvr_update_status();
void dvr_select_audio_source(uint8_t audio_source);
void dvr_set_dvr_audio_volume(int volume);
void dvr_set_live_audio_volume(int volume);
void dvr_set_mic_gain(int gain);
void dvr_set_linein_gain(int gain);
void dvr_mute_live_audio(void);
void dvr_restore_live_audio(void);
void dvr_enable_line_out(bool enable);
// ripples' raw codec routines of before the audio mixer: only while the mixer is held (the Audio page's tests)
void dvr_test_line_out(bool enable);
void dvr_test_select_audio_source(uint8_t source);
bool dvr_live_audio_is_enabled(void);
// True when there is enough room on the card to begin a recording. With rolling
// recording enabled the recorder frees space itself, so a nearly full card is no
// longer a reason to refuse the start.
bool dvr_space_allows_start(void);
// false: a start not taken (recorder unavailable, or a transition holds it paused)
bool dvr_cmd(osd_dvr_cmd_t cmd);
// Automatic DVR (src/core/dvr_auto.h): the backpack's recording command (MSP_SET_REC_STATE) ...
void dvr_auto_backpack_cmd(bool on, unsigned delay_s);
// ... every pass of the peripheral thread, with the source's picture ...
void dvr_auto_tick(bool picture);
// ... and whether Record Mode Auto may start a recording now (not after a disarm or a stop by the button)
bool dvr_auto_picture_start_allowed(void);
void dvr_update_vi_conf(video_resolution_t fmt);
void dvr_toggle();
void dvr_star();
void dvr_set_race_label(const uint8_t *label, uint16_t len);
void dvr_clear_race_label(void);

#ifdef __cplusplus
}
#endif
