#include "audio_mixer.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "lang/language.h"
#include "ui/page_common.h"
#include "util/system.h"

#define AUDIO_MIXER_SECTION "mixer"
#define RECORD_SECTION      "record"
// 2: the version of all's firmware (082b2f5), whose [mixer] keys this one shares; 1 was a test
// build of all's (dvr_mode, dvr_mic, dvr_line_in, dvr_av), never released.
#define AUDIO_MIXER_VERSION 2

// The last batch stays there, handy to see what was sent to the codec.
#ifndef EMULATOR_BUILD
#define AUDIO_MIXER_CMD_FILE TMP_DIR "/audio_mixer.cmd"
#else
#define AUDIO_MIXER_CMD_FILE "audio_mixer.cmd"
#endif
#define AUDIO_MIXER_AMIXER "amixer -q -s < " AUDIO_MIXER_CMD_FILE

// A hook of the host test (utilities/audio_mixer/harness), compiled out on the goggle: right before a
// batch, after the state was taken, so that the test can take hold() in exactly the window that
// write_batch()'s check of held guards.
#ifdef AUDIO_MIXER_TEST_HOOKS
void audio_mixer_test_before_batch(void);
#define BEFORE_BATCH() audio_mixer_test_before_batch()
#else
#define BEFORE_BATCH() ((void)0)
#endif

// How often the worker compares the codec with the last batch (audio_mixer_codec.c). A pass reads
// the controls libmedia_mpp's reset writes (reset_controls[], at most 12 ioctls, no process);
// every AUDIO_MIXER_WATCH_FULL passes, every control of the batch (about 36 ioctls).
#ifndef AUDIO_MIXER_WATCH_MS
#define AUDIO_MIXER_WATCH_MS 250
#endif
#define AUDIO_MIXER_WATCH_FULL 8
// After audio_mixer_recheck() (the record daemon was restarted): a full pass this often, this long.
#define AUDIO_MIXER_RECHECK_MS     (AUDIO_MIXER_WATCH_MS / 5)
#define AUDIO_MIXER_RECHECK_WINDOW (AUDIO_MIXER_WATCH_MS * 24)
// Applies in a row for the same state when the codec does not keep it; then it is applied again
// with a backoff (and when the state changes).
#define AUDIO_MIXER_WATCH_RETRIES 3
// Backoff (codec not readable, amixer failing, routing not kept): from this, doubling, up to the max.
#define AUDIO_MIXER_BACKOFF_MS     (AUDIO_MIXER_WATCH_MS * 8)
#define AUDIO_MIXER_BACKOFF_MAX_MS (AUDIO_MIXER_WATCH_MS * 240)

// The controls libmedia_mpp's alsaOpenMixer() writes whenever AW_MPI_SYS_Init() runs (record daemon
// start, DVR playback; found by disassembling libmedia_mpp.so, docs/audio.md): a reset changes one of
// them, so the quick pass reads only these. utilities/audio_mixer/codec_model.py LIBMEDIA_RESET.
static const char *const reset_controls[] = {
    "Lineout Switch", "DACL Mixer AIF1DA0L Switch", "Left Output Mixer DACL Switch", "AIF1 AD0L Mixer ADCL Switch",
    "LADC input Mixer MIC1 boost Switch", "lineout volume", "DAC volume", "LINEOUTL Mux", "LINEOUTR Mux",
    "MIC1 boost AMP gain control", "ADC input gain control", "ADC volume",
};

static const struct {
    const char *section;
    const char *key;
} field_ini[AUDIO_MIXER_FIELD_COUNT] = {
    [AUDIO_MIXER_MIC] = {AUDIO_MIXER_SECTION, "mic"},
    [AUDIO_MIXER_LINE_IN] = {AUDIO_MIXER_SECTION, "line_in"},
    [AUDIO_MIXER_AV] = {AUDIO_MIXER_SECTION, "av"},
    [AUDIO_MIXER_HDMI] = {AUDIO_MIXER_SECTION, "hdmi"},
    [AUDIO_MIXER_REC_MIC] = {AUDIO_MIXER_SECTION, "rec_mic"},
    [AUDIO_MIXER_REC_LINE_IN] = {AUDIO_MIXER_SECTION, "rec_line_in"},
    [AUDIO_MIXER_REC_AV] = {AUDIO_MIXER_SECTION, "rec_av"},
    [AUDIO_MIXER_RX_AUDIO] = {AUDIO_MIXER_SECTION, "rx_audio"},
    [AUDIO_MIXER_PHONES_AT_BOOT] = {AUDIO_MIXER_SECTION, "phones_at_boot"},
    [AUDIO_MIXER_REC_LEVEL] = {AUDIO_MIXER_SECTION, "rec_level"},
    [AUDIO_MIXER_LIVE_VOLUME] = {RECORD_SECTION, "live_audio_volume"},
    [AUDIO_MIXER_MIC_GAIN] = {RECORD_SECTION, "mic_gain"},
    [AUDIO_MIXER_LINEIN_GAIN] = {RECORD_SECTION, "linein_gain"},
    [AUDIO_MIXER_SOURCE] = {RECORD_SECTION, "audio_source"},
};

