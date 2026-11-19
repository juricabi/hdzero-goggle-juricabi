#include "page_audio.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "../conf/ui.h"

#include "core/app_state.h"
#include "core/audio_mixer.h"
#include "core/common.hh"
#include "core/dvr.h"
#include "core/hdmi_audio.h"
#include "lang/language.h"
#include "page_common.h"
#include "ui/ui_style.h"
#include "util/filesystem.h"
#include "util/system.h"
#include "wav_test.h"

/*
 * Audio page, ripples-all: ripples' page (Record Audio, Audio Source, DVR Playback, Live Volume, Mic
 * Gain, Line/AV Gain, the test buttons; its setting.ini keys) with the extras of all's mixer below it
 * (docs/audio.md): the headphone level of each source, the Record switches with the headphone mix,
 * the built-in receiver's audio, the two HDMI audio switches. One list in sections, scrolled so that
 * the selected row is always on screen, with the help of the selected row below it (ripples' note
 * for its test buttons).
 *
 * Sliders apply while they move (click to grab, dial, click to release), switches on click. Applying
 * runs in the mixer's worker; setting.ini is written when a switch is clicked, a slider is released
 * (ripples' DVR Playback then), the page is left, or SAVE_DELAY_MS after the last dial step of a
 * slider that is still grabbed - only what changed. The two HDMI switches are [hdmi] audio_out /
 * audio_in (core/hdmi_audio.c). ripples' Audio Source is the preset of the Record switches and of
 * the receiver's audio, ripples' Live Volume moves the A/V In level by ripples' curve
 * (core/audio_mixer.h). The test buttons run ripples' own codec code with the mixer held.
 */

#define SAVE_DELAY_MS 1000

// ripples' test buttons
#define DVR_AUDIO_VOLUME_MAX 8
#define AUDIO_TEST_SAMPLE "/mnt/app/app/audio/dvr_playback_volume_test.wav"
#define AUDIO_TEST_SAMPLE_SDCARD "/mnt/extsd/dvr_playback_volume_test.wav"
#define AUDIO_TEST_CAPTURE "/tmp/hdzero_audio_test.wav"
#define AUDIO_TEST_APLAY "/mnt/app/app/record/audio/aplay"
#define AUDIO_TEST_ARECORD "/mnt/app/app/record/audio/arecord"
#define AUDIO_TEST_COUNT 4
#define AUDIO_TEST_SECONDS 5            // live stream / record length
#define AUDIO_TEST_DVR_SAMPLE_MS 10000  // bundled test WAV length
// the test buttons fill the two button columns, the spacer and the value column
#define AUDIO_TEST_AREA_W (2 * UI_AUDIO_COL_BUTTON + UI_AUDIO_COL_SPACER + UI_AUDIO_COL_VALUE)
#define AUDIO_TEST_BUTTON_W ((AUDIO_TEST_AREA_W - (AUDIO_TEST_COUNT - 1) * UI_AUDIO_TEST_BUTTON_GAP) / AUDIO_TEST_COUNT)
// full button width: the solid-grey button has no border, so the progress
// fill spans edge to edge (clipped to the rounded corners by the button).
#define AUDIO_TEST_FILL_MAX_W (AUDIO_TEST_BUTTON_W)

typedef enum {
    ROW_HEADING,      // section heading, not selectable
    ROW_SLIDER,       // a mixer level or gain
    ROW_SWITCH,       // a mixer on/off field
    ROW_LIVE_VOLUME,  // ripples' Live Volume (the mixer, with the A/V In level by ripples' curve)
    ROW_DVR_VOLUME,   // ripples' DVR Playback ([record] dvr_audio_volume_v2, ripples' playback code)
    ROW_RECORD_AUDIO, // ripples' Record Audio ([record] audio)
    ROW_SOURCE,       // ripples' Audio Source: the preset of the Record switches
    ROW_TEST,         // ripples' test buttons
    ROW_HDMI_OUT,
    ROW_HDMI_IN,
    ROW_BACK,
} row_type_t;

typedef struct {
    row_type_t type;
    audio_mixer_field_t field; // sliders and mixer switches
    const char *name;
    const char *hint;
    const char *btn0; // switches: left button (field value 1), right button (0)
    const char *btn1;
} row_t;

#define HEADING(n)           {.type = ROW_HEADING, .field = AUDIO_MIXER_FIELD_COUNT, .name = (n)}
#define SLIDER(f, n, h)      {.type = ROW_SLIDER, .field = (f), .name = (n), .hint = (h)}
#define YES_NO(f, n, h)      {.type = ROW_SWITCH, .field = (f), .name = (n), .hint = (h), .btn0 = "Yes", .btn1 = "No"}
#define ON_OFF(t, f, n, h)   {.type = (t), .field = (f), .name = (n), .hint = (h), .btn0 = "On", .btn1 = "Off"}
#define RECORD_SOURCE(f, n)  YES_NO(f, n, NULL) // the hint depends on the state, see row_hint()

static const row_t rows[] = {
    // ---- ripples' page, as it was
    {.type = ROW_RECORD_AUDIO, .field = AUDIO_MIXER_FIELD_COUNT, .name = "Record Audio", .btn0 = "Yes", .btn1 = "No"},
    {.type = ROW_SOURCE, .field = AUDIO_MIXER_SOURCE, .name = "Audio Source"}, // hint: row_hint()
    HEADING("Volume Control"),
    {.type = ROW_DVR_VOLUME, .field = AUDIO_MIXER_FIELD_COUNT, .name = "DVR Playback", .hint = "Headphone volume while a DVR file plays"},
    {.type = ROW_LIVE_VOLUME, .field = AUDIO_MIXER_LIVE_VOLUME, .name = "Live Volume", .hint = "Headphone volume. Also sets the A/V In level"},
    SLIDER(AUDIO_MIXER_MIC_GAIN, "Mic Gain", "Mic gain for headphones and recording"),
    SLIDER(AUDIO_MIXER_LINEIN_GAIN, "Line/AV Gain", "Level of the recording, the stream and HDMI out"),
    {.type = ROW_TEST, .field = AUDIO_MIXER_FIELD_COUNT, .name = "Run Test"}, // hint: ripples' note

    // ---- the extras of all's mixer
    HEADING("Headphones"),
    SLIDER(AUDIO_MIXER_MIC, "Mic", "Goggle mic, heard in your headphones"),
    SLIDER(AUDIO_MIXER_LINE_IN, "Line In", "Line In jack (ext. mic), heard in headphones"),
#if defined(HDZGOGGLE2) || defined(HDZBOXPRO)
    SLIDER(AUDIO_MIXER_AV, "A/V In", "AV jack, module or receiver (needs Built-in RX Audio On)"),
#else
    SLIDER(AUDIO_MIXER_AV, "A/V In", "AV jack or analog module, heard in headphones"),
#endif
    // HDMI-in audio played into the DAC (audio_mixer_set_live_dac()); greyed while HDMI In Audio
    // is not in effect (selectable: its hint says why). All three targets (core/hdmi_audio.h).
    SLIDER(AUDIO_MIXER_HDMI, "HDMI In", "HDMI in audio, heard in headphones"),

    // the ADC: the DVR, the WiFi stream audio and HDMI out audio all take it
    HEADING("Recording (DVR, WiFi Stream, HDMI Out)"),
    YES_NO(AUDIO_MIXER_REC_MIX, "Record Headphone Mix", "Yes: record exactly what you hear above"),
    RECORD_SOURCE(AUDIO_MIXER_REC_MIC, "Record Mic"),
    RECORD_SOURCE(AUDIO_MIXER_REC_LINE_IN, "Record Line In"),
    RECORD_SOURCE(AUDIO_MIXER_REC_AV, "Record A/V In"),

#if defined(HDZGOGGLE2) || defined(HDZBOXPRO)
    // audio output of the built-in analog receiver, one of the sources of A/V In
    HEADING("Inputs"),
    ON_OFF(ROW_SWITCH, AUDIO_MIXER_RX_AUDIO, "Built-in RX Audio", "Audio of the built-in analog receiver on A/V In"),
#endif

    // both take effect at the next start only (core/hdmi_audio.h)
    HEADING("HDMI Audio (experimental)"),
    ON_OFF(ROW_HDMI_OUT, AUDIO_MIXER_FIELD_COUNT, "HDMI Out Audio", "Send the recorded audio to HDMI out. Restart to apply"),
    ON_OFF(ROW_HDMI_IN, AUDIO_MIXER_FIELD_COUNT, "HDMI In Audio", "Hear HDMI in audio. Restart to apply"),

    {.type = ROW_BACK, .field = AUDIO_MIXER_FIELD_COUNT, .name = "Back"},
};

