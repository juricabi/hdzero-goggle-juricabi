/*
 * Audio mixer: translation of the mixer state into codec controls.
 *
 * Pure code without firmware dependencies, so that utilities/audio_mixer/test_mixer.py can build
 * it on the host and check every issued control against the device's `amixer contents` dump
 * (mkapp/app/app/record/audio/audiocodec/alsa-util/).
 *
 * Signal paths (sun8iw16 codec, see docs/audio.md):
 *   MIC1 (built-in mic) -> MIC1 boost -> L/R output mixer (MIC1_G gain) / ADC input mixer
 *   MIC2 ("Line In")    -> MIC2 boost -> L/R output mixer (MIC2 BST gain) / ADC input mixer
 *   LINEIN (A/V)        ------------> L/R output mixer (LINEIN gain)    / ADC input mixer
 *   DAC (HDMI-in audio) ------------> L/R output mixer
 *   L/R output mixer -> LINEOUT (L+R sum) -> lineout volume -> headphone jack
 *                    -> ADC input mixer ("l_output/r_output mixer" switches: left ADC <- left
 *                       output mixer, right <- right; before the sum and the line-out volume)
 *   ADC input mixer -> ADC input gain -> ADC -> AIF1 -> record daemon (48 kHz stereo AAC)
 * The ADC input mixers are plain DAPM mixers (one bit per input in L/R_ADCMIX_SRC, summed), so
 * the output mixer and the direct inputs can be on together (docs/audio.md).
 *
 * juricabi: the line-out volume is ripples' Live Volume, the preamps ripples' Mic Gain and
 * Line/AV Gain, the ADC input gain ripples' Line/AV Gain unless only the mic is recorded (ripples
 * set it with Line In / A/V In as the Audio Source only, the mic recorded at the record daemon's
 * 0 dB). DVR playback is not routed here: ripples' playback code runs while the mixer is held.
 */

#include <stdio.h>

#include "audio_mixer.h"

#define OUTPUT_GAIN_0DB 3 // register value of the -4.5..+6 dB / 1.5 dB step gains for 0 dB
// 'ADC volume' / 'DAC volume' (digital, 0.75 dB steps): 160 is what libmedia_mpp sets and the
// old firmware recorded with (docs/audio.md).
#define DIGITAL_VOLUME_OLD 160
#define DAC_PLAYBACK       152 // 8 steps (6 dB) less: line out sums the left and right output mixers

static const uint8_t field_max[AUDIO_MIXER_FIELD_COUNT] = {
    [AUDIO_MIXER_MIC] = AUDIO_MIXER_LEVEL_MAX,
    [AUDIO_MIXER_LINE_IN] = AUDIO_MIXER_LEVEL_MAX,
    [AUDIO_MIXER_AV] = AUDIO_MIXER_LEVEL_MAX,
    [AUDIO_MIXER_HDMI] = AUDIO_MIXER_LEVEL_MAX,
    [AUDIO_MIXER_REC_MIX] = 1,
    [AUDIO_MIXER_REC_MIC] = 1,
    [AUDIO_MIXER_REC_LINE_IN] = 1,
    [AUDIO_MIXER_REC_AV] = 1,
    [AUDIO_MIXER_RX_AUDIO] = 1,
    [AUDIO_MIXER_PHONES_AT_BOOT] = 1,
    [AUDIO_MIXER_LIVE_VOLUME] = AUDIO_MIXER_LIVE_VOLUME_MAX,
    [AUDIO_MIXER_MIC_GAIN] = AUDIO_MIXER_GAIN_MAX,
    [AUDIO_MIXER_LINEIN_GAIN] = AUDIO_MIXER_GAIN_MAX,
    [AUDIO_MIXER_SOURCE] = 2,
};

int audio_mixer_max(audio_mixer_field_t field) {
    if (field >= AUDIO_MIXER_FIELD_COUNT)
        return 0;
    return field_max[field];
}

static int clamp(int value, int max) {
    if (value < 0)
        return 0;
    if (value > max)
        return max;
    return value;
}

int audio_mixer_live_line_out(int live_volume) {
    const int v = clamp(live_volume, AUDIO_MIXER_LIVE_VOLUME_MAX);
    return (v * 31 + 5) / 10; // ripples' live_volume_to_lineout()
}

int audio_mixer_live_av_level(int live_volume) {
    // ripples' dvr_set_live_audio_volume(): LINEIN output mixer gain register (v^3 * 7 + 500) / 1000,
    // at least 1 above 0; as a level (1..8 = register 0..7)
    const int v = clamp(live_volume, AUDIO_MIXER_LIVE_VOLUME_MAX);
    int gain = (v * v * v * 7 + 500) / 1000;
    if (v > 0 && gain == 0)
        gain = 1;
    return gain + 1;
}

