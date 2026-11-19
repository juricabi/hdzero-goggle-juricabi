#pragma once

/*
 * Audio mixer for the V536 internal codec (all targets), ripples-all: ripples' Audio page (Record
 * Audio, Audio Source, DVR Playback, Live Volume, Mic Gain, Line/AV Gain, the test buttons) with
 * the extras of all's mixer (docs/audio.md).
 *
 * Every analog source has its own level in the analog output mixers, which feed the headphone /
 * line-out jack (left + right summed, mono). The ADC (recording, WiFi stream audio and HDMI-out
 * audio alike) takes that mix ("Record Headphone Mix": left and right separately, before the sum
 * and before the line-out volume) and/or each analog source straight from its input at full level
 * ("Record Mic / Line In / A/V In"; ripples' Audio Source is the preset that records one of them).
 * A source that is already in the recorded mix is not added a second time.
 *
 * ripples' controls keep their keys and their meaning: Live Volume is the line-out volume (and,
 * when it is moved, sets the A/V In level by ripples' curve), Mic Gain the MIC1 preamp, Line/AV
 * Gain the MIC2 preamp and the ADC input gain (whenever more than the mic alone is recorded),
 * Audio Source the preset of the Record switches and of the built-in receiver's audio. DVR
 * playback and the page's test buttons run ripples' own codec code: the mixer is held meanwhile
 * (audio_mixer_hold()) and applies its state again afterwards.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    // [mixer] of setting.ini (the keys and meaning of all's mixer, version 2)
    AUDIO_MIXER_MIC = 0,        // headphone level, 0 = off, 1..8 = -4.5..+6 dB
    AUDIO_MIXER_LINE_IN,        // headphone level
    AUDIO_MIXER_AV,             // headphone level
    AUDIO_MIXER_HDMI,           // headphone level of HDMI-in audio (the live digital source in the DAC)
    AUDIO_MIXER_REC_MIX,        // Record Headphone Mix: the output mixers into the ADC, 0/1
    AUDIO_MIXER_REC_MIC,        // Record Mic: the mic straight into the ADC (unless in the recorded mix), 0/1
    AUDIO_MIXER_REC_LINE_IN,    // Record Line In, 0/1
    AUDIO_MIXER_REC_AV,         // Record A/V In, 0/1
    AUDIO_MIXER_RX_AUDIO,       // audio output of the built-in analog receiver (RTC6715), 0/1;
                                // not a codec control (Goggle 2 and BoxPro only)
    AUDIO_MIXER_PHONES_AT_BOOT, // 0: as stock, the headphones get the mix from the first switch to an analog
                                // source on; 1: from boot. Set by the first change of a headphone level.
    // [record] of setting.ini, ripples' keys (src/core/settings.c loads them into g_setting.record)
    AUDIO_MIXER_LIVE_VOLUME,    // live_audio_volume 0..10: line-out volume (v * 31 + 5) / 10, 0 = off
    AUDIO_MIXER_MIC_GAIN,       // mic_gain 0..7: MIC1 preamp
    AUDIO_MIXER_LINEIN_GAIN,    // linein_gain 0..7: MIC2 preamp; ADC input gain unless only the mic is recorded
    AUDIO_MIXER_SOURCE,         // audio_source 0..2 (Mic, Line In, A/V In): the preset, audio_mixer_set_source()
    AUDIO_MIXER_FIELD_COUNT
} audio_mixer_field_t;

#define AUDIO_MIXER_LEVEL_MAX       8
#define AUDIO_MIXER_LIVE_VOLUME_MAX 10
#define AUDIO_MIXER_GAIN_MAX        7

typedef struct {
    uint8_t value[AUDIO_MIXER_FIELD_COUNT];
} audio_mixer_settings_t;

typedef struct {
    bool line_out; // false: line out muted (HDMI in, or the stock boot state)
    bool live_dac; // a live digital source plays into the DAC (HDMI-in audio), see audio_mixer_set_live_dac()
} audio_mixer_context_t;

// ripples' [record] values as src/core/settings.c loaded them (g_setting.record); audio_mixer_init()
// takes them over and may fill in what a setting.ini of all's firmware has instead (docs/audio.md)
typedef struct {
    int audio_source;
    int live_volume; // live_audio_volume
    int dvr_volume;  // dvr_audio_volume (DVR Playback: not routed by the mixer, ripples' playback code)
    int mic_gain;
    int linein_gain;
} audio_mixer_record_t;

// setting.ini section and key of a field
const char *audio_mixer_section(audio_mixer_field_t field);
const char *audio_mixer_key(audio_mixer_field_t field);

// Load the [mixer] settings, take over ripples' values, start the worker that writes them to the
// codec. Without a [mixer] of version 2, the Record switches and the receiver's audio come from
// [record] audio_source and the A/V In level from Live Volume (ripples' curve): the codec gets what
// ripples' firmware set. A [mixer] written by this firmware also holds the [record] audio_source and
// live_audio_volume its values belong to (record_source, record_live): when stock ripples changed
// one of them since, the Record switches and the receiver's audio (or the A/V In level) follow the
// new one again, as the Audio Source (or Live Volume) row would have set them. A setting.ini of all's
// firmware ([mixer] version 2, none of ripples' keys) keeps its [mixer] values, and ripples' values
// are filled in from all's equivalents: `rec` is updated, the caller copies it back to
// g_setting.record. Nothing is written to setting.ini.
void audio_mixer_init(audio_mixer_record_t *rec);

int audio_mixer_get(audio_mixer_field_t field);
int audio_mixer_max(audio_mixer_field_t field);
// Clamp and store one value (memory only): audio_mixer_apply() sends it to the codec,
// audio_mixer_save() writes it to setting.ini. Changing a headphone level of a source (Mic, Line In,
// A/V In, HDMI In) also makes the headphones live from every boot (AUDIO_MIXER_PHONES_AT_BOOT), and
// now, unless a source has muted the line out (HDMI in). (Not Live Volume: ripples' slider never
// switched the line out on.)
void audio_mixer_set(audio_mixer_field_t field, int value);
// ripples' Live Volume slider: the line-out volume and the A/V In level by ripples' curve
// (LINEIN gain (v^3 * 7 + 500) / 1000, at least -3 dB above 0)
void audio_mixer_set_live_volume(int value);
// ripples' Audio Source: record this source directly, not the other two; the built-in receiver's
// audio on with A/V In only (ripples: rtc6715.init(1, audio_source == A/V In))
void audio_mixer_set_source(int source);
// Write the values changed since the last save to setting.ini (nothing without a change). Call it
// from the UI, which writes the other settings (under lvgl_mutex), never from the worker. With a
// change, values that only follow stock ripples' newer [record] values (audio_mixer_init()) are
// written too, and the [record] values the [mixer] values belong to.
void audio_mixer_save(void);
bool audio_mixer_unsaved(void);

// Write the whole routing to the codec. Returns at once: a worker runs one amixer process
// for the latest state, so a burst of calls ends in one or two batches.
void audio_mixer_apply(void);
// Same, but wait (at most timeout_ms) until the codec has the current state (at once while held).
bool audio_mixer_apply_wait(unsigned timeout_ms);
// While idle, the worker compares the codec with the last batch every 250 ms (the 12 controls a
// libmedia_mpp reset writes: 12 ioctls, no process; every 2 s all of them) and applies again when
// it differs: libmedia_mpp's AW_MPI_SYS_Init() resets part of the mixer whenever the record daemon
// starts (boot, SD check, card insertion, SD repair / format), at a time the app does not know. A
// codec that cannot be read, an amixer that fails, or a codec that does not keep a value: logged,
// tried again with a backoff up to 60 s, never given up.

// ripples' own codec code runs (DVR playback, the Audio page's test buttons). Each holder has its
// own bit, so one releasing does not end another's hold. The first hold takes the mixer's own live
// paths out of the codec's mixers (one small batch: the mic, Line In, A/V In and DAC switches of the
// output mixers, and the output mixers' switches into the ADC), so that ripples' code finds the
// mixers as ripples' firmware left them: nothing but what it routes itself is heard or recorded.
// Then no batch and no watching while anybody holds; the last release applies the current state
// (and waits for it, at most 1 s), which also sets every one of those switches again.
typedef enum {
    AUDIO_MIXER_HOLD_PLAYBACK = 1 << 0, // ui_player.c: ripples' DVR playback (player/adec2ao.c)
    AUDIO_MIXER_HOLD_TEST = 1 << 1,     // page_audio.c: the test buttons
} audio_mixer_holder_t;
void audio_mixer_hold(audio_mixer_holder_t holder, bool hold);
// The batch of the first hold (amixer -s syntax); returns the length like snprintf.
size_t audio_mixer_build_held_commands(char *buf, size_t size);

// The record daemon is (re)started now or within a few seconds (Storage page scripts): compare the
// whole codec at once and every 50 ms for the next 6 s.
void audio_mixer_recheck(void);
// Host test: codec controls read by the watcher so far, and the controls of the quick pass.
unsigned audio_mixer_codec_reads(void);
bool audio_mixer_is_reset_control(const char *name);

// Line out allowed (an analog source) or muted (HDMI in), as ripples' dvr_enable_line_out(). At
// boot it is muted until the first call with true, like ripples' firmware that left the record
// daemon's (silent) mixer in place until then, unless AUDIO_MIXER_PHONES_AT_BOOT is set. While it
// is off, only what Record Headphone Mix records stays in the output mixers.
void audio_mixer_set_line_out(bool enable);
// HDMI-in audio (src/core/hdmi_audio.c, only with [hdmi] audio_in=1): a live digital source plays
// into the DAC. Its level is AUDIO_MIXER_HDMI; it is mixed with the other live sources, the line
// out is on even while HDMI in is the source (Live Volume permitting), and Record Headphone Mix
// records it (there is no direct path from the DAC into the ADC).
void audio_mixer_set_live_dac(bool on);

// Audio output of the built-in analog receiver: the audio_on of every rtc6715.init(1, ...)
// for the live picture (Goggle 2 and BoxPro; the Analog RSSI page keeps it off).
bool audio_mixer_rx_audio(void);

// Human readable value of a field ("Off", "-4.5 dB", "7", ...)
void audio_mixer_value_str(audio_mixer_field_t field, int value, char *buf, size_t size);

// The headphone level field of a Record source switch (AUDIO_MIXER_REC_MIC -> AUDIO_MIXER_MIC, ...),
// AUDIO_MIXER_FIELD_COUNT for other fields.
audio_mixer_field_t audio_mixer_level_of(audio_mixer_field_t rec_field);
// Is that source (a Record source switch field) recorded through the headphone mix (Record Headphone
// Mix on and its level above 0)? Then its direct path stays off: it would be recorded twice.
bool audio_mixer_in_recorded_mix(const audio_mixer_settings_t *settings, audio_mixer_field_t rec_field);
// The same for the current settings.
bool audio_mixer_recorded_via_mix(audio_mixer_field_t rec_field);
// Does the current state record exactly the Audio Source preset (its source directly, nothing else
// directly)? The page marks the Audio Source row otherwise.
bool audio_mixer_is_preset(void);

// ripples' curves (dvr.c had them): line-out volume of Live Volume, A/V In level of Live Volume
int audio_mixer_live_line_out(int live_volume);
int audio_mixer_live_av_level(int live_volume);

// The amixer batch (amixer -s syntax) for a state; returns the length like snprintf.
// Every control the mixer owns is set once. Order: a line out that goes off first, then
// gains and every path that is on, the line out, and last the paths that are off, so a
// change never leaves the output or the ADC without its sources in between (docs/audio.md).
size_t audio_mixer_build_commands(const audio_mixer_settings_t *settings, const audio_mixer_context_t *context, char *buf, size_t size);

// Defaults for a setting.ini without a [mixer] of version 2: ripples' state for these values
// (audio_source 0 = Mic, 1 = Line In, 2 = A/V In, as ripples read it).
void audio_mixer_defaults(audio_mixer_settings_t *settings, const audio_mixer_record_t *rec);

// Value a batch sets for a control; false if the batch does not set it.
bool audio_mixer_batch_value(const char *batch, const char *name, int *value);

// Read one control of the codec (audio_mixer_codec.c): 0 = ok, -1 = not available.
int audio_mixer_codec_read(const char *name, int *value);

#ifdef __cplusplus
}
#endif