/*
 * Threads: the UI (page, source switches) changes `mixer` / `context` and asks for an apply;
 * a worker writes the latest state to the codec, so a burst of requests (dial steps) ends
 * in one or two amixer runs and never waits for amixer (and the Goggle 2 driver's 160 ms
 * PA sleep) in the input thread. setting.ini is written by audio_mixer_save() only, from
 * the UI like every other setting.
 */
static pthread_mutex_t mixer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t batch_mutex = PTHREAD_MUTEX_INITIALIZER; // one amixer batch at a time
static pthread_cond_t work_cond;                                 // worker: something to apply
static pthread_cond_t done_cond;                                 // audio_mixer_apply_wait()
static audio_mixer_settings_t mixer;                             // current values
static audio_mixer_settings_t saved;                             // values in setting.ini (UI)
static audio_mixer_context_t context = {.line_out = false, .live_dac = false};
static unsigned requested; // apply generations: requested by the UI or the watcher ...
static unsigned applied;   // ... and written
static unsigned ui_requests; // requests from outside the worker: a new state, retried afresh
static bool worker_running;
static bool line_out_by_source; // audio_mixer_set_line_out() was called (a source decided)
static uint64_t recheck_until_ms; // audio_mixer_recheck(): full passes, often, until then
static char last_batch[4096]; // what the codec should have (batch_mutex)
static bool have_batch;
static bool batch_failed; // amixer did not run or failed: the codec may not have last_batch
static unsigned held;     // audio_mixer_hold(): one bit per holder, written under batch_mutex and mixer_mutex
static bool section_written; // setting.ini has a [mixer] of this version (UI thread)
// What setting.ini has in [record] for ripples (audio_source, live_audio_volume as stock ripples reads
// them), and the values of [mixer] record_source / record_live (NO_KEY: none): the [record] values the
// [mixer] values were written with (UI thread)
static long file_source, file_live, file_basis_source, file_basis_live;
static unsigned stale; // fields that follow stock ripples' newer [record] values: written with the next change
static unsigned in_file; // [mixer] fields setting.ini has (the others follow [record] at every start)
// juricabi.1's Record Headphone Mix (rec_mix) is in setting.ini: removed with the next save (UI thread)
static bool retire_rec_mix;
static unsigned codec_reads; // controls read by the watcher (host test: the cost of a pass)

static void start_worker(void);

const char *audio_mixer_section(audio_mixer_field_t field) {
    return field < AUDIO_MIXER_FIELD_COUNT ? field_ini[field].section : "";
}

const char *audio_mixer_key(audio_mixer_field_t field) {
    return field < AUDIO_MIXER_FIELD_COUNT ? field_ini[field].key : "";
}

static int clamp_field(audio_mixer_field_t field, long value) {
    if (value < 0)
        return 0;
    if (value > audio_mixer_max(field))
        return audio_mixer_max(field);
    return value;
}

#define NO_KEY (-999999L)
#define FIELD_BIT(field) (1u << (field))
static bool has_key(const char *section, const char *key) {
    return ini_getl(section, key, NO_KEY, SETTING_INI) != NO_KEY;
}

// all's DVR Playback line-out volume (0..31, 0 = off) as ripples' DVR Playback level (0..8): the
// smallest level whose line-out volume is at least as loud (ripples' dvr_volume_to_hardware())
static int dvr_volume_of_playback(long playback) {
    static const int line_out[] = {0, 26, 28, 29, 29, 30, 30, 31, 31};
    if (playback <= 0)
        return 0;
    for (int level = 1; level < 8; level++)
        if (line_out[level] >= playback)
            return level;
    return 8;
}

