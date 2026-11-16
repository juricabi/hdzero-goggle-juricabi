#include <string.h>

#include <log/log.h>
#include <minIni.h>

#include "../conf/ui.h"

#include "core/common.hh"
#include "core/settings.h"
#include "lang/language.h"
#include "module/module.h"
#include "page_common.h"
#include "page_modulebay.h"
#include "ui/ui_main_menu.h"
#include "ui/ui_style.h"

// Rows: 0 Module (None = the bay pins are never touched), 1 Mode (selectable only
// when the module has modes), 2 Check Pins, 3 Back.  A change applies (and is
// stored) when its list is closed with a click.  Check Pins only reads (module.h).
enum {
    ROW_TYPE = 0,
    ROW_MODE,
    ROW_CHECK,
    ROW_BACK,
    ROW_COUNT,
    ROW_STATUS = ROW_COUNT + 1,
    ROW_NOTE,
};

static lv_obj_t *cont;
static lv_obj_t *btn_module_type;
static lv_obj_t *lbl_module_mode;
static lv_obj_t *btn_module_mode;
static lv_obj_t *lbl_mode_note; // why Mode is greyed, in place of its list
static lv_obj_t *lbl_status;
static lv_obj_t *lbl_check;
static unsigned check_seq_shown = 0;

#define COLOR_ORANGE "FFA000"
#define COLOR_GREEN  "00FF00"

#define MB_DROPDOWN_W 420 // Module and Mode lists (utilities/module_bay_test/check_strings.c: mb_option)

static lv_obj_t *page_item_focused = NULL;
static uint16_t focused_orig_sel = 0; // selection when the focused list was opened

static lv_coord_t col_dsc[] = {160, 200, 240, 240, 160, 40, LV_GRID_TEMPLATE_LAST};
static lv_coord_t row_dsc[] = {60, 60, 60, 60, 60, 60, 60, 60, 60, 60, LV_GRID_TEMPLATE_LAST};

// The Module list, in module_type_t order ([module] type).  Every entry goes
// through the language files (the modules' own names map to themselves there).
static const char *const type_names[MODULE_TYPES] = {
    "None",
    "Switch (Fat Shark)",
    "rapidFIRE",
    "SteadyView",
    "SteadyView X",
};

// '\n'-separated options, each through _lang() (the Switch wirings "Wiring A
// (standard)" ... "Wiring F", the SteadyView modes).
static void translate_options(char *out, size_t size, const char *options) {
    char item[64];
    size_t len = 0;
    out[0] = 0;
    for (const char *p = options; p && *p && len + 1 < size;) {
        const char *end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        snprintf(item, sizeof(item), "%.*s", (int)n, p);
        const int w = snprintf(out + len, size - len, "%s%s", len ? "\n" : "", _lang(item));
        len = w < 0 ? size : len + (size_t)w;
        p = end ? end + 1 : p + n;
    }
}

// Mode list of a type, NULL when it has none here.
static const char *mode_names(int type) {
    // SteadyView changes mode through a bay power cycle: not possible without the
    // bay power switch (batch 1 hardware)
    if (type == MODULE_STEADYVIEW && getHwRevision() < HW_REV_2)
        return NULL;
    return module_mode_names(type);
}

