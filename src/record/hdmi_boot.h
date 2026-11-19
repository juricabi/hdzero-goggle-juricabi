#pragma once
/* ===========================================================================
 * HDMI audio (EXPERIMENTAL, off by default): what is in effect for THIS BOOT.
 * docs/hdmi-audio.md, section "Boot-time decision" and the edge-case table.
 *
 * HDMI Out Audio needs PH1-PH3 in pin function 3 (I2S0 BCLK / LRCK / DOUT). rc.sh writes
 * PH_CFG0 = 0x35517751 at every boot (setup_uart(), the same script and value on every
 * target: PH1 = uart4 RX, PH2/PH3 off). The pins are switched at most ONCE PER BOOT, by
 * /mnt/app/app/hdmi_audio_boot (hdmi_boot_run()), which rc.sh runs right after it wrote
 * the stock value and before the record daemon or the app start, and only if setting.ini
 * [hdmi] audio_out is exactly 1. Nothing switches them back during that boot (the one
 * exception: hdmi_boot_app_start() puts them back if the boot step was interrupted half
 * way); the next boot's rc.sh writes the stock value again, so Off + restart is a full
 * undo. An app restart, a record daemon restart (SD check, card insert, format) or the
 * Audio page never repeat or undo it.
 *
 * Strict rule for PH_CFG0 (also in utilities/probe/hdmi_audio_test.sh): before a write the
 * whole register must be one of exactly two values, HB_PH_STOCK (as rc.sh leaves it) or
 * HB_PH_AUDIO (only the PH1-PH3 fields changed); the value written is one of these two
 * constants, never built from what was read; after a write three agreeing reads must give
 * the intended value, else HB_PH_STOCK is written back and verified, and HDMI audio stays
 * off for this boot with the fault "pin check failed".
 *
 * The result is HDMIA_BOOT_FILE (/tmp: RAM, empty after every boot), written whole into a
 * temporary file and renamed into place, one "key=value" per line:
 *   version=1  boot_id=<the kernel's boot id>  phase=check|done  supported=0|1
 *   audio_out=0|1  audio_in=0|1  (the settings at boot, 1 = exactly "1")
 *   pins=untouched|stock|audio|other|unknown  fault=<"" or one of the HDMI_AUDIO_FAULT_*>
 *   ph_cfg0=<last verified value or "">  detail=<one line for the log>
 * It is valid only for the boot whose boot id it carries and only if every line checks out;
 * anything else counts as "no decision" (HDMI audio off). The app (src/core/hdmi_audio.c)
 * and the record daemon (pcmbridge.c) read it.
 * ======================================================================== */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// PH_CFG0 (PIO base 0x0300B000 + port H 7 * 0x24); fields of 4 bits, PH0 in bits 3:0
#define HB_PH_CFG0        0x0300B0FCu
#define HB_PH_STOCK       0x35517751u // rc.sh setup_uart(): PH0 out, PH1 uart4, PH2/3 off, PH4 out, PH5/6 uart3, PH7 uart0
#define HB_PH_OWNED_MASK  0x0000FFF0u // PH1..PH3: the only fields HDMI audio changes
#define HB_PH_OWNED_AUDIO 0x00003330u // function 3: I2S0 BCLK / LRCK / DOUT (V536 datasheet, PH_CFG0)
#define HB_PH_AUDIO       ((HB_PH_STOCK & ~HB_PH_OWNED_MASK) | HB_PH_OWNED_AUDIO) // 0x35513331

// the only reasons the app shows (they go through the language files)
#define HDMI_AUDIO_FAULT_PINS "pin check failed"
#define HDMI_AUDIO_FAULT_CHIP "HDMI chip check failed"
#define HDMI_AUDIO_FAULT_PATH "audio path failed"

typedef enum { HB_PHASE_CHECK = 1, HB_PHASE_DONE = 2 } hb_phase_t;
typedef enum { HB_PINS_UNTOUCHED = 0, HB_PINS_STOCK, HB_PINS_AUDIO, HB_PINS_OTHER, HB_PINS_UNKNOWN } hb_pins_t;
typedef enum { HB_REC_OK = 0, HB_REC_MISSING, HB_REC_STALE, HB_REC_CORRUPT } hb_rec_t;
typedef enum { HB_FAULT_NONE = 0, HB_FAULT_PINS, HB_FAULT_CHIP, HB_FAULT_PATH } hb_fault_t;

typedef struct {
    char boot_id[40];
    hb_phase_t phase;
    bool supported;
    bool out; // [hdmi] audio_out was exactly 1 at boot
    bool in;  // [hdmi] audio_in was exactly 1 at boot
    hb_pins_t pins;
    hb_fault_t fault;
    uint32_t ph_cfg0; // last verified PH_CFG0 (ph_valid)
    bool ph_valid;
    char detail[128];
} hdmi_boot_t;

const char *hdmi_boot_fault_text(hb_fault_t f); // NULL for HB_FAULT_NONE, else HDMI_AUDIO_FAULT_*

bool hdmi_boot_supported(void); // HDMI audio on this target (Goggle 1, Goggle 2, BoxPro; no other)
// [hdmi] <key> in setting.ini is exactly "1" (as minIni reads it: spaces and a trailing comment
// removed); any other value, a missing key or an unreadable file is false
bool hdmi_boot_setting_is_1(const char *setting_ini, const char *key);
hb_pins_t hdmi_boot_ph_state(uint32_t ph_cfg0); // HB_PINS_STOCK / _AUDIO / _OTHER

hb_rec_t hdmi_boot_read(hdmi_boot_t *b); // HB_REC_OK only for a complete, consistent record of this boot
int hdmi_boot_write(const hdmi_boot_t *b);

// rc.sh step (hdmi_audio_boot): decide once for this boot, switch PH1-PH3 if asked and safe
int hdmi_boot_run(const char *setting_ini);
// app start: what is in effect for this boot, and why HDMI out audio is off if it was asked
// for; enabled_out = [hdmi] audio_out is 1 now (with no valid record that is a fault)
void hdmi_boot_app_start(bool enabled_out, bool *out_active, bool *in_active, hb_fault_t *fault);
// a later fault (HB_FAULT_CHIP, HB_FAULT_PATH): HDMI out audio off for the rest of this boot,
// also across app restarts; the pins are not touched
void hdmi_boot_set_fault(hb_fault_t fault);
// the record daemon, before it touches I2S0: HDMI out audio is in effect for this boot
bool hdmi_boot_out_ready(void);

#ifdef __cplusplus
}
#endif