void audio_mixer_init(audio_mixer_record_t *rec) {
    pthread_mutex_lock(&mixer_mutex);

    // ripples' state: [record] audio_source and Live Volume (and the gains) as settings.c loaded them
    audio_mixer_settings_t defaults;
    audio_mixer_defaults(&defaults, rec);

    const long version = ini_getl(AUDIO_MIXER_SECTION, "version", 0, SETTING_INI);
    file_source = rec->audio_source;
    file_live = rec->live_volume;
    file_basis_source = ini_getl(AUDIO_MIXER_SECTION, "record_source", NO_KEY, SETTING_INI);
    file_basis_live = ini_getl(AUDIO_MIXER_SECTION, "record_live", NO_KEY, SETTING_INI);
    stale = 0;
    in_file = 0;
    if (version < AUDIO_MIXER_VERSION) {
        // No [mixer] yet (stock ripples, fresh install), one of a test build of all's, or one cut short
        // before its version was written: ripples' behaviour of these settings, whatever the section
        // had. Nothing is written for a stock setting.ini: the section appears with the first change on
        // the Audio page (audio_mixer_save()). A section that is there (old keys of version 1, stale
        // fields) goes, in one write.
        mixer = defaults;
        file_basis_source = file_basis_live = NO_KEY; // (a section of another version is no basis)
        char key[32];
        if (ini_getkey(AUDIO_MIXER_SECTION, 0, key, sizeof(key), SETTING_INI) > 0)
            ini_puts(AUDIO_MIXER_SECTION, NULL, NULL, SETTING_INI);
        LOGI("audio mixer: from record/audio_source=%d live_audio_volume=%d (mixer version %ld)",
             rec->audio_source, rec->live_volume, version);
    } else {
        // [mixer] of juricabi's firmware or of all's firmware: its values; a field missing from it comes from
        // ripples' state
        section_written = true;
        mixer = defaults;
        for (int i = 0; i < AUDIO_MIXER_FIELD_COUNT; i++) {
            if (strcmp(field_ini[i].section, AUDIO_MIXER_SECTION) != 0)
                continue;
            if (has_key(AUDIO_MIXER_SECTION, field_ini[i].key))
                in_file |= FIELD_BIT(i);
            mixer.value[i] = clamp_field(i, ini_getl(AUDIO_MIXER_SECTION, field_ini[i].key, defaults.value[i], SETTING_INI));
        }
        // juricabi.1's Record Headphone Mix (rec_mix=1) recorded what was heard: each input heard then is
        // recorded directly now. Its Record Level was Line/AV Gain whenever more than the mic alone was
        // recorded, else 0 dB. rec_mix goes with the next save.
        const long rec_mix = ini_getl(AUDIO_MIXER_SECTION, "rec_mix", NO_KEY, SETTING_INI);
        retire_rec_mix = rec_mix != NO_KEY;
        if (!(in_file & FIELD_BIT(AUDIO_MIXER_REC_LEVEL))) {
            const bool more_than_mic = rec_mix > 0 || mixer.value[AUDIO_MIXER_REC_LINE_IN] || mixer.value[AUDIO_MIXER_REC_AV];
            mixer.value[AUDIO_MIXER_REC_LEVEL] = more_than_mic ? clamp_field(AUDIO_MIXER_REC_LEVEL, rec->linein_gain) : 3;
        }
        if (rec_mix > 0) {
            for (audio_mixer_field_t f = AUDIO_MIXER_REC_MIC; f <= AUDIO_MIXER_REC_AV; f++) {
                if (mixer.value[audio_mixer_level_of(f)] > 0 && !mixer.value[f]) {
                    mixer.value[f] = 1;
                    stale |= FIELD_BIT(f);
                }
            }
            LOGI("audio mixer: Record Headphone Mix of juricabi.1: the inputs it heard are recorded directly");
        }
        // Stock ripples changed its Audio Source (Live Volume) since this firmware wrote [mixer]: the
        // Record switches, the Record Level and the receiver's audio (the A/V In level) as that row sets
        // them, from the new value; written with the next change on the page
        if (file_basis_source != NO_KEY && file_basis_source != rec->audio_source) {
            static const audio_mixer_field_t preset[] = {AUDIO_MIXER_REC_MIC, AUDIO_MIXER_REC_LINE_IN, AUDIO_MIXER_REC_AV,
                                                         AUDIO_MIXER_RX_AUDIO, AUDIO_MIXER_REC_LEVEL};
            for (size_t i = 0; i < sizeof(preset) / sizeof(preset[0]); i++) {
                mixer.value[preset[i]] = defaults.value[preset[i]];
                stale |= FIELD_BIT(preset[i]);
            }
            LOGI("audio mixer: [record] audio_source %d since record_source %ld: its preset", rec->audio_source, file_basis_source);
        }
        if (file_basis_live != NO_KEY && file_basis_live != rec->live_volume) {
            mixer.value[AUDIO_MIXER_AV] = defaults.value[AUDIO_MIXER_AV];
            stale |= FIELD_BIT(AUDIO_MIXER_AV);
            LOGI("audio mixer: [record] live_audio_volume %d since record_live %ld: A/V In by its curve", rec->live_volume, file_basis_live);
        }
        // A setting.ini of all's firmware has none of ripples' keys, but all's equivalents in [mixer]
        // (docs/audio.md): taken over in memory, written only when changed on the page
        const bool had_live = has_key(RECORD_SECTION, "live_audio_volume") || has_key(RECORD_SECTION, "audio_volume");
        if (!had_live && has_key(AUDIO_MIXER_SECTION, "headphones")) {
            const long headphones = ini_getl(AUDIO_MIXER_SECTION, "headphones", 31, SETTING_INI);
            mixer.value[AUDIO_MIXER_LIVE_VOLUME] = clamp_field(AUDIO_MIXER_LIVE_VOLUME, (headphones * 10 + 15) / 31);
        }
        if (!has_key(RECORD_SECTION, "mic_gain") && has_key(AUDIO_MIXER_SECTION, "mic_boost"))
            mixer.value[AUDIO_MIXER_MIC_GAIN] = clamp_field(AUDIO_MIXER_MIC_GAIN, ini_getl(AUDIO_MIXER_SECTION, "mic_boost", 4, SETTING_INI));
        if (!has_key(RECORD_SECTION, "linein_gain") && has_key(AUDIO_MIXER_SECTION, "line_in_boost"))
            mixer.value[AUDIO_MIXER_LINEIN_GAIN] = clamp_field(AUDIO_MIXER_LINEIN_GAIN, ini_getl(AUDIO_MIXER_SECTION, "line_in_boost", 4, SETTING_INI));
        if (!(in_file & FIELD_BIT(AUDIO_MIXER_REC_LEVEL)) && has_key(AUDIO_MIXER_SECTION, "dvr_level"))
            mixer.value[AUDIO_MIXER_REC_LEVEL] = clamp_field(AUDIO_MIXER_REC_LEVEL, ini_getl(AUDIO_MIXER_SECTION, "dvr_level", 3, SETTING_INI));
        if (!has_key(RECORD_SECTION, "dvr_audio_volume_v2") && !has_key(RECORD_SECTION, "dvr_audio_volume") &&
            !has_key(RECORD_SECTION, "audio_volume") && has_key(AUDIO_MIXER_SECTION, "playback"))
            rec->dvr_volume = dvr_volume_of_playback(ini_getl(AUDIO_MIXER_SECTION, "playback", 27, SETTING_INI));
        // ripples' Audio Source: the first recorded input (written with the next change)
        mixer.value[AUDIO_MIXER_SOURCE] = audio_mixer_source_of(&mixer);
        LOGI("audio mixer: [mixer] version %ld, record/audio_source=%d", version, rec->audio_source);
    }
    // what setting.ini has (or stands for): written when changed. (A [record] value taken over from
    // all's [mixer], or a default, is not in setting.ini yet: written with the first change of it.)
    saved = mixer;
    rec->live_volume = mixer.value[AUDIO_MIXER_LIVE_VOLUME];
    rec->mic_gain = mixer.value[AUDIO_MIXER_MIC_GAIN];
    rec->linein_gain = mixer.value[AUDIO_MIXER_LINEIN_GAIN];
    rec->audio_source = mixer.value[AUDIO_MIXER_SOURCE];
    // stock: the line out comes on with the first switch to an analog source (dvr_enable_line_out)
    context.line_out = mixer.value[AUDIO_MIXER_PHONES_AT_BOOT] != 0;

    pthread_mutex_unlock(&mixer_mutex);

    start_worker();
}