static void show_module_settings(int mode) {
    const int type = lv_dropdown_get_selected(btn_module_type);
    const char *modes = mode_names(type);
    // a module that has modes, but not on this goggle: say why the row is greyed
    if (!modes && module_mode_names(type))
        lv_label_set_text(lbl_mode_note, _lang("Mode needs the bay power switch"));
    else
        lv_label_set_text(lbl_mode_note, "");
    if (modes) {
        char buf[256];
        translate_options(buf, sizeof(buf), modes);
        lv_dropdown_set_options(btn_module_mode, buf);
        if (mode < 0 || mode >= (int)lv_dropdown_get_option_cnt(btn_module_mode))
            mode = 0;
        lv_dropdown_set_selected(btn_module_mode, mode);
        lv_obj_clear_flag(btn_module_mode, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_state(lbl_module_mode, STATE_DISABLED);
        lv_obj_add_flag(pp_modulebay.p_arr.panel[ROW_MODE], FLAG_SELECTABLE);
    } else {
        lv_dropdown_set_options(btn_module_mode, "-");
        lv_dropdown_set_selected(btn_module_mode, 0);
        lv_obj_add_flag(btn_module_mode, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(lbl_module_mode, STATE_DISABLED);
        lv_obj_clear_flag(pp_modulebay.p_arr.panel[ROW_MODE], FLAG_SELECTABLE);
    }
}

static void update_status(void) {
    char buf[128];
    switch (module_status()) {
    case MODULE_STATUS_NO_BAY:
        snprintf(buf, sizeof(buf), "%s", _lang("No module bay"));
        break;
    case MODULE_STATUS_NONE:
        snprintf(buf, sizeof(buf), "%s", _lang("Off, bay pins untouched"));
        break;
    case MODULE_STATUS_WIFI:
        snprintf(buf, sizeof(buf), "#" COLOR_ORANGE " %s#", _lang("Paused while WiFi is on"));
        break;
    case MODULE_STATUS_SDIO:
        snprintf(buf, sizeof(buf), "#" COLOR_ORANGE " %s#", _lang("Paused, pins in use by WiFi"));
        break;
    case MODULE_STATUS_PINS_UNKNOWN: // PG_CFG0 not readable (sunxi_dump missing, busy or answers disagree)
        snprintf(buf, sizeof(buf), "#" COLOR_ORANGE " %s#", _lang("Paused, pin state unknown"));
        break;
    case MODULE_STATUS_UNPOWERED:
        snprintf(buf, sizeof(buf), "%s", _lang("Waiting for bay power"));
        break;
    case MODULE_STATUS_STARTING:
        snprintf(buf, sizeof(buf), "%s", _lang("Waiting for the module to boot"));
        break;
    case MODULE_STATUS_ACTIVE:
        // what the goggle sends: the bay module's own channel (it cannot know whether a module
        // is there to take it). Goggle 2: also while the built-in receiver is the analog source.
        snprintf(buf, sizeof(buf), "#" COLOR_GREEN " %s# %s%s%s", _lang("Sending"), module_channel_name(),
                 module_bay_is_source() ? "" : " ", module_bay_is_source() ? "" : _lang("(not video source)"));
        break;
    default:
        buf[0] = 0;
        break;
    }
    char text[160];
    snprintf(text, sizeof(text), "%s: %s", _lang("Status"), buf);
    lv_label_set_text(lbl_status, text);
}

// one pin of the result: 1, 0, or "toggling" (it changed while it was read)
static const char *pin_text(const module_pin_check_t *r, int pin) {
    if (r->toggled & (1 << pin))
        return _lang("toggling");
    return r->levels & (1 << pin) ? "1" : "0";
}

// Result of the last pin check, in plain words: "Pins 1 1 0, 3.3 V" (PG0 PG1 PG2, the
// PG bank supply). No verdict: the goggle cannot know what is connected. 1.8 V in orange:
// resistors alone cannot drive a 5 V module then (docs/module-bay-mod-safety.md).
static void update_check(void) {
    char buf[192], bank[48];
    const module_pin_check_t r = module_pin_check_result();
    if (r.running) {
        lv_label_set_text(lbl_check, _lang("Reading..."));
        return;
    }
    if (!r.seq)
        return; // (the hint set at page creation stays)
    check_seq_shown = r.seq;
    if (r.levels < 0) {
        snprintf(buf, sizeof(buf), "%s", _lang(r.why ? r.why : "Not read: no GPIO access"));
        lv_label_set_text(lbl_check, buf);
        return;
    }
    if (r.pg_bank_mv == 1800)
        snprintf(bank, sizeof(bank), "#" COLOR_ORANGE " 1.8 V#");
    else if (r.pg_bank_mv)
        snprintf(bank, sizeof(bank), "%d.%d V", r.pg_bank_mv / 1000, r.pg_bank_mv % 1000 / 100);
    else
        snprintf(bank, sizeof(bank), "? V");
    snprintf(buf, sizeof(buf), "%s %s %s %s, %s", _lang("Pins"), pin_text(&r, 0), pin_text(&r, 1), pin_text(&r, 2), bank);
    lv_label_set_text(lbl_check, buf);
}

static lv_obj_t *page_create(lv_obj_t *parent, panel_arr_t *arr) {
    char buf[512];
    // as the stock pages: the title and the rows start where theirs do
    lv_obj_t *page = lv_menu_page_create(parent, NULL);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(page, UI_PAGE_VIEW_SIZE);
    lv_obj_add_style(page, &style_subpage, LV_PART_MAIN);

    lv_obj_t *section = lv_menu_section_create(page);
    lv_obj_add_style(section, &style_submenu, LV_PART_MAIN);
    lv_obj_set_size(section, UI_PAGE_VIEW_SIZE);

    snprintf(buf, sizeof(buf), "%s:", _lang("Module Bay"));
    create_text(NULL, section, false, buf, LV_MENU_ITEM_BUILDER_VARIANT_2);

    cont = lv_obj_create(section);
    lv_obj_set_size(cont, 1040, 600);
    lv_obj_set_pos(cont, 0, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_GRID);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_context, LV_PART_MAIN);

    lv_obj_set_style_grid_column_dsc_array(cont, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(cont, row_dsc, 0);

    create_select_item(arr, cont);

    snprintf(buf, sizeof(buf), "%s", _lang(type_names[0]));
    for (int i = 1; i < MODULE_TYPES; i++)
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\n%s", _lang(type_names[i]));
    create_label_item(cont, _lang("Module"), 1, ROW_TYPE, 1);
    // (both lists wide enough for every entry in every language, "Wiring A (standard)" the widest:
    // utilities/module_bay_test/check_strings.py)
    btn_module_type = create_dropdown_item(cont, buf, 2, ROW_TYPE,
                                           MB_DROPDOWN_W, 40, 2, 4, LV_GRID_ALIGN_START, &lv_font_montserrat_26);

    lbl_module_mode = create_label_item(cont, _lang("Mode"), 1, ROW_MODE, 1);
    btn_module_mode = create_dropdown_item(cont, "-", 2, ROW_MODE,
                                           MB_DROPDOWN_W, 40, 2, 4, LV_GRID_ALIGN_START, &lv_font_montserrat_26);
    lbl_mode_note = create_label_item(cont, "", 2, ROW_MODE, 4);
    lv_obj_set_width(lbl_mode_note, 610);
    lv_obj_set_style_text_color(lbl_mode_note, lv_color_hex(TEXT_COLOR_DISABLE), 0);

    // ("Check Pins": the name the mod guide and the True-D documents use)
    create_label_item(cont, _lang("Check Pins"), 1, ROW_CHECK, 1);
    lbl_check = create_label_item(cont, _lang("Click to read the 3 bay pins (read only)"), 2, ROW_CHECK, 4);
    // up to the page's right edge, so that a longer translation scrolls instead of being cut off
    lv_obj_set_width(lbl_check, 610);

    snprintf(buf, sizeof(buf), "< %s", _lang("Back"));
    create_label_item(cont, buf, 1, ROW_BACK, 1);

    lbl_status = create_label_item(cont, "", 1, ROW_STATUS, 4);

    // what a pilot needs before choosing a module
    lv_obj_t *note = lv_label_create(cont);
    snprintf(buf, sizeof(buf), "%s\n%s\n%s",
             _lang("Module control needs the 3-wire bay mod (resistors or level shifter)."),
             _lang("It pauses while WiFi is on: WiFi uses the same pins."),
             _lang("Furious True-D with MOD 1.17: choose rapidFIRE."));
    lv_label_set_text(note, buf);
    lv_obj_set_style_text_font(note, UI_PAGE_LABEL_FONT, 0);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(TEXT_COLOR_DEFAULT), 0);
    lv_obj_set_style_pad_top(note, UI_PAGE_TEXT_PAD, 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 880);
    lv_obj_set_grid_cell(note, LV_GRID_ALIGN_START, 1, 4, LV_GRID_ALIGN_START, ROW_NOTE, 3);

    int type = g_setting.module.type;
    if (type < 0 || type >= MODULE_TYPES)
        type = MODULE_NONE;
    lv_dropdown_set_selected(btn_module_type, type);
    show_module_settings(g_setting.module.setting);
    update_status();

    arr->max = ROW_COUNT;
    return page;
}

// Stores and applies what the page shows.
static void apply_settings(void) {
    const int type = lv_dropdown_get_selected(btn_module_type);
    const int mode = lv_obj_has_flag(btn_module_mode, LV_OBJ_FLAG_HIDDEN) ? 0 : (int)lv_dropdown_get_selected(btn_module_mode);
    const bool type_changed = type != g_setting.module.type;
    const bool mode_changed = mode != g_setting.module.setting;

    if (!type_changed && !mode_changed)
        return;

    LOGI("module bay: module %d, mode %d", type, mode);
    // a new module is set up from scratch (its mode included); same one: new mode only
    module_settings_set(type, mode);
    ini_putl("module", "type", g_setting.module.type, SETTING_INI);
    ini_putl("module", "setting", g_setting.module.setting, SETTING_INI);
    update_status();
}

static void open_list(lv_obj_t *dropdown) {
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    focused_orig_sel = lv_dropdown_get_selected(dropdown);
    // every entry shown, none cut off (the theme limits a list to about 5 entries; the Switch
    // wirings are 6, and there is room for them below the row)
    lv_obj_set_style_max_height(list, 480, 0);
    lv_dropdown_open(dropdown);
    lv_obj_add_style(list, &style_dropdown, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_make(0, 0, 0), LV_PART_SELECTED | LV_STATE_CHECKED);
    page_item_focused = dropdown;
    pp_modulebay.p_arr.max = 0; // we are now in control of the roller
}

// keep: take the highlighted entry; otherwise back to the one before the list opened
static void close_list(bool keep) {
    if (!page_item_focused)
        return;
    if (keep) {
        lv_event_send(page_item_focused, LV_EVENT_RELEASED, NULL);
    } else {
        lv_dropdown_set_selected(page_item_focused, focused_orig_sel);
        lv_dropdown_close(page_item_focused);
    }
    lv_obj_remove_style(page_item_focused, &style_dropdown, LV_PART_MAIN);
    page_item_focused = NULL;
    pp_modulebay.p_arr.max = ROW_COUNT;
}

static void page_on_roller(uint8_t key) {
    if (page_item_focused) {
        lv_obj_remove_style(page_item_focused, &style_dropdown, LV_PART_MAIN);

        if (key == DIAL_KEY_UP) {
            uint32_t evt = LV_KEY_DOWN;
            lv_event_send(page_item_focused, LV_EVENT_KEY, &evt);
        } else if (key == DIAL_KEY_DOWN) {
            uint32_t evt = LV_KEY_UP;
            lv_event_send(page_item_focused, LV_EVENT_KEY, &evt);
        }
    }
}

static void page_on_click(uint8_t key, int sel) {
    (void)key;
    switch (sel) {
    case ROW_TYPE:
        if (!page_item_focused) {
            open_list(btn_module_type);
        } else {
            close_list(true);
            // another module starts with its first mode, the stored one keeps its mode
            show_module_settings(g_setting.module.type == (int)lv_dropdown_get_selected(btn_module_type) ? g_setting.module.setting : 0);
            apply_settings();
        }
        break;

    case ROW_MODE:
        if (lv_obj_has_flag(btn_module_mode, LV_OBJ_FLAG_HIDDEN))
            break; // (not selectable then)
        if (!page_item_focused) {
            open_list(btn_module_mode);
        } else {
            close_list(true);
            apply_settings();
        }
        break;

    case ROW_CHECK:
        if (!page_item_focused) {
            module_check_pins();
            update_check();
        }
        break;

    default:
        break;
    }
}

static void page_enter() {
    update_status();
}

static void page_exit() {
    // left with a list open (long press): drop what was only highlighted there
    close_list(false);
    apply_settings();
}

// Called from the main menu loop for every page, also while Module Bay is only
// highlighted in the menu (its preview shows the same status line).  The status
// follows bay power, WiFi and the module's boot.
static void page_on_update(uint32_t delta_ms) {
    static uint32_t last_ms = 0;
    (void)delta_ms;
    if (!lbl_status || !main_menu_is_shown() || lv_tick_elaps(last_ms) < 500)
        return;
    last_ms = lv_tick_get();
    update_status();
    const module_pin_check_t r = module_pin_check_result();
    if (lbl_check && (r.running || r.seq != check_seq_shown))
        update_check();
}

page_pack_t pp_modulebay = {
    .p_arr = {
        .cur = 0,
        .max = ROW_COUNT,
    },

    .name = "Module Bay",
    .create = page_create,
    .enter = page_enter,
    .exit = page_exit,
    .on_created = NULL,
    .on_update = page_on_update,
    .on_roller = page_on_roller,
    .on_click = page_on_click,
    .on_right_button = NULL,
};