void audio_mixer_defaults(audio_mixer_settings_t *settings, const audio_mixer_record_t *rec) {
    uint8_t *v = settings->value;

    // ripples passed the value to dvr_select_audio_source(uint8_t), which records A/V In for
    // anything above 2, and compared it with A/V In (2) for the receiver audio.
    uint8_t dvr_source = (uint8_t)rec->audio_source;
    if (dvr_source > 2)
        dvr_source = 2;

    // Headphones: what ripples did on a switch to an analog source (dvr_enable_line_out(true)):
    // A/V at the level of its Live Volume curve, no mic, no Line In.
    v[AUDIO_MIXER_MIC] = 0;
    v[AUDIO_MIXER_LINE_IN] = 0;
    v[AUDIO_MIXER_AV] = audio_mixer_live_av_level(rec->live_volume);
    // HDMI-in audio (only with HDMI In Audio on): 0 dB, the level of DVR playback.
    v[AUDIO_MIXER_HDMI] = OUTPUT_GAIN_0DB + 1;

    // Recording: exactly the one source of the Audio Source setting, straight into the ADC; the
    // headphone mix is not recorded.
    v[AUDIO_MIXER_REC_MIX] = 0;
    v[AUDIO_MIXER_REC_MIC] = dvr_source == 0;
    v[AUDIO_MIXER_REC_LINE_IN] = dvr_source == 1;
    v[AUDIO_MIXER_REC_AV] = dvr_source == 2;

    // Built-in analog receiver audio: ripples' rtc6715.init(1, audio_source == A/V In).
    v[AUDIO_MIXER_RX_AUDIO] = rec->audio_source == 2;

    // ripples set the headphone routing only on a switch to an analog source: booting straight
    // into HDZero (or HDMI in) left the headphones silent until then. Kept until a headphone level
    // of a source is changed on the Audio page.
    v[AUDIO_MIXER_PHONES_AT_BOOT] = 0;

    v[AUDIO_MIXER_LIVE_VOLUME] = clamp(rec->live_volume, AUDIO_MIXER_LIVE_VOLUME_MAX);
    v[AUDIO_MIXER_MIC_GAIN] = clamp(rec->mic_gain, AUDIO_MIXER_GAIN_MAX);
    v[AUDIO_MIXER_LINEIN_GAIN] = clamp(rec->linein_gain, AUDIO_MIXER_GAIN_MAX);
    v[AUDIO_MIXER_SOURCE] = dvr_source;
}

audio_mixer_field_t audio_mixer_level_of(audio_mixer_field_t rec_field) {
    switch (rec_field) {
    case AUDIO_MIXER_REC_MIC:
        return AUDIO_MIXER_MIC;
    case AUDIO_MIXER_REC_LINE_IN:
        return AUDIO_MIXER_LINE_IN;
    case AUDIO_MIXER_REC_AV:
        return AUDIO_MIXER_AV;
    default:
        return AUDIO_MIXER_FIELD_COUNT;
    }
}

bool audio_mixer_in_recorded_mix(const audio_mixer_settings_t *settings, audio_mixer_field_t rec_field) {
    const audio_mixer_field_t level = audio_mixer_level_of(rec_field);
    return level < AUDIO_MIXER_FIELD_COUNT && settings->value[AUDIO_MIXER_REC_MIX] && settings->value[level] > 0;
}

typedef struct {
    char *buf;
    size_t size;
    size_t len;
} cmd_buf_t;

static void cset(cmd_buf_t *cb, const char *name, int value) {
    size_t room = cb->len < cb->size ? cb->size - cb->len : 0;
    int n = snprintf(cb->buf ? cb->buf + cb->len : NULL, room, "cset name='%s' %d\n", name, value);
    if (n > 0)
        cb->len += (size_t)n;
}