// What follows the settings of the page (mixer_mutex held): ripples' Audio Source (the first recorded
// input) and the built-in receiver's audio (on while A/V In is heard or recorded).
static void follow_locked(audio_mixer_field_t field) {
    if (field == AUDIO_MIXER_REC_MIC || field == AUDIO_MIXER_REC_LINE_IN || field == AUDIO_MIXER_REC_AV)
        mixer.value[AUDIO_MIXER_SOURCE] = audio_mixer_source_of(&mixer);
    if (field == AUDIO_MIXER_AV || field == AUDIO_MIXER_REC_AV)
        mixer.value[AUDIO_MIXER_RX_AUDIO] = mixer.value[AUDIO_MIXER_AV] > 0 || mixer.value[AUDIO_MIXER_REC_AV];
}

static bool is_headphone_level(audio_mixer_field_t field) {
    return field == AUDIO_MIXER_MIC || field == AUDIO_MIXER_LINE_IN || field == AUDIO_MIXER_AV || field == AUDIO_MIXER_HDMI;
}

int audio_mixer_get(audio_mixer_field_t field) {
    if (field >= AUDIO_MIXER_FIELD_COUNT)
        return 0;
    pthread_mutex_lock(&mixer_mutex);
    const int value = mixer.value[field];
    pthread_mutex_unlock(&mixer_mutex);
    return value;
}

// with mixer_mutex held
static void set_locked(audio_mixer_field_t field, int value) {
    const int old = mixer.value[field];
    mixer.value[field] = clamp_field(field, value);
    if (is_headphone_level(field) && mixer.value[field] != old && !mixer.value[AUDIO_MIXER_PHONES_AT_BOOT]) {
        // the pilot sets the headphones up: what the page shows is heard, now and from every boot;
        // a line out that a source muted (HDMI in) stays muted
        mixer.value[AUDIO_MIXER_PHONES_AT_BOOT] = 1;
        if (!line_out_by_source)
            context.line_out = true;
    }
    if (mixer.value[field] != old)
        follow_locked(field);
}