#define ROW_COUNT ((int)ARRAY_SIZE(rows))

// hints of the Record source switches
#define HINT_IN_MIX     "Recorded via the mix, at its headphone level"
#define HINT_DIRECT_YES "Recorded even if its headphone level is Off"
#define HINT_DIRECT_NO  "Not recorded. Yes: record it directly"
// hint of the Audio Source: its source recorded as above, or the Record switches below differ from it
#define HINT_SOURCE_CHANGED "Changed in the Recording section below"
// hints of the HDMI In level while HDMI In Audio is not in effect
#define HINT_HDMI_OFF     "Off: switch on HDMI In Audio at the bottom"
#define HINT_HDMI_RESTART "Restart the goggle to use HDMI In Audio"
// HDMI Out Audio switched on, but kept off at this start (core/hdmi_audio.h: the reason follows)
#define HINT_HDMI_FAULT "Off at this start"
// HDMI Out Audio can be switched on only with the read-only probe's GO for this board (hdmi_boot.h)
#define HINT_HDMI_PROBE "Run the HDMI audio probe first (hdmi-audio.md, 7)"
// a Mic or Line/AV test recorded nothing: the audio input is held by Stream Audio or HDMI Out Audio
// (the record daemon's capture), or failed otherwise; the two rows' names follow
#define HINT_TEST_BUSY "Nothing recorded: audio input busy"
// value column of the HDMI rows: the switch differs from what is in effect, or it failed
#define MARK_RESTART "Restart"
#define MARK_ERROR   "Error"
// ripples' note of the test buttons
#if defined(HDZBOXPRO) || defined(HDZGOGGLE2)
// "Active", not "wired": a received analog signal (e.g. a transmitting drone) feeds the tests too,
// not just the AV jack.
static const char *const HINT_TEST[] = {"1. Mic: record 5s, auto playback.", "2. Line/AV: record 5s, auto playback.",
                                         "3. Test fill: pulsing = loading, red = recording, green = playback.",
                                         "4. Live & Line/AV need an active analog audio source."};
#else
// The faint clicking is menu-mode electrical interference coupling into the codec converters
// (confirmed by waveform analysis: ~130 analog impulses/s in menu captures, zero in video-mode
// recordings on the same unit). Not reachable from software; flights and DVR playback are clean.
static const char *const HINT_TEST[] = {"1. Mic: record 5s, auto playback.", "2. Line/AV: record 5s, auto playback.",
                                         "3. Test fill: pulsing = loading, red = recording, green = playback.",
                                         "4. Clicking or static during tests is normal in the menu, flight recordings are unaffected."};
#endif

/*
 * Own column layout (conf/ui.h): indent | label | button | button | spacer | value. Sliders span
 * both button columns and the spacer; ripples' Audio Source (three buttons) and test buttons span
 * them and the value column; the value column of a Record source switch shows "Mix" while that
 * source is recorded through the headphone mix. Widths checked for every language at the target's
 * fonts by utilities/audio_mixer/check_strings.py.
 */
static lv_coord_t col_dsc[] = {UI_AUDIO_COL_INDENT, UI_AUDIO_COL_LABEL, UI_AUDIO_COL_BUTTON, UI_AUDIO_COL_BUTTON,
                               UI_AUDIO_COL_SPACER, UI_AUDIO_COL_VALUE, LV_GRID_TEMPLATE_LAST};
static lv_coord_t row_dsc[ARRAY_SIZE(rows) + 1];
static lv_coord_t btn_col_dsc[] = {UI_AUDIO_ARROW_W, UI_AUDIO_COL_BUTTON - UI_AUDIO_ARROW_W, LV_GRID_TEMPLATE_LAST};
// ripples' Audio Source row: its label and three buttons, each as wide as its text, across the label
// and button columns, the spacer and the value column (some translations of "Line In" are as wide as
// two button columns)
#define SOURCE_ROW_W (UI_AUDIO_COL_LABEL + AUDIO_TEST_AREA_W)
#define SOURCE_LABEL_GAP 12
static lv_coord_t source_col_dsc[3][3];
_Static_assert(UI_AUDIO_COL_INDENT + UI_AUDIO_COL_LABEL + 2 * UI_AUDIO_COL_BUTTON + UI_AUDIO_COL_SPACER + UI_AUDIO_COL_VALUE <= UI_AUDIO_GRID_MAX_W,
               "the Audio page's columns must fit the page");

static slider_group_t sliders[ARRAY_SIZE(rows)];
static btn_group_t switches[ARRAY_SIZE(rows)];
static lv_obj_t *marks[ARRAY_SIZE(rows)];    // "Mix" of the Record source switches, "Restart" / "Error"
static lv_obj_t *headings[ARRAY_SIZE(rows)]; // heading label of each heading row
static int sel_row[ARRAY_SIZE(rows)];        // selectable index (panel) -> row
static int sel_count;
static lv_obj_t *list; // the scrolling grid
static lv_obj_t *hint;
static int focused = -1; // selectable index of the grabbed slider
static bool dvr_volume_changed; // ripples' DVR Playback moved since it was grabbed
static lv_timer_t *save_timer;

