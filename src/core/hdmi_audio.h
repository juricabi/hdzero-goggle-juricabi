#pragma once
/* ===========================================================================
 * HDMI audio routing (EXPERIMENTAL, default off). See docs/hdmi-audio.md.
 *
 * setting.ini (own section; set by hand on ripples-all until the Audio page has the
 * HDMI Out Audio / HDMI In Audio switches, which change it through hdmi_audio_set(),
 * which only stores):
 *   [hdmi]
 *   audio_out=0  1 = IT66121 sends 48 kHz stereo L-PCM taken from its I2S input,
 *                    and the record daemon copies the ADC capture (the same signal
 *                    that is recorded) to I2S0 (ALSA hw:snddaudio0).
 *   audio_in=0   1 = while HDMI in is shown, the record daemon plays I2S2 (ALSA
 *                    hw:snddaudio2, assumed IT66021 audio) into the codec DAC, which
 *                    is heard on the line out at the DVR Playback volume (ripples' codec
 *                    routing, hdmi_audio.c hdmi_audio_line_out(); not recorded); the
 *                    line out then stays on during HDMI in. The EDID offers 48 kHz
 *                    L-PCM only.
 * Only exactly "1" counts; any other value, a missing key or an unreadable file is 0.
 *
 * Both take effect only at the next boot: rc.sh runs hdmi_audio_boot, which records what
 * is in effect for this boot (and for audio_out switches PH1-PH3 to I2S0, once, with the
 * checks of src/record/hdmi_boot.h). The app reads that record at start; nothing touches
 * a pin function, the I2S0 registers or the IT66121 because a switch was clicked. Switching
 * Off and restarting is a full undo (rc.sh writes the stock pin functions at every boot).
 * With both keys 0 (default) nothing here touches any hardware, and the line out is muted
 * during HDMI in as in the stock firmware.
 * ======================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// app start, before the IT66121 / IT66021 are initialised: the stored switch positions and
// what is in effect for this boot (/tmp/hdmi_audio.boot); configures the two drivers
void hdmi_audio_load(void);

// HDMI audio exists on this target: true on Goggle 1, Goggle 2 and BoxPro (the same device tree,
// sys_config and rc.sh), false on any other (src/record/hdmi_boot.c hdmi_boot_supported())
bool hdmi_audio_supported(void);

// Stored switch position: what setting.ini [hdmi] audio_out says now (exactly "1"), as last
// written by hdmi_audio_set(). Not necessarily in effect: see hdmi_audio_out_active().
bool hdmi_audio_out_enabled(void);
// HDMI Out Audio in effect for this boot: audio_out was 1 at boot, PH1-PH3 were switched and
// verified, and no fault since. Can turn false during the boot (a fault), never true.
bool hdmi_audio_out_active(void);
// the same for HDMI In Audio (audio_in; in effect for this boot = audio_in was 1 at boot)
bool hdmi_audio_in_enabled(void);
bool hdmi_audio_in_active(void);
// NULL, or why HDMI Out Audio stayed off this boot although audio_out was 1 at boot. Exactly
// one of these English strings (they go through the language files):
//   "pin check failed"       the PH1-PH3 boot step refused, did not verify, or no valid
//                            record of this boot exists (deleted, corrupt, other boot)
//   "HDMI chip check failed" the IT66121 Reg04 check (src/driver/it66121.c) tripped
//   "audio path failed"      the record daemon could not start or was refused the route
// The Audio page shows "Restart" (orange) while enabled != active, and "Error" plus this
// reason in its hint line while it is not NULL.
const char *hdmi_audio_out_fault(void);
// store both switch positions in setting.ini; nothing else (they apply at the next boot)
void hdmi_audio_set(bool out, bool in);

// re-evaluate the route after a source / HDMI out change; hdmi_in_shown = the source being
// switched TO is HDMI in (g_source_info.source is set only after app_switch_to_*() returns)
void hdmi_audio_apply(bool hdmi_in_shown);
void hdmi_audio_poll(void); // every ~0.5 s: re-send what the record daemon did not confirm

#ifdef __cplusplus
}
#endif