void audio_mixer_set(audio_mixer_field_t field, int value) {
    if (field >= AUDIO_MIXER_FIELD_COUNT)
        return;
    pthread_mutex_lock(&mixer_mutex);
    set_locked(field, value);
    pthread_mutex_unlock(&mixer_mutex);
}

void audio_mixer_save(void) {
    pthread_mutex_lock(&mixer_mutex);
    const audio_mixer_settings_t current = mixer;
    pthread_mutex_unlock(&mixer_mutex);

    if (memcmp(&current, &saved, sizeof(current)) == 0)
        return; // no change: nothing written
    bool wrote_mixer = false;
    for (int i = 0; i < AUDIO_MIXER_FIELD_COUNT; i++) {
        if (current.value[i] != saved.value[i] || (stale & FIELD_BIT(i))) {
            ini_putl(field_ini[i].section, field_ini[i].key, current.value[i], SETTING_INI);
            saved.value[i] = current.value[i];
            if (strcmp(field_ini[i].section, AUDIO_MIXER_SECTION) == 0) {
                wrote_mixer = true;
                in_file |= FIELD_BIT(i);
            }
            if (i == AUDIO_MIXER_SOURCE)
                file_source = current.value[i];
            if (i == AUDIO_MIXER_LIVE_VOLUME)
                file_live = current.value[i];
        }
    }
    stale = 0;
    // A [mixer] field setting.ini does not have comes from [record] at the next start (Audio Source,
    // Live Volume): one that would then differ from its value now is written too (Audio Source Line
    // In, then Record Mic Yes again: the Line In preset would turn it off)
    const audio_mixer_record_t file_rec = {.audio_source = (int)file_source, .live_volume = (int)file_live,
                                           .mic_gain = current.value[AUDIO_MIXER_MIC_GAIN],
                                           .linein_gain = current.value[AUDIO_MIXER_LINEIN_GAIN]};
    audio_mixer_settings_t from_record;
    audio_mixer_defaults(&from_record, &file_rec);
    for (int i = 0; i < AUDIO_MIXER_FIELD_COUNT; i++) {
        if (strcmp(field_ini[i].section, AUDIO_MIXER_SECTION) == 0 && !(in_file & FIELD_BIT(i)) &&
            from_record.value[i] != current.value[i]) {
            ini_putl(AUDIO_MIXER_SECTION, field_ini[i].key, current.value[i], SETTING_INI);
            in_file |= FIELD_BIT(i);
            wrote_mixer = true;
        }
    }
    if (wrote_mixer || section_written) {
        // [record] as the page shows it (a value taken over from all's firmware), and the [record]
        // values these [mixer] values belong to: stock ripples changing them is seen at the next start
        if (file_source != current.value[AUDIO_MIXER_SOURCE]) {
            file_source = current.value[AUDIO_MIXER_SOURCE];
            ini_putl(RECORD_SECTION, "audio_source", file_source, SETTING_INI);
        }
        if (file_live != current.value[AUDIO_MIXER_LIVE_VOLUME]) {
            file_live = current.value[AUDIO_MIXER_LIVE_VOLUME];
            ini_putl(RECORD_SECTION, "live_audio_volume", file_live, SETTING_INI);
        }
        if (file_basis_source != file_source) {
            file_basis_source = file_source;
            ini_putl(AUDIO_MIXER_SECTION, "record_source", file_basis_source, SETTING_INI);
            wrote_mixer = true;
        }
        if (file_basis_live != file_live) {
            file_basis_live = file_live;
            ini_putl(AUDIO_MIXER_SECTION, "record_live", file_basis_live, SETTING_INI);
            wrote_mixer = true;
        }
    }
    // juricabi.1's rec_mix: what it recorded is in the Record switches now
    if (retire_rec_mix) {
        ini_puts(AUDIO_MIXER_SECTION, "rec_mix", NULL, SETTING_INI);
        retire_rec_mix = false;
    }
    // the first change in [mixer]: the version last, so that a section cut short by a power loss is
    // read as none (from ripples' state again); the fields not written keep coming from there
    if (wrote_mixer && !section_written) {
        ini_putl(AUDIO_MIXER_SECTION, "version", AUDIO_MIXER_VERSION, SETTING_INI);
        section_written = true;
    }
}

bool audio_mixer_unsaved(void) {
    pthread_mutex_lock(&mixer_mutex);
    const bool unsaved = memcmp(&mixer, &saved, sizeof(mixer)) != 0;
    pthread_mutex_unlock(&mixer_mutex);
    return unsaved;
}