_Static_assert(ARRAY_SIZE(rows) - 5 <= MAX_PANELS, "one select panel per selectable row (at least 5 headings)");

// ---- ripples' test buttons ----
static lv_obj_t *test_container;
static lv_obj_t *test_btn[AUDIO_TEST_COUNT];
static lv_obj_t *test_label[AUDIO_TEST_COUNT];
static lv_obj_t *test_fill[AUDIO_TEST_COUNT];

typedef enum {
    AUDIO_TEST_DVR = 0,
    AUDIO_TEST_LIVE,
    AUDIO_TEST_MIC,
    AUDIO_TEST_LINE_AV,
} audio_test_mode_t;

typedef enum {
    AUDIO_TEST_PHASE_IDLE = 0,
    AUDIO_TEST_PHASE_RECORDING,
    AUDIO_TEST_PHASE_PLAYING,
} audio_test_phase_t;

static bool selected_test_active;
static volatile bool audio_test_running;
static volatile audio_test_phase_t audio_test_phase;
static volatile uint32_t audio_test_phase_duration_ms; // 0 = indeterminate
static volatile audio_test_mode_t active_test_mode = AUDIO_TEST_DVR;
static volatile audio_test_mode_t selected_test_mode = AUDIO_TEST_DVR;
static volatile bool audio_test_busy; // the last Mic / Line/AV test recorded nothing (shown instead of the note)
static uint32_t audio_test_update_ms;

static void save_timer_cb(lv_timer_t *timer) {
    lv_timer_pause(timer);
    audio_mixer_save();
}

static bool is_record_source(const row_t *row) {
    return row->type == ROW_SWITCH && audio_mixer_level_of(row->field) < AUDIO_MIXER_FIELD_COUNT;
}

// value of a switch row: 1 = left button
static int switch_value(const row_t *row) {
    switch (row->type) {
    case ROW_HDMI_OUT:
        return hdmi_audio_out_enabled();
    case ROW_HDMI_IN:
        return hdmi_audio_in_enabled();
    case ROW_RECORD_AUDIO:
        return g_setting.record.audio;
    default:
        return audio_mixer_get(row->field) != 0;
    }
}

static int slider_value(const row_t *row) {
    return row->type == ROW_DVR_VOLUME ? g_setting.record.dvr_audio_volume : audio_mixer_get(row->field);
}

static int slider_max(const row_t *row) {
    return row->type == ROW_DVR_VOLUME ? DVR_AUDIO_VOLUME_MAX : audio_mixer_max(row->field);
}

static void update_slider(int r) {
    char buf[32];
    const int value = slider_value(&rows[r]);

    lv_slider_set_value(sliders[r].slider, value, LV_ANIM_OFF);
    if (rows[r].type == ROW_DVR_VOLUME)
        snprintf(buf, sizeof(buf), "%d", value);
    else
        audio_mixer_value_str(rows[r].field, value, buf, sizeof(buf));
    lv_label_set_text(sliders[r].label, buf);
}

// The HDMI In level works only while HDMI In Audio is in effect (switched on before this start).
static bool hdmi_level_usable(const row_t *row) {
    return !(row->type == ROW_SLIDER && row->field == AUDIO_MIXER_HDMI) || hdmi_audio_in_active();
}

// A Record source that is recorded through the mix: its own Yes/No does not matter then.
static bool in_mix(const row_t *row) {
    return is_record_source(row) && audio_mixer_recorded_via_mix(row->field);
}

// HDMI Out Audio was on at this start but stayed off (the reason, translated), NULL otherwise.
static const char *hdmi_out_fault(void) {
    return hdmi_audio_out_enabled() ? hdmi_audio_out_fault() : NULL;
}

// The switch differs from what is in effect: it applies at the next start.
static bool hdmi_restart_pending(const row_t *row) {
    if (row->type == ROW_HDMI_OUT)
        return hdmi_audio_out_enabled() != hdmi_audio_out_active() && !hdmi_out_fault();
    if (row->type == ROW_HDMI_IN)
        return hdmi_audio_in_enabled() != hdmi_audio_in_active();
    return false;
}

// A line of ripples' note in the user's language: its key in the language files has '-' for '=' (a key
// cannot hold '='); without a translation, the line as it is.
static const char *note_line(const char *line, char *key, size_t size) {
    snprintf(key, size, "%s", line);
    for (char *p = key; *p; p++)
        if (*p == '=')
            *p = '-';
    const char *t = _lang(key);
    return t == key ? line : t;
}

static void row_hint(const row_t *row, char *buf, size_t size) {
    const char *text = row->hint;
    if (is_record_source(row)) {
        if (in_mix(row))
            text = HINT_IN_MIX;
        else
            text = audio_mixer_get(row->field) ? HINT_DIRECT_YES : HINT_DIRECT_NO;
    } else if (row->type == ROW_SOURCE) {
        text = audio_mixer_is_preset() ? HINT_DIRECT_YES : HINT_SOURCE_CHANGED;
    } else if (row->type == ROW_TEST && audio_test_busy) {
        snprintf(buf, size, "%s (%s, %s)", _lang(HINT_TEST_BUSY), _lang("Stream Audio"), _lang("HDMI Out Audio"));
        return;
    } else if (row->type == ROW_TEST) {
        // ripples' note, line by line
        char key[160];
        size_t len = 0;
        buf[0] = 0;
        for (size_t i = 0; i < ARRAY_SIZE(HINT_TEST) && len < size; i++)
            len += snprintf(buf + len, size - len, "%s%s", i ? "\n" : "", note_line(HINT_TEST[i], key, sizeof(key)));
        return;
    } else if (!hdmi_level_usable(row)) {
        text = hdmi_audio_in_enabled() ? HINT_HDMI_RESTART : HINT_HDMI_OFF;
    } else if (row->type == ROW_HDMI_OUT && hdmi_out_fault()) {
        snprintf(buf, size, "%s: %s", _lang(HINT_HDMI_FAULT), _lang(hdmi_out_fault()));
        return;
    } else if (row->type == ROW_HDMI_OUT && !hdmi_audio_out_allowed()) {
        text = HINT_HDMI_PROBE;
    }
    snprintf(buf, size, "%s", text ? _lang(text) : "");
}

static void update_hint(void) {
    char buf[512] = "";
    const int sel = pp_audio.p_arr.cur;
    const bool note = sel >= 0 && sel < sel_count && rows[sel_row[sel]].type == ROW_TEST;
    if (sel >= 0 && sel < sel_count)
        row_hint(&rows[sel_row[sel]], buf, sizeof(buf));
    // (ripples' note of the test buttons in its own font, as on ripples' page)
    lv_obj_set_style_text_font(hint, note ? UI_PAGE_LABEL_FONT : UI_AUDIO_HINT_FONT, 0);
    lv_label_set_text(hint, buf);
}