size_t audio_mixer_build_held_commands(char *buf, size_t size) {
    // The live paths the mixer itself connects, out: what ripples' playback and tests do not switch
    // themselves (adec2ao.c and dvr_mute_live_audio() cut only LINEIN; libmedia_mpp's reset touches
    // none of these but the left DAC switch, which it sets again for the file), so that no headphone
    // source of the Audio page is heard with a DVR file or a test sample, and a test records only the
    // source it routes. Line out, volumes and the ADC inputs stay: ripples' code sets them.
    static const char *const off[] = {
        "Left Output Mixer MIC1Booststage Switch", "Right Output Mixer MIC1Booststage Switch",
        "Left Output Mixer MIC2Booststage Switch", "Right Output Mixer MIC2Booststage Switch",
        "Left Output Mixer LINEINL Switch",        "Right Output Mixer LINEINR Switch",
        "Left Output Mixer DACL Switch",           "Right Output Mixer DACR Switch",
        "Left Output Mixer DACR Switch",           "Right Output Mixer DACL Switch",
        "LADC input Mixer l_output mixer Switch",  "RADC input Mixer r_output mixer Switch",
        "LADC input Mixer r_output mixer Switch",  "RADC input Mixer l_output mixer Switch",
    };
    cmd_buf_t cb = {.buf = buf, .size = size, .len = 0};
    if (buf && size)
        buf[0] = 0;
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++)
        cset(&cb, off[i], 0);
    return cb.len;
}