// The command file and one amixer process for it (batch_mutex held). true: amixer ran and succeeded.
static bool run_batch(const char *cmd, size_t len) {
    bool ok = false;
    FILE *fp = fopen(AUDIO_MIXER_CMD_FILE, "w");
    if (!fp) {
        LOGE("audio mixer: cannot write %s", AUDIO_MIXER_CMD_FILE);
    } else {
        const bool written = fwrite(cmd, 1, len, fp) == len;
        if (fclose(fp) != 0 || !written) {
            LOGE("audio mixer: cannot write %s", AUDIO_MIXER_CMD_FILE);
        } else {
            const int status = system_exec(AUDIO_MIXER_AMIXER);
            ok = status == 0;
            if (!ok)
                LOGE("audio mixer: amixer failed (status %d): the codec may not have this routing", status);
        }
    }
    return ok;
}

// Write one state to the codec: one amixer process for the whole batch. false: held, not written.
static bool write_batch(const audio_mixer_settings_t *settings, const audio_mixer_context_t *ctx) {
    static char cmd[4096];

    pthread_mutex_lock(&batch_mutex);
    if (held) {
        // ripples' own code drives the codec (playback, a test): the state is applied on release
        pthread_mutex_unlock(&batch_mutex);
        return false;
    }

    const size_t len = audio_mixer_build_commands(settings, ctx, cmd, sizeof(cmd));
    const uint8_t *v = settings->value;
    LOGI("audio mixer: heard mic=%d line_in=%d av=%d hdmi=%d volume=%d, recorded mic=%d line_in=%d av=%d at %d, boosts=%d/%d, "
         "source=%d rx=%d line_out=%d live_dac=%d",
         v[AUDIO_MIXER_MIC], v[AUDIO_MIXER_LINE_IN], v[AUDIO_MIXER_AV], v[AUDIO_MIXER_HDMI], v[AUDIO_MIXER_LIVE_VOLUME],
         v[AUDIO_MIXER_REC_MIC], v[AUDIO_MIXER_REC_LINE_IN], v[AUDIO_MIXER_REC_AV], v[AUDIO_MIXER_REC_LEVEL],
         v[AUDIO_MIXER_MIC_GAIN], v[AUDIO_MIXER_LINEIN_GAIN], v[AUDIO_MIXER_SOURCE], v[AUDIO_MIXER_RX_AUDIO],
         ctx->line_out, ctx->live_dac);

    // what the codec should have from now on, whatever happens below: the watcher compares the
    // codec with it, and applies again (with a backoff) when amixer did not get it there
    if (len < sizeof(cmd)) {
        memcpy(last_batch, cmd, len + 1);
        have_batch = true;
    }
    const bool ok = len < sizeof(cmd) && run_batch(cmd, len);
    if (ok && batch_failed)
        LOGI("audio mixer: amixer works again");
    batch_failed = !ok;

    pthread_mutex_unlock(&batch_mutex);
    return true;
}

// Apply the latest state; called with mixer_mutex held, returns with it held.
static void apply_latest(void) {
    const unsigned generation = requested;
    const audio_mixer_settings_t settings = mixer;
    const audio_mixer_context_t ctx = context;

    pthread_mutex_unlock(&mixer_mutex);
    BEFORE_BATCH();
    const bool written = write_batch(&settings, &ctx);
    pthread_mutex_lock(&mixer_mutex);

    if (written && (int)(generation - applied) > 0)
        applied = generation;
    pthread_cond_broadcast(&done_cond);
}

bool audio_mixer_batch_value(const char *batch, const char *name, int *value) {
    char line_name[64];
    int line_value;
    for (const char *line = batch; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (sscanf(line, "cset name='%63[^']' %d", line_name, &line_value) == 2 && strcmp(line_name, name) == 0) {
            *value = line_value;
            return true;
        }
    }
    return false;
}

static bool is_reset_control(const char *name) {
    for (size_t i = 0; i < sizeof(reset_controls) / sizeof(reset_controls[0]); i++)
        if (strcmp(reset_controls[i], name) == 0)
            return true;
    return false;
}

// Does the codec still have the values of the last batch (full: every control; otherwise the ones
// a libmedia_mpp reset writes)? -1: the codec cannot be read (emulator, no ALSA device).
static int codec_has_last_batch(bool full) {
    int result = 1;

    pthread_mutex_lock(&batch_mutex);
    for (const char *line = have_batch ? last_batch : ""; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        char name[64];
        int want, have;
        if (sscanf(line, "cset name='%63[^']' %d", name, &want) != 2)
            continue;
        if (!full && !is_reset_control(name))
            continue;
        codec_reads++;
        if (audio_mixer_codec_read(name, &have) != 0) {
            result = -1;
            break;
        }
        if (have != want) {
            LOGI("audio mixer: codec changed by someone else ('%s' = %d, want %d), applying again", name, have, want);
            result = 0;
            break;
        }
    }
    pthread_mutex_unlock(&batch_mutex);

    return result;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// Wait on cond (mixer_mutex held) at most ms; ETIMEDOUT when the time is up.
static int wait_ms(pthread_cond_t *cond, unsigned ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, &mixer_mutex, &deadline);
}