static void update_rows(void) {
    char buf[64];
    for (int sel = 0; sel < sel_count; sel++) {
        const int r = sel_row[sel];
        const row_t *row = &rows[r];
        switch (row->type) {
        case ROW_SLIDER:
        case ROW_LIVE_VOLUME:
        case ROW_DVR_VOLUME:
            update_slider(r);
            // (greyed, but selectable: its hint says how to switch it on)
            slider_enable(&sliders[r], hdmi_level_usable(row));
            break;
        case ROW_RECORD_AUDIO:
            btn_group_set_sel(&switches[r], switch_value(row) ? 0 : 1);
            break;
        case ROW_SOURCE:
            // ripples: greyed and skipped while Record Audio is No
            btn_group_set_sel(&switches[r], audio_mixer_get(AUDIO_MIXER_SOURCE));
            btn_group_enable(&switches[r], g_setting.record.audio);
            if (g_setting.record.audio)
                lv_obj_add_flag(pp_audio.p_arr.panel[sel], FLAG_SELECTABLE);
            else
                lv_obj_clear_flag(pp_audio.p_arr.panel[sel], FLAG_SELECTABLE);
            break;
        case ROW_SWITCH:
            // a source recorded through the mix: recorded, so Yes, greyed (the row never shows
            // "No" next to "Mix"); its own setting comes back when it leaves the mix
            btn_group_set_sel(&switches[r], (in_mix(row) || switch_value(row)) ? 0 : 1);
            btn_group_enable(&switches[r], !in_mix(row));
            if (marks[r])
                lv_label_set_text(marks[r], in_mix(row) ? _lang("Mix") : "");
            break;
        case ROW_HDMI_OUT:
        case ROW_HDMI_IN:
            btn_group_set_sel(&switches[r], switch_value(row) ? 0 : 1);
            // HDMI Out Audio: only with the probe's GO for this board (one that is on can be switched off)
            if (row->type == ROW_HDMI_OUT)
                btn_group_enable(&switches[r], hdmi_audio_out_allowed() || hdmi_audio_out_enabled());
            buf[0] = 0;
            if (row->type == ROW_HDMI_OUT && hdmi_out_fault())
                snprintf(buf, sizeof(buf), "#FFA000 %s#", _lang(MARK_ERROR));
            else if (hdmi_restart_pending(row))
                snprintf(buf, sizeof(buf), "#FFA000 %s#", _lang(MARK_RESTART));
            lv_label_set_text(marks[r], buf);
            break;
        default:
            break;
        }
    }
    update_hint();
}

// Keep the selected row on screen, with its section heading when it is the first row of a section.
// (set_select_item() already scrolls the selected panel into view; this adds the heading.)
static void scroll_to_selection(void) {
    const int sel = pp_audio.p_arr.cur;
    if (sel < 0 || sel >= sel_count)
        return;
    const int r = sel_row[sel];
    if (r > 0 && rows[r - 1].type == ROW_HEADING)
        lv_obj_scroll_to_view(headings[r - 1], LV_ANIM_OFF);
    lv_obj_scroll_to_view(pp_audio.p_arr.panel[sel], LV_ANIM_OFF);
}

static lv_obj_t *create_heading(lv_obj_t *parent, const char *text, int r) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, _lang(text));
    lv_obj_set_style_text_font(label, UI_AUDIO_HEADING_FONT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_AUDIO_HEADING_COLOR), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    // the whole row (scrolling it into view then scrolls by whole rows), the text at its bottom
    // with a thin line under it, across the row
    lv_obj_set_size(label, UI_AUDIO_COL_LABEL + 2 * UI_AUDIO_COL_BUTTON + UI_AUDIO_COL_SPACER + UI_AUDIO_COL_VALUE, UI_AUDIO_ROW_H);
    lv_obj_set_style_pad_bottom(label, 2, 0);
    lv_obj_set_style_pad_top(label, UI_AUDIO_ROW_H - 2 - lv_font_get_line_height(UI_AUDIO_HEADING_FONT), 0);
    lv_obj_set_style_border_side(label, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(label, 1, 0);
    lv_obj_set_style_border_color(label, lv_color_hex(UI_AUDIO_HEADING_COLOR), 0);
    lv_obj_set_style_border_opa(label, LV_OPA_50, 0);
    lv_obj_set_grid_cell(label, LV_GRID_ALIGN_START, 1, 5, LV_GRID_ALIGN_START, r, 1);
    return label;
}

static void style_button(btn_with_arr_t *b, lv_coord_t width, const lv_coord_t *cols) {
    lv_obj_set_width(b->container, width);
    lv_obj_set_style_grid_column_dsc_array(b->container, cols, 0);
    lv_obj_set_width(b->btn, width - UI_AUDIO_ARROW_W);
    lv_obj_set_style_pad_left(b->btn, UI_AUDIO_BTN_PAD, 0);
    lv_obj_set_style_pad_right(b->btn, 0, 0);
}

static void create_switch(int r) {
    const row_t *row = &rows[r];
    btn_group_t *t = &switches[r];
    create_btn_group_item(t, list, 2, _lang(row->name), _lang(row->btn0), _lang(row->btn1), "", "", r);
    lv_obj_set_grid_cell(t->label, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_CENTER, r, 1);
    lv_obj_set_width(t->label, UI_AUDIO_COL_LABEL);
    for (int i = 0; i < 2; i++)
        style_button(&t->btn_a[i], UI_AUDIO_COL_BUTTON, btn_col_dsc);
    // value column: "Mix" of a Record source, "Restart" / "Error" (orange) of the HDMI switches
    if (is_record_source(row) || row->type == ROW_HDMI_OUT || row->type == ROW_HDMI_IN) {
        marks[r] = lv_label_create(list);
        lv_label_set_text(marks[r], "");
        lv_label_set_recolor(marks[r], true);
        lv_obj_set_style_text_font(marks[r], UI_PAGE_TEXT_FONT, 0);
        lv_obj_set_style_text_color(marks[r], lv_color_hex(UI_AUDIO_HEADING_COLOR), 0);
        lv_obj_set_style_pad_top(marks[r], UI_PAGE_TEXT_PAD, 0);
        lv_obj_set_size(marks[r], UI_AUDIO_COL_VALUE, UI_AUDIO_ROW_H);
        lv_label_set_long_mode(marks[r], LV_LABEL_LONG_DOT);
        lv_obj_set_grid_cell(marks[r], LV_GRID_ALIGN_START, 5, 1, LV_GRID_ALIGN_CENTER, r, 1);
    }
}