size_t audio_mixer_build_commands(const audio_mixer_settings_t *settings, const audio_mixer_context_t *context, char *buf, size_t size) {
    cmd_buf_t cb = {.buf = buf, .size = size, .len = 0};
    int v[AUDIO_MIXER_FIELD_COUNT];

    if (buf && size)
        buf[0] = 0;

    for (int i = 0; i < AUDIO_MIXER_FIELD_COUNT; i++)
        v[i] = clamp(settings->value[i], field_max[i]);

    // Record Headphone Mix: the output mixers into the ADC
    const bool rec_mix = v[AUDIO_MIXER_REC_MIX] != 0;

    // Sources in the output mixer at their levels
    const int mon_mic = v[AUDIO_MIXER_MIC];
    const int mon_line = v[AUDIO_MIXER_LINE_IN];
    const int mon_av = v[AUDIO_MIXER_AV];

    // The DAC as a live source (HDMI-in audio), mixed with the others at its own level. It reaches
    // the ADC only through the output mixers (Record Headphone Mix); the AIF1 DA -> AD loopbacks are
    // never set. (DVR playback is ripples' code, while the mixer is held.)
    const bool live_dac = context->live_dac;
    const int mon_dac = live_dac ? v[AUDIO_MIXER_HDMI] : 0;
    const int dac_on = mon_dac > 0;
    // live level 1..8 = -4.5..+6 dB around the playback level, in 2 steps of 0.75 dB
    const int dac_volume = mon_dac ? DAC_PLAYBACK + 2 * (mon_dac - 1 - OUTPUT_GAIN_0DB) : DIGITAL_VOLUME_OLD;

    // Sources straight into the ADC at full level (Record Mic / Line In / A/V In), heard or not.
    // A source that is already in the recorded mix stays out: it would be recorded twice.
    const int adc_mic = v[AUDIO_MIXER_REC_MIC] && !(rec_mix && mon_mic > 0);
    const int adc_line = v[AUDIO_MIXER_REC_LINE_IN] && !(rec_mix && mon_line > 0);
    const int adc_av = v[AUDIO_MIXER_REC_AV] && !(rec_mix && mon_av > 0);

    // ADC input gain: ripples' Line/AV Gain, which ripples set with Line In / A/V In as the Audio
    // Source; with the mic alone the record daemon's 0 dB stayed
    const int adc_gain = (rec_mix || adc_line || adc_av) ? v[AUDIO_MIXER_LINEIN_GAIN] : OUTPUT_GAIN_0DB;

    // Line out volume (ripples' Live Volume); 0 switches the line out off
    const int volume = audio_mixer_live_line_out(v[AUDIO_MIXER_LIVE_VOLUME]);
    // (HDMI in mutes the line out, unless its audio is played: live_dac)
    const int line_out = (context->line_out || live_dac) && volume > 0;

    // What is connected to the output mixers. While the line out is off (HDMI in, Live Volume 0, or
    // the stock boot state: silent until the first switch to an analog source) only what the
    // recording of the mix needs: the record daemon's AW_MPI_SYS_Init() (SD check, card insertion,
    // SD repair / format) switches the line out on at volume 27 behind the mixer's back until the
    // watcher undoes it, and must then find nothing to play but the idle DAC, as on a stock goggle.
    // (The levels of the sources are kept: they are connected again with the line out.)
    const bool connect = line_out || rec_mix;
    const int out_mic = connect ? mon_mic : 0;
    const int out_line = connect ? mon_line : 0;
    const int out_av = connect ? mon_av : 0;
    const int out_dac = connect && dac_on;

    // Every switch the mixer owns, with the value it gets. They are written in two passes, the
    // ones that end up on before the ones that end up off: a path that stays never drops out,
    // and a change (another source, the mix or a direct source on/off) never leaves the line out
    // or the ADC without a source in between. Without a connected source, DAPM powers the line
    // out down (PA off, then on again: a click, and on the Goggle 2 the driver sleeps 160 ms in
    // the PA-off event).
    const struct {
        const char *name;
        int on;
    } sw[] = {
        // output mixers: headphone mix, and the recording with Record Headphone Mix
        {"Left Output Mixer MIC1Booststage Switch", out_mic > 0},
        {"Right Output Mixer MIC1Booststage Switch", out_mic > 0},
        {"Left Output Mixer MIC2Booststage Switch", out_line > 0},
        {"Right Output Mixer MIC2Booststage Switch", out_line > 0},
        {"Left Output Mixer LINEINL Switch", out_av > 0},
        {"Right Output Mixer LINEINR Switch", out_av > 0},
        {"Left Output Mixer DACL Switch", out_dac},
        {"Right Output Mixer DACR Switch", out_dac},
        {"Left Output Mixer DACR Switch", 0},
        {"Right Output Mixer DACL Switch", 0},
        {"DACL Mixer AIF1DA0L Switch", 1},
        {"DACR Mixer AIF1DA0R Switch", 1},
        // ADC input mixers: the recording
        {"LADC input Mixer MIC1 boost Switch", adc_mic},
        {"RADC input Mixer MIC1 boost Switch", adc_mic},
        {"LADC input Mixer MIC2 boost Switch", adc_line},
        {"RADC input Mixer MIC2 boost Switch", adc_line},
        {"LADC input Mixer LINEINL Switch", adc_av},
        {"RADC input Mixer LINEINR Switch", adc_av},
        {"LADC input Mixer l_output mixer Switch", rec_mix},
        {"RADC input Mixer r_output mixer Switch", rec_mix},
        {"LADC input Mixer r_output mixer Switch", 0},
        {"RADC input Mixer l_output mixer Switch", 0},
        {"AIF1 AD0L Mixer ADCL Switch", 1},
        {"AIF1 AD0R Mixer ADCR Switch", 1},
    };
    const size_t sw_count = sizeof(sw) / sizeof(sw[0]);

    // 1. a line out that goes off goes off first, so nothing below is heard
    if (!line_out)
        cset(&cb, "Lineout Switch", 0);

    // 2. gains: shared ones, and the output mixer gains of the sources that are on
    cset(&cb, "MIC1 boost AMP gain control", v[AUDIO_MIXER_MIC_GAIN]);
    cset(&cb, "MIC2 boost AMP gain control", v[AUDIO_MIXER_LINEIN_GAIN]);
    if (out_mic)
        cset(&cb, "MIC1_G boost stage output mixer control", out_mic - 1);
    if (out_line)
        cset(&cb, "MIC2 BST stage to L_R outp mixer gain", out_line - 1);
    if (out_av)
        cset(&cb, "LINEINL/R to L_R output mixer gain", out_av - 1);
    cset(&cb, "ADC input gain control", adc_gain);
    cset(&cb, "ADC volume", DIGITAL_VOLUME_OLD);
    cset(&cb, "DAC volume", dac_volume);
    // the digital volume of both AIF1 DAC timeslots: ripples' DVR Playback sets them for a file or a
    // test (dvr_set_dvr_audio_volume(), the test's timeslot 1 at 0); HDMI-in audio plays through
    // timeslot 0 at the stock 0 dB
    cset(&cb, "AIF1 DAC timeslot 0 volume", DIGITAL_VOLUME_OLD);
    cset(&cb, "AIF1 DAC timeslot 1 volume", DIGITAL_VOLUME_OLD);

    // 3. every path that is on
    for (size_t i = 0; i < sw_count; i++)
        if (sw[i].on)
            cset(&cb, sw[i].name, 1);

    // 4. line out (mono: both outputs carry the sum of the left and right output mixers);
    //    one that is on is switched on once its sources are connected
    cset(&cb, "LINEOUTL Mux", 1);
    cset(&cb, "LINEOUTR Mux", 1);
    cset(&cb, "lineout volume", volume);
    if (line_out)
        cset(&cb, "Lineout Switch", 1);

    // 5. every path that is off, then the gains of the sources that are off back to 0 dB
    for (size_t i = 0; i < sw_count; i++)
        if (!sw[i].on)
            cset(&cb, sw[i].name, 0);
    if (!out_mic)
        cset(&cb, "MIC1_G boost stage output mixer control", OUTPUT_GAIN_0DB);
    if (!out_line)
        cset(&cb, "MIC2 BST stage to L_R outp mixer gain", OUTPUT_GAIN_0DB);
    if (!out_av)
        cset(&cb, "LINEINL/R to L_R output mixer gain", OUTPUT_GAIN_0DB);

    return cb.len;
}