static unsigned next_backoff(unsigned ms) {
    if (ms < AUDIO_MIXER_BACKOFF_MS)
        return AUDIO_MIXER_BACKOFF_MS;
    return ms >= AUDIO_MIXER_BACKOFF_MAX_MS / 2 ? AUDIO_MIXER_BACKOFF_MAX_MS : 2 * ms;
}

static bool recheck_now; // audio_mixer_recheck(): a full pass at once (mixer_mutex)

static void *worker_thread(void *arg) {
    (void)arg;
    unsigned pass = 0;       // quick passes since the last full one
    unsigned backoff = 0;    // ms between passes while something fails; 0: AUDIO_MIXER_WATCH_MS
    int retries = 0;         // applies in a row by the watcher for the same state
    unsigned seen_ui = 0;    // ui_requests at the last apply
    bool unreadable = false; // the codec could not be read (logged once)

    pthread_mutex_lock(&mixer_mutex);
    for (;;) {
        // held: ripples' playback or a test drives the codec; neither applied nor watched until released
        while (held)
            pthread_cond_wait(&work_cond, &mixer_mutex);
        while (applied == requested && !held) {
            // Nothing to do: look at the codec now and then. The record daemon's AW_MPI_SYS_Init
            // (boot, SD check, card insertion, SD repair / format) resets part of the mixer whenever
            // it runs; whatever the timing, it is undone here.
            const bool rechecking = mono_ms() < recheck_until_ms;
            if (!recheck_now) {
                const unsigned period = backoff ? backoff : (rechecking ? AUDIO_MIXER_RECHECK_MS : AUDIO_MIXER_WATCH_MS);
                if (wait_ms(&work_cond, period) != ETIMEDOUT || applied != requested || held)
                    continue;
            }
            const bool full = recheck_now || rechecking || backoff || ++pass >= AUDIO_MIXER_WATCH_FULL;
            if (full)
                pass = 0;
            recheck_now = false;

            pthread_mutex_unlock(&mixer_mutex);
            const int has = codec_has_last_batch(full);
            pthread_mutex_lock(&mixer_mutex);
            pthread_mutex_lock(&batch_mutex);
            const bool failed = batch_failed;
            pthread_mutex_unlock(&batch_mutex);
            if (applied != requested || held)
                continue; // a new state came meanwhile, or ripples' code took the codec

            if (has == 0 || (has < 0 && failed)) {
                // someone else changed the codec, or amixer did not get the batch there: again,
                // at once a few times, then with a backoff
                if (retries < AUDIO_MIXER_WATCH_RETRIES) {
                    retries++;
                    backoff = 0;
                } else {
                    if (retries == AUDIO_MIXER_WATCH_RETRIES)
                        LOGE("audio mixer: the codec does not keep the routing, applying it again with a backoff (up to %u s)",
                             AUDIO_MIXER_BACKOFF_MAX_MS / 1000);
                    retries = AUDIO_MIXER_WATCH_RETRIES + 1;
                    backoff = next_backoff(backoff);
                }
                requested++;
            } else if (has < 0) {
                if (!unreadable)
                    LOGE("audio mixer: cannot read the codec controls, trying again with a backoff (up to %u s)",
                         AUDIO_MIXER_BACKOFF_MAX_MS / 1000);
                unreadable = true;
                backoff = next_backoff(backoff);
            } else {
                if (unreadable || retries > AUDIO_MIXER_WATCH_RETRIES)
                    LOGI("audio mixer: the codec has the routing (watching every %u ms again)", AUDIO_MIXER_WATCH_MS);
                unreadable = false;
                retries = 0;
                backoff = 0;
            }
        }
        if (held)
            continue;
        if (ui_requests != seen_ui) {
            // a new state: watched afresh
            seen_ui = ui_requests;
            retries = 0;
            backoff = 0;
        }
        apply_latest();
    }
    return NULL;
}

static void start_worker(void) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&work_cond, &attr);
    pthread_cond_init(&done_cond, &attr);
    pthread_condattr_destroy(&attr);

    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_thread, NULL) == 0) {
        pthread_detach(tid);
        pthread_mutex_lock(&mixer_mutex);
        worker_running = true;
        pthread_mutex_unlock(&mixer_mutex);
    } else {
        LOGE("audio mixer: no worker thread, applying synchronously");
    }
}

// Ask for an apply; returns the generation that has the current state.
static unsigned request_apply(void) {
    pthread_mutex_lock(&mixer_mutex);
    const unsigned generation = ++requested;
    ui_requests++;
    if (worker_running)
        pthread_cond_signal(&work_cond);
    else
        apply_latest();
    pthread_mutex_unlock(&mixer_mutex);
    return generation;
}

void audio_mixer_apply(void) {
    request_apply();
}

