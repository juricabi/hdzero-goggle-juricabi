/*
 * Measures the strings of the Module Bay page (src/ui/page_modulebay.c) with the firmware's own
 * LVGL fonts (check_strings.py builds it once with conf/ui.h). Input, one per line:
 * "<kind>\t<lang>\t<text>", kind one of mb_label, mb_text, mb_status, mb_option, mb_note.
 * Output, one per line:
 *   "OK|WIDE|GLYPH <kind> <lang> <width>/<room> px <text>"
 * WIDE: wider than its place on the page, GLYPH: a character the font does not have (shown as
 * a box or nothing: the CJK fonts only hold the characters of the translations they were made
 * from). Exit status: 0 all OK, 1 something WIDE / GLYPH / of an unknown kind.
 */

#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "../../conf/ui.h"

// lv_conf.h's tick source (the firmware's util/time.c); not used for measuring
uint32_t time_ms(void) {
    return 0;
}

// the room page_modulebay.c leaves for the text, per kind (fixed sizes there; goggles only)
static int room(const char *kind, const lv_font_t **font) {
    *font = UI_PAGE_TEXT_FONT;
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
        lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

        const char *verdict = missing[0] ? "GLYPH" : (size.x > max ? "WIDE" : "OK");
        if (strcmp(verdict, "OK") != 0)
            bad++;
        printf("%s %s %s %d/%d px %s%s%s\n", verdict, kind, lang, (int)size.x, max, text,
               missing[0] ? " missing:" : "", missing);
    }
    return bad ? 1 : 0;
}