static lv_coord_t text_width(const char *text, const lv_font_t *font) {
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// ripples' Audio Source: its label and three buttons in a row of their own widths (a button of the
// width of its text, as the other buttons of the page leave room for theirs), from the label column
// to the value column. utilities/audio_mixer/check_strings.py checks that the row fits.
static void create_source(int r) {
    btn_group_t *t = &switches[r];
    const char *names[3] = {_lang("Mic"), _lang("Line In"), _lang("A/V In")};
    create_btn_group_item(t, list, 3, _lang(rows[r].name), names[0], names[1], names[2], "", r);
    lv_obj_t *box = lv_obj_create(list);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, SOURCE_ROW_W, UI_AUDIO_ROW_H);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_grid_cell(box, LV_GRID_ALIGN_START, 1, 5, LV_GRID_ALIGN_CENTER, r, 1);
    lv_obj_set_parent(t->label, box);
    lv_obj_set_width(t->label, text_width(lv_label_get_text(t->label), UI_PAGE_TEXT_FONT) + SOURCE_LABEL_GAP);
    for (int i = 0; i < 3; i++) {
        const lv_coord_t w = UI_AUDIO_ARROW_W + UI_AUDIO_BTN_PAD + text_width(names[i], UI_PAGE_TEXT_FONT) + SOURCE_LABEL_GAP;
        source_col_dsc[i][0] = UI_AUDIO_ARROW_W;
        source_col_dsc[i][1] = w - UI_AUDIO_ARROW_W;
        source_col_dsc[i][2] = LV_GRID_TEMPLATE_LAST;
        lv_obj_set_parent(t->btn_a[i].container, box);
        style_button(&t->btn_a[i], w, source_col_dsc[i]);
    }
}