bool audio_mixer_apply_wait(unsigned timeout_ms) {
    const unsigned generation = request_apply();

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&mixer_mutex);
    // (held: applied on release, nothing to wait for)
    while ((int)(applied - generation) < 0 && !held) {
        if (pthread_cond_timedwait(&done_cond, &mixer_mutex, &deadline) == ETIMEDOUT)
            break;
    }
    const bool done = (int)(applied - generation) >= 0 || held;
    pthread_mutex_unlock(&mixer_mutex);

    if (!done)
        LOGW("audio mixer: codec not updated within %u ms", timeout_ms);
    return done;
}

void audio_mixer_hold(audio_mixer_holder_t holder, bool hold) {
    // under batch_mutex too: a batch that is being written finishes first, none starts afterwards
    pthread_mutex_lock(&batch_mutex);
    pthread_mutex_lock(&mixer_mutex);
    const unsigned before = held;
    held = hold ? (held | holder) : (held & ~(unsigned)holder);
    const bool taken = !before && held;
    const bool released = before && !held;
    if (taken) {
        // a watcher pass or an apply_wait() in progress sees it now (released: the apply below wakes
        // the worker, with every request made meanwhile: one batch)
        pthread_cond_broadcast(&work_cond);
        pthread_cond_broadcast(&done_cond);
    }
    pthread_mutex_unlock(&mixer_mutex);
    if (taken) {
        // the mixer's own live paths out before ripples' code takes the codec (still under
        // batch_mutex: no batch of the worker in between)
        static char cmd[1024];
        const size_t len = audio_mixer_build_held_commands(cmd, sizeof(cmd));
        LOGI("audio mixer: held (0x%x): the headphone sources and the recorded mix out", held);
        if (len >= sizeof(cmd) || !run_batch(cmd, len))
            LOGE("audio mixer: the live paths may still be connected while held");
    }
    pthread_mutex_unlock(&batch_mutex);

    if (released) {
        // ripples' code is done with the codec: the mixer's state again, everything set
        audio_mixer_apply_wait(1000);
    }
}

void audio_mixer_set_line_out(bool enable) {
    pthread_mutex_lock(&mixer_mutex);
    const bool changed = context.line_out != enable;
    context.line_out = enable;
    line_out_by_source = true;
    pthread_mutex_unlock(&mixer_mutex);

    if (changed)
        audio_mixer_apply();
}

void audio_mixer_recheck(void) {
    pthread_mutex_lock(&mixer_mutex);
    recheck_until_ms = mono_ms() + AUDIO_MIXER_RECHECK_WINDOW;
    recheck_now = true;
    pthread_cond_signal(&work_cond);
    pthread_mutex_unlock(&mixer_mutex);
}

unsigned audio_mixer_codec_reads(void) {
    pthread_mutex_lock(&batch_mutex);
    const unsigned n = codec_reads;
    pthread_mutex_unlock(&batch_mutex);
    return n;
}

bool audio_mixer_is_reset_control(const char *name) {
    return is_reset_control(name);
}

void audio_mixer_set_live_dac(bool on) {
    pthread_mutex_lock(&mixer_mutex);
    const bool changed = context.live_dac != on;
    context.live_dac = on;
    pthread_mutex_unlock(&mixer_mutex);

    if (changed)
        audio_mixer_apply();
}

bool audio_mixer_rx_audio(void) {
    return audio_mixer_get(AUDIO_MIXER_RX_AUDIO) != 0;
}

static void db_str(int tenths, char *buf, size_t size) {
    const int abs_tenths = tenths < 0 ? -tenths : tenths;
    snprintf(buf, size, "%s%d.%d dB", tenths < 0 ? "-" : (tenths > 0 ? "+" : ""), abs_tenths / 10, abs_tenths % 10);
}

void audio_mixer_value_str(audio_mixer_field_t field, int value, char *buf, size_t size) {
    switch (field) {
    case AUDIO_MIXER_MIC:
    case AUDIO_MIXER_LINE_IN:
    case AUDIO_MIXER_AV:
    case AUDIO_MIXER_HDMI:
        // 0 = off, 1..8 = output mixer gain register 0..7: -4.5 dB .. +6 dB in 1.5 dB steps
        if (value <= 0)
            snprintf(buf, size, "%s", _lang("Off"));
        else
            db_str(-45 + 15 * (value - 1), buf, size);
        break;
    case AUDIO_MIXER_REC_LEVEL:
        // the ADC input gain register 0..7: -4.5 dB .. +6 dB in 1.5 dB steps
        db_str(-45 + 15 * value, buf, size);
        break;
    case AUDIO_MIXER_LIVE_VOLUME:
        if (value <= 0)
            snprintf(buf, size, "%s", _lang("Off")); // (the line out off)
        else
            snprintf(buf, size, "%d", value);
        break;
    default:
        // ripples' sliders show their steps (Live Volume, Mic Gain, Line/AV Gain)
        snprintf(buf, size, "%d", value);
        break;
    }
}
