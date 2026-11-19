/*
 * Measures the strings of the Audio page, the Module Bay page and the new WiFi rows with the
 * firmware's LVGL fonts (check_strings.py builds it once per target with conf/ui.h). Input, one
 * per line: "<kind>\t<lang>\t<text>", kind one of label, button, source_row (label|b|b|b), test_button,
 * value, heading, hint, note (Audio page), mb_label, mb_text, mb_status, mb_option, mb_note (Module Bay page, goggles only),
 * wifi_label, wifi_note (WiFi page). Output, one per line:
 *   "OK|WIDE|GLYPH <kind> <lang> <width>/<room> px <text>"
 * WIDE: wider than its column, GLYPH: a character the font does not have (shown as a box or
 * nothing: the CJK fonts only hold the characters of the translations they were made from).
 * Exit status: 0 all OK, 1 something WIDE / GLYPH / of an unknown kind.
 */

#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "../../conf/ui.h"

#define ROW_W (UI_AUDIO_COL_LABEL + 2 * UI_AUDIO_COL_BUTTON + UI_AUDIO_COL_SPACER + UI_AUDIO_COL_VALUE)
// ripples' Audio Source and test buttons across the button columns, the spacer and the value (page_audio.c)
#define AUDIO_TEST_AREA_W (2 * UI_AUDIO_COL_BUTTON + UI_AUDIO_COL_SPACER + UI_AUDIO_COL_VALUE)
#define AUDIO_TEST_BUTTON_W ((AUDIO_TEST_AREA_W - 3 * UI_AUDIO_TEST_BUTTON_GAP) / 4)
#define SOURCE_ROW_W (UI_AUDIO_COL_LABEL + AUDIO_TEST_AREA_W)
#define SOURCE_LABEL_GAP 12

// lv_conf.h's tick source (the firmware's util/time.c); not used for measuring
uint32_t time_ms(void) {
    return 0;
}

// what create_label/create_slider/create_btn_group leave for the text, per kind
static int room(const char *kind, const lv_font_t **font) {
    *font = UI_PAGE_TEXT_FONT;
    if (strcmp(kind, "label") == 0)
        return UI_AUDIO_COL_LABEL - 8; // the label column, a few pixels to the slider / buttons
    if (strcmp(kind, "button") == 0)
        return UI_AUDIO_COL_BUTTON - UI_AUDIO_ARROW_W - UI_AUDIO_BTN_PAD;
    if (strcmp(kind, "source_row") == 0)
        return SOURCE_ROW_W; // label|button|button|button, see main()
    if (strcmp(kind, "note") == 0) {
        *font = UI_PAGE_LABEL_FONT; // ripples' note of the test buttons, a line of it
        return ROW_W;
    }
    if (strcmp(kind, "test_button") == 0) {
        *font = UI_PAGE_LABEL_FONT;
        return AUDIO_TEST_BUTTON_W - 8; // centred, a few pixels from the edges
    }
    if (strcmp(kind, "value") == 0)
        return UI_AUDIO_COL_VALUE;
    if (strcmp(kind, "heading") == 0) {
        *font = UI_AUDIO_HEADING_FONT;
        return ROW_W;
    }
    if (strcmp(kind, "hint") == 0) {
        *font = UI_AUDIO_HINT_FONT;
        return ROW_W;
    }
    // Module Bay page (src/ui/page_modulebay.c: goggles only, fixed sizes there)
    if (strcmp(kind, "mb_label") == 0)
        return 200 - 8; // column 1
    if (strcmp(kind, "mb_text") == 0)
        return 610; // Check Pins result, Mode note: up to the page's right edge
    if (strcmp(kind, "mb_status") == 0)
        return 880; // "Status: ..." from column 1 to the page's right edge
    if (strcmp(kind, "mb_option") == 0) {
        *font = &lv_font_montserrat_26;
        return 420 - 60; // the 420 px dropdowns (MB_DROPDOWN_W) without their padding and arrow
    }
    if (strcmp(kind, "mb_note") == 0) {
        *font = UI_PAGE_LABEL_FONT;
        return 880;
    }
    // WiFi page, System: a row label up to the first button's arrow (measured on the emulator)
    if (strcmp(kind, "wifi_label") == 0) {
#ifdef HDZBOXPRO
        return 132;
#else
        return 200;
#endif
    }
    if (strcmp(kind, "wifi_note") == 0) {
        *font = UI_PAGE_LABEL_FONT;
#ifdef HDZBOXPRO
        return 600;
#else
        return 880;
#endif
    }
    return -1;
}

int main(void) {
    char line[1024];
    int bad = 0;

    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *kind = line;
        char *lang = strchr(kind, '\t');
        if (!lang)
            continue;
        *lang++ = 0;
        char *text = strchr(lang, '\t');
        if (!text)
            continue;
        *text++ = 0;

        const lv_font_t *font;
        const int max = room(kind, &font);
        if (max < 0) {
            printf("BAD-KIND %s\n", kind);
            bad++;
            continue;
        }

        // glyphs: every code point must be in the font
        char missing[256] = "";
        uint32_t i = 0;
        while (text[i]) {
            const uint32_t letter = _lv_txt_encoded_next(text, &i);
            lv_font_glyph_dsc_t dsc;
            if (letter >= 0x20 && !lv_font_get_glyph_dsc(font, &dsc, letter, 0)) {
                const size_t n = strlen(missing);
                snprintf(missing + n, sizeof(missing) - n, " U+%04X", (unsigned)letter);
            }
        }

        lv_point_t size;
        if (strcmp(kind, "source_row") == 0) {
            // ripples' Audio Source row (page_audio.c create_source()): the label and three buttons,
            // each its text + SOURCE_LABEL_GAP, the buttons also their arrow and padding
            size.x = 0;
            int part = 0;
            for (char *p = text; p; part++) {
                char *bar = strchr(p, '|');
                if (bar)
                    *bar = 0;
                lv_point_t piece;
                lv_txt_get_size(&piece, p, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                size.x += piece.x + SOURCE_LABEL_GAP + (part ? UI_AUDIO_ARROW_W + UI_AUDIO_BTN_PAD : 0);
                if (bar)
                    *bar = '|';
                p = bar ? bar + 1 : NULL;
            }
        } else {
            lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        }

        const char *verdict = missing[0] ? "GLYPH" : (size.x > max ? "WIDE" : "OK");
        if (strcmp(verdict, "OK") != 0)
            bad++;
        printf("%s %s %s %d/%d px %s%s%s\n", verdict, kind, lang, (int)size.x, max, text,
               missing[0] ? " missing:" : "", missing);
    }
    return bad ? 1 : 0;
}