static void create_test_button(lv_obj_t *parent, audio_test_mode_t mode, const char *name) {
    test_btn[mode] = lv_btn_create(parent);
    lv_obj_set_size(test_btn[mode], AUDIO_TEST_BUTTON_W, UI_AUDIO_TEST_BUTTON_HEIGHT);
    lv_obj_set_pos(test_btn[mode], mode * (AUDIO_TEST_BUTTON_W + UI_AUDIO_TEST_BUTTON_GAP), 0);
    // Solid dark-grey base with rounded corners, matching the Scan Now mode
    // buttons. The focus (green) / border state is applied per-frame in
    // page_audio_update_test_indicator. clip_corner keeps the progress fill
    // child inside the rounded outline.
    lv_obj_set_style_bg_color(test_btn[mode], lv_color_hex(UI_AUDIO_TEST_BTN_BG), 0);
    lv_obj_set_style_bg_opa(test_btn[mode], LV_OPA_100, 0);
    lv_obj_set_style_shadow_width(test_btn[mode], 0, 0);
    lv_obj_set_style_border_width(test_btn[mode], 0, 0);
    lv_obj_set_style_border_color(test_btn[mode], lv_color_make(0xff, 0xff, 0xff), 0);
    lv_obj_set_style_radius(test_btn[mode], 6, 0);
    lv_obj_set_style_clip_corner(test_btn[mode], true, 0);
    lv_obj_set_style_pad_all(test_btn[mode], 0, 0);

    // Progress fill: grows left-to-right behind the label while a test runs
    // (red while recording, green while playing back). Created before the
    // label so the text stays on top.
    test_fill[mode] = lv_obj_create(test_btn[mode]);
    lv_obj_remove_style_all(test_fill[mode]);
    lv_obj_set_size(test_fill[mode], 0, UI_AUDIO_TEST_BUTTON_HEIGHT);
    lv_obj_align(test_fill[mode], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(test_fill[mode], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(test_fill[mode], LV_OBJ_FLAG_SCROLLABLE);

    test_label[mode] = lv_label_create(test_btn[mode]);
    lv_label_set_text(test_label[mode], name);
    lv_obj_set_style_text_font(test_label[mode], UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_color(test_label[mode], lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_center(test_label[mode]);
}

static void create_tests(int r) {
    create_label_item(list, _lang(rows[r].name), 1, r, 1);
    test_container = lv_obj_create(list);
    lv_obj_set_size(test_container, AUDIO_TEST_AREA_W, UI_AUDIO_TEST_BUTTON_HEIGHT);
    lv_obj_clear_flag(test_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(test_container, &style_context, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(test_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(test_container, 0, 0);
    lv_obj_set_style_pad_all(test_container, 0, 0);
    lv_obj_set_grid_cell(test_container, LV_GRID_ALIGN_START, 2, 4, LV_GRID_ALIGN_CENTER, r, 1);
    create_test_button(test_container, AUDIO_TEST_DVR, _lang("DVR"));
    create_test_button(test_container, AUDIO_TEST_LIVE, _lang("Live"));
    create_test_button(test_container, AUDIO_TEST_MIC, _lang("Mic"));
    create_test_button(test_container, AUDIO_TEST_LINE_AV, _lang("Line/AV"));
}

static lv_obj_t *page_audio_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[128];
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);
    lv_obj_set_style_pad_top(section, UI_AUDIO_PAD_TOP, 0);

    snprintf(buf, sizeof(buf), "%s:", _lang(pp_audio.name));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    // The rows: taller than the window, scrolled only by the selection (lv_obj_scroll_to_view).
    list = lv_obj_create(section);
    lv_obj_set_size(list, lv_pct(100), UI_AUDIO_VIEW_H);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_layout(list, LV_LAYOUT_GRID);
    lv_obj_add_style(list, &style_context, LV_PART_MAIN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    // a scroll bar shows that there is more
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_bg_color(list, lv_color_hex(TEXT_COLOR_DISABLE), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    // every row the same height, headings too: scrolled by whole rows, the window never shows half a row
    for (int r = 0; r < ROW_COUNT; r++)
        row_dsc[r] = UI_AUDIO_ROW_H;
    _Static_assert(UI_AUDIO_VIEW_H % UI_AUDIO_ROW_H == 0, "the window shows whole rows");
    row_dsc[ROW_COUNT] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_style_grid_column_dsc_array(list, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(list, row_dsc, 0);
    // no column gaps (the columns are sized for the longest translation), no row gaps
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_style_pad_column(list, 0, 0);

    create_select_item(arr, list);

    sel_count = 0;
    for (int r = 0; r < ROW_COUNT; r++) {
        const row_t *row = &rows[r];
        if (row->type == ROW_HEADING) {
            headings[r] = create_heading(list, row->name, r);
            continue;
        }

        // this row's selection panel
        const int sel = sel_count++;
        sel_row[sel] = r;
        lv_obj_set_grid_cell(arr->panel[sel], LV_GRID_ALIGN_STRETCH, 0, 6, LV_GRID_ALIGN_STRETCH, r, 1);

        switch (row->type) {
        case ROW_SLIDER:
        case ROW_LIVE_VOLUME:
        case ROW_DVR_VOLUME: {
            slider_group_t *s = &sliders[r];
            create_slider_item(s, list, _lang(row->name), slider_max(row), 0, r);
            lv_obj_set_grid_cell(s->name, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_CENTER, r, 1);
            lv_obj_set_width(s->name, UI_AUDIO_COL_LABEL);
            lv_obj_set_grid_cell(s->slider, LV_GRID_ALIGN_START, 2, 3, LV_GRID_ALIGN_CENTER, r, 1);
            lv_obj_set_width(s->slider, 2 * UI_AUDIO_COL_BUTTON + UI_AUDIO_COL_SPACER - UI_AUDIO_SLIDER_GAP);
            lv_obj_set_width(s->label, UI_AUDIO_COL_VALUE);
            // the knob stays inside its row: nothing of a row scrolled out shows at the window's edge
            lv_obj_set_style_pad_ver(s->slider, UI_AUDIO_KNOB_PAD, LV_PART_KNOB);
            break;
        }
        case ROW_SWITCH:
        case ROW_RECORD_AUDIO:
        case ROW_HDMI_OUT:
        case ROW_HDMI_IN:
            create_switch(r);
            break;
        case ROW_SOURCE:
            create_source(r);
            break;
        case ROW_TEST:
            create_tests(r);
            break;
        case ROW_BACK:
            snprintf(buf, sizeof(buf), "< %s", _lang(row->name));
            create_label_item(list, buf, 1, r, 1);
            break;
        default:
            break;
        }
    }

    // the help for the selected row, fixed below the list (up to UI_AUDIO_HINT_LINES lines: ripples'
    // note of the test buttons)
    hint = lv_label_create(section);
    lv_label_set_text(hint, "");
    lv_obj_set_style_text_font(hint, UI_AUDIO_HINT_FONT, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(hint, UI_AUDIO_HINT_PAD, 0);
    lv_obj_set_style_pad_left(hint, UI_AUDIO_COL_INDENT, 0);
    lv_obj_set_width(hint, UI_AUDIO_COL_INDENT + UI_AUDIO_COL_LABEL + 2 * UI_AUDIO_COL_BUTTON + UI_AUDIO_COL_SPACER + UI_AUDIO_COL_VALUE);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    pp_audio.p_arr.max = sel_count;
    update_rows();
    // no hint while the page is only previewed from the main menu
    lv_label_set_text(hint, "");

    // runs in lv_timer_handler(), under lvgl_mutex like the input handlers
    save_timer = lv_timer_create(save_timer_cb, SAVE_DELAY_MS, NULL);
    lv_timer_pause(save_timer);

    return page;
}

// ---- ripples' test buttons (src/ui/page_audio.c of ripples 4fbb865; the mixer held meanwhile) ----

static void page_audio_update_test_indicator(uint32_t delta_ms) {
    static audio_test_phase_t prev_phase = AUDIO_TEST_PHASE_IDLE;
    static bool prev_running = false;
    static uint32_t phase_elapsed_ms = 0;
    bool blink_on;
    lv_color_t record_color = lv_color_make(0xff, 0x20, 0x20);
    lv_color_t play_color = lv_color_make(0, 0xc0, 0);
    lv_color_t idle_color = lv_color_hex(UI_AUDIO_TEST_BTN_BG);
    lv_color_t focus_color = lv_color_make(0, 0xa0, 0);
    lv_color_t white_color = lv_color_make(0xff, 0xff, 0xff);

    audio_test_update_ms += delta_ms;
    blink_on = (audio_test_update_ms / 250) % 2 == 0;

    // Track how long the current phase has been showing so the fill can grow
    // in step with the known phase duration.
    audio_test_phase_t phase = audio_test_phase;
    bool running = audio_test_running;
    static bool prev_busy = false;
    if (audio_test_busy != prev_busy) {
        prev_busy = audio_test_busy;
        update_hint();
    }
    if (phase != prev_phase || running != prev_running) {
        phase_elapsed_ms = 0;
        prev_phase = phase;
        prev_running = running;
    } else {
        phase_elapsed_ms += delta_ms;
    }

    for (int i = 0; i < AUDIO_TEST_COUNT; i++) {
        // Focus highlight (green base + white border, Scan Now style) shows
        // only while the Run Test row holds focus. Once a test is clicked
        // (selected_test_active clears) or the page is left, every button
        // falls back to idle grey -- so the last-tested button no longer
        // stays highlighted.
        bool is_focused = selected_test_active && i == (int)selected_test_mode;
        bool is_running = running && i == (int)active_test_mode;
        lv_color_t bg_color = is_focused ? focus_color : idle_color;
        lv_coord_t border_w = is_focused ? 2 : 0;
        lv_coord_t fill_w = 0;
        lv_color_t fill_color = idle_color;
        lv_opa_t fill_opa = LV_OPA_TRANSP;

        if (is_running) {
            if (phase == AUDIO_TEST_PHASE_RECORDING || phase == AUDIO_TEST_PHASE_PLAYING) {
                // Scan-bar style: red fills while recording, green while
                // playing back, growing left-to-right over the phase length.
                // Opaque so it reads cleanly over the grey button base.
                uint32_t dur = audio_test_phase_duration_ms;
                uint32_t el = (dur && phase_elapsed_ms > dur) ? dur : phase_elapsed_ms;
                fill_color = (phase == AUDIO_TEST_PHASE_RECORDING) ? record_color : play_color;
                fill_opa = LV_OPA_COVER;
                fill_w = dur ? (lv_coord_t)((uint64_t)AUDIO_TEST_FILL_MAX_W * el / dur)
                             : AUDIO_TEST_FILL_MAX_W;
            } else {
                // Setting up (audio routing before the first phase starts,
                // noticeable on the Mic test): pulse a dim full-width fill so
                // the wait reads as activity, not a hang.
                fill_color = white_color;
                fill_opa = blink_on ? LV_OPA_40 : LV_OPA_20;
                fill_w = AUDIO_TEST_FILL_MAX_W;
            }
        }
        lv_obj_set_style_text_color(test_label[i], white_color, 0);
        lv_obj_set_style_bg_color(test_btn[i], bg_color, 0);
        lv_obj_set_style_bg_opa(test_btn[i], LV_OPA_100, 0);
        lv_obj_set_style_border_width(test_btn[i], border_w, 0);
        if (test_fill[i]) {
            lv_obj_set_width(test_fill[i], fill_w);
            lv_obj_set_style_bg_color(test_fill[i], fill_color, 0);
            lv_obj_set_style_bg_opa(test_fill[i], fill_opa, 0);
        }
    }
}

static void page_audio_play_wav(const char *path) {
#if defined(HDZGOGGLE)
    // MPI AO engine, not aplay: the ALSA hw path renders static-laden audio
    // on the G1's kernel no matter how it is buffered, while the engine that
    // plays normal DVR files is clean on every target.
    wav_test_play(path);
#else
    // BoxPro/G2 keep the ALSA transport: plughw with deep buffers (0.5s,
    // 125ms periods) is clean on their kernel, and these tests shipped
    // working this way.
    char buf[256];
    snprintf(buf, sizeof(buf), "%s -D plughw:audiocodec -B 500000 -F 125000 %s",
             AUDIO_TEST_APLAY, path);
    system_exec(buf);
#endif
}

static void page_audio_enable_dac_playback(void) {
    char buf[128];

    snprintf(buf, sizeof(buf), "%s out_on", AUDIO_SEL_SH);
    system_exec(buf);
#if defined(HDZGOGGLE)
    // Replicate the Playback menu's codec state exactly -- the only playback
    // route that is clean on the G1. It never enables the Output Mixer DAC
    // path (out_dac_on): the DAC reaches LINEOUT through the LINEOUT mux
    // alone. Every static-laden test playback since the first version had
    // out_dac_on in common, across both the ALSA and the MPI transports.
    dvr_mute_live_audio(); // cut LINEIN from the output mixer, like adec2ao
    dvr_set_dvr_audio_volume(g_setting.record.dvr_audio_volume);
#else
    snprintf(buf, sizeof(buf), "%s out_linein_off", AUDIO_SEL_SH);
    system_exec(buf);
    // Cut the remaining analog inputs to the output mixer too: a MIC boost
    // stage left enabled by the board's boot state feeds open-mic noise to
    // the lineout whenever it is on.
    snprintf(buf, sizeof(buf), "%s out_mic1_off", AUDIO_SEL_SH);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "%s out_mic2_off", AUDIO_SEL_SH);
    system_exec(buf);
    snprintf(buf, sizeof(buf), "%s out_dac_on", AUDIO_SEL_SH);
    system_exec(buf);
    // aplay drives AIF1 timeslot 0 only; force the documented clean playback
    // route (record/audio/audiocodec/audio-setup.txt). The MPI AO engine sets
    // its own codec route up (which is why normal DVR playback is clean), but
    // a raw aplay inherits whatever DAC-mixer state the board booted with --
    // an enabled ADC sidetone or an idle timeslot-1 input mixes noise
    // straight into the DAC, heard as static under the test sample.
    system_exec("amixer cset name='DACL Mixer AIF1DA0L Switch' 1");
    system_exec("amixer cset name='DACR Mixer AIF1DA0R Switch' 1");
    system_exec("amixer cset name='DACL Mixer ADCL Switch' 0");
    system_exec("amixer cset name='DACR Mixer ADCR Switch' 0");
    dvr_set_dvr_audio_volume(g_setting.record.dvr_audio_volume);
    // dvr_set_dvr_audio_volume raises both timeslot volumes; keep the unused
    // timeslot 1 silent while the test plays (the next real playback or test
    // restores it).
    system_exec("amixer cset name='AIF1 DAC timeslot 1 volume' 0,0");
#endif
}

static const char *page_audio_test_sample_path(void) {
    if (fs_file_exists(AUDIO_TEST_SAMPLE_SDCARD))
        return AUDIO_TEST_SAMPLE_SDCARD;
    return AUDIO_TEST_SAMPLE;
}

static const char *page_audio_capture_path(void) {
    return AUDIO_TEST_CAPTURE;
}

// true: something was recorded (more than a WAV header)
static bool page_audio_capture_wav(setting_record_audio_source_t source) {
    bool ok;
    dvr_test_select_audio_source(source);
    audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
    audio_test_phase = AUDIO_TEST_PHASE_RECORDING;
#if defined(HDZGOGGLE)
    // MPI AI engine, not arecord: the captured clips themselves carried the
    // static on the G1 (confirmed by playing them on a computer) regardless
    // of ALSA device or buffering, while normal DVR recordings -- made by
    // this engine -- are clean.
    ok = wav_test_record(page_audio_capture_path(), AUDIO_TEST_SECONDS) == 0;
#else
    // BoxPro/G2: ALSA capture is clean on their kernel (see
    // page_audio_play_wav).
    char buf[256];
    snprintf(buf, sizeof(buf), "%s -D plughw:audiocodec -B 500000 -F 125000 -t wav -f S16_LE -c2 -r 48000 -d %d %s",
             AUDIO_TEST_ARECORD, AUDIO_TEST_SECONDS, page_audio_capture_path());
    ok = system_exec(buf) == 0;
#endif
    // The record daemon holds the codec's capture while Stream Audio streams or HDMI Out Audio runs its
    // bridge (docs/live-audio.md): the capture fails then, and there is nothing to play back
    if (!ok || fs_filesize(page_audio_capture_path()) <= 44) {
        LOGE("audio test: nothing recorded (the audio input busy: Stream Audio, HDMI Out Audio?)");
        return false;
    }
    return true;
}

static void *page_audio_test_thread(void *arg) {
    audio_test_mode_t mode = (audio_test_mode_t)(intptr_t)arg;

    // No RX handling on BoxPro/G2: the internal analog module's audio only
    // reaches the codec in analog video mode (a wired AV-in source plays
    // through these tests in the menu, a powered+tuned RX does not), so
    // powering it up here just made heat. From the menu the Live/Line-AV
    // tests cover the wired Line In / A/V In path.

    // ripples' own codec code from here on; the mixer neither applies nor watches meanwhile, and
    // puts the page's state back afterwards (ripples restored its source and line out itself)
    audio_mixer_hold(AUDIO_MIXER_HOLD_TEST, true);
    active_test_mode = mode;
    switch (mode) {
    case AUDIO_TEST_MIC:
        dvr_mute_live_audio();
        if (!page_audio_capture_wav(SETTING_RECORD_AUDIO_SOURCE_MIC)) {
            audio_test_busy = true;
            break;
        }
        page_audio_enable_dac_playback();
        audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        page_audio_play_wav(page_audio_capture_path());
        break;
    case AUDIO_TEST_LIVE: {
        dvr_test_select_audio_source(SETTING_RECORD_AUDIO_SOURCE_LINE_IN);
        dvr_test_line_out(true);
        dvr_set_live_audio_volume(g_setting.record.live_audio_volume);
        audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        sleep(AUDIO_TEST_SECONDS);
        break;
    }
    case AUDIO_TEST_LINE_AV:
        dvr_mute_live_audio();
        if (!page_audio_capture_wav(SETTING_RECORD_AUDIO_SOURCE_LINE_IN)) {
            audio_test_busy = true;
            break;
        }
        page_audio_enable_dac_playback();
        audio_test_phase_duration_ms = AUDIO_TEST_SECONDS * 1000;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        page_audio_play_wav(page_audio_capture_path());
        break;
    case AUDIO_TEST_DVR:
        dvr_mute_live_audio();
        page_audio_enable_dac_playback();
        audio_test_phase_duration_ms = AUDIO_TEST_DVR_SAMPLE_MS;
        audio_test_phase = AUDIO_TEST_PHASE_PLAYING;
        page_audio_play_wav(page_audio_test_sample_path());
        break;
    }
    // the page's routing again: record sources, line out, the output mixers (DAC out of them)
    audio_mixer_hold(AUDIO_MIXER_HOLD_TEST, false);

    audio_test_phase = AUDIO_TEST_PHASE_IDLE;
    audio_test_running = false;
    return NULL;
}

static void page_audio_run_test(void) {
    pthread_t tid;
    audio_test_mode_t mode = selected_test_mode;

    // tests reroute the record audio mux/gains, which would corrupt an active recording
    if (audio_test_running || dvr_is_recording)
        return;

    active_test_mode = mode;
    audio_test_phase = AUDIO_TEST_PHASE_IDLE;
    audio_test_update_ms = 0;
    audio_test_busy = false;
    audio_test_running = true;
    if (pthread_create(&tid, NULL, page_audio_test_thread, (void *)(intptr_t)mode) == 0) {
        pthread_detach(tid);
    } else {
        audio_test_running = false;
    }
}

// ---- input ----

static void release_slider(bool back_to_submenu) {
    if (focused < 0)
        return;

    const row_t *row = &rows[sel_row[focused]];
    lv_obj_remove_style(sliders[sel_row[focused]].slider, &style_silder_select, LV_PART_MAIN);
    if (row->type == ROW_DVR_VOLUME && dvr_volume_changed) {
        // ripples: stored on release, used by the next playback
        ini_putl("record", "dvr_audio_volume_v2", g_setting.record.dvr_audio_volume, SETTING_INI);
        dvr_volume_changed = false;
    }
    focused = -1;
    lv_timer_pause(save_timer);
    audio_mixer_save();
    if (back_to_submenu)
        app_state_push(APP_STATE_SUBMENU);
}

static void page_audio_enter(void) {
    // from the top, with the first heading
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    update_rows();
    scroll_to_selection();
}

static void page_audio_exit(void) {
    // submenu_exit() has already set the main menu state
    release_slider(false);
    audio_mixer_save();
    selected_test_active = false;
    audio_test_busy = false;
    // previewed from the main menu as when it was first shown: from the top, no hint
    lv_label_set_text(hint, "");
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

// a mixer value of ripples' (Live Volume, the gains) also in g_setting.record, which ripples' code reads
static void mirror_ripples(void) {
    g_setting.record.live_audio_volume = audio_mixer_get(AUDIO_MIXER_LIVE_VOLUME);
    g_setting.record.mic_gain = audio_mixer_get(AUDIO_MIXER_MIC_GAIN);
    g_setting.record.linein_gain = audio_mixer_get(AUDIO_MIXER_LINEIN_GAIN);
    g_setting.record.audio_source = audio_mixer_get(AUDIO_MIXER_SOURCE);
}

static void page_audio_on_roller(uint8_t key) {
    if (selected_test_active) {
        // ripples: the dial picks a test button while Run Test holds the focus
        if (key == DIAL_KEY_UP) {
            selected_test_mode = selected_test_mode == 0 ? AUDIO_TEST_COUNT - 1 : selected_test_mode - 1;
        } else if (key == DIAL_KEY_DOWN) {
            selected_test_mode = (selected_test_mode + 1) % AUDIO_TEST_COUNT;
        }
        return;
    }
    if (g_app_state != APP_STATE_SUBMENU_ITEM_FOCUSED || focused < 0) {
        // the selection moved
        scroll_to_selection();
        update_hint();
        return;
    }

    const row_t *row = &rows[sel_row[focused]];
    int value = slider_value(row);

    // same direction as the other slider pages
    if (key == DIAL_KEY_UP && value > 0) {
        value--;
    } else if (key == DIAL_KEY_DOWN && value < slider_max(row)) {
        value++;
    } else {
        return;
    }

    if (row->type == ROW_DVR_VOLUME) {
        g_setting.record.dvr_audio_volume = value;
        dvr_volume_changed = true;
        update_rows();
        return;
    }
    if (row->type == ROW_LIVE_VOLUME)
        audio_mixer_set_live_volume(value);
    else
        audio_mixer_set(row->field, value);
    mirror_ripples();
    // the "Mix" marks, the A/V In level (Live Volume) and the hint follow
    update_rows();
    audio_mixer_apply();
    // saved on release, or once the dial rests
    lv_timer_reset(save_timer);
    lv_timer_resume(save_timer);
}

static void page_audio_on_click(uint8_t key, int sel) {
    (void)key;

    if (selected_test_active) {
        if (audio_test_running)
            return;
        selected_test_active = false;
        app_state_push(APP_STATE_SUBMENU);
        page_audio_run_test();
        return;
    }

    if (sel < 0 || sel >= sel_count) {
        return;
    }

    const int r = sel_row[sel];
    const row_t *row = &rows[r];

    switch (row->type) {
    case ROW_SLIDER:
    case ROW_LIVE_VOLUME:
    case ROW_DVR_VOLUME:
        if (!hdmi_level_usable(row))
            break; // (greyed: the hint says why)
        if (focused == sel) {
            release_slider(true);
        } else {
            release_slider(false);
            focused = sel;
            dvr_volume_changed = false;
            app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
            lv_obj_add_style(sliders[r].slider, &style_silder_select, LV_PART_MAIN);
        }
        break;
    case ROW_RECORD_AUDIO:
        // ripples
        g_setting.record.audio = !g_setting.record.audio;
        settings_put_bool("record", "audio", g_setting.record.audio);
        update_rows();
        break;
    case ROW_SOURCE:
        // ripples' Audio Source, the next one: the preset of the Record switches below
        audio_mixer_set_source((audio_mixer_get(AUDIO_MIXER_SOURCE) + 1) % 3);
        mirror_ripples();
        audio_mixer_save();
        update_rows();
        audio_mixer_apply();
        break;
    case ROW_SWITCH:
        if (in_mix(row))
            break; // (greyed: recorded through the mix whatever it says)
        audio_mixer_set(row->field, !switch_value(row));
        audio_mixer_save();
        update_rows();
        audio_mixer_apply();
        break;
    case ROW_TEST:
        if (audio_test_running || dvr_is_recording)
            return;
        selected_test_active = true;
        app_state_push(APP_STATE_SUBMENU_ITEM_FOCUSED);
        break;
    case ROW_HDMI_OUT:
        // On only with the read-only probe's GO for this board (its hint says so); Off always
        if (!hdmi_audio_out_enabled() && !hdmi_audio_out_allowed())
            break;
        // stored only: in effect from the next start (core/hdmi_audio.h)
        hdmi_audio_set(!hdmi_audio_out_enabled(), hdmi_audio_in_enabled());
        update_rows();
        break;
    case ROW_HDMI_IN:
        hdmi_audio_set(hdmi_audio_out_enabled(), !hdmi_audio_in_enabled());
        update_rows();
        break;
    default:
        break;
    }
}

page_pack_t pp_audio = {
    .p_arr = {
        .cur = 0,
        .max = 0, // the selectable rows, counted by page_audio_create()
    },
    .name = "Audio",
    .create = page_audio_create,
    .enter = page_audio_enter,
    .exit = page_audio_exit,
    .on_created = NULL,
    .on_update = page_audio_update_test_indicator,
    .on_roller = page_audio_on_roller,
    .on_click = page_audio_on_click,
    .on_right_button = NULL,
};
