#ifndef __MODULE_MODULE_H_
#define __MODULE_MODULE_H_

#include <stdbool.h>

// Module bay control (Goggle 1 and Goggle 2; nothing on BoxPro, it has no bay).
//
// The bay pins are SoC PG0-PG2, which are also the SDIO bus of the WiFi chip on
// both goggles, and they reach an analog module only through a wiring mod of the
// bay board (docs/module-bay.md).  The goggle drives them only while Module Bay >
// Module is not None, WiFi is off and the bay is powered, and never before
// MODULE_BOOT_MS after the bay was powered on (or the app started).  With Module =
// None (the default) they are never touched, as on a stock goggle.  A worker
// thread owns them, so none of the calls below waits for a transfer (except
// module_wifi_changed() before WiFi starts).

// [module] type in setting.ini: pkendall64's numbering; any other value loads as None.
typedef enum {
    MODULE_NONE,         // 0: the bay pins are never touched
    MODULE_SWITCH,       // 1: Fat Shark style 3-bit code
    MODULE_RAPIDFIRE,    // 2: ImmersionRC rapidFIRE SPI (also a True-D with MOD 1.17)
    MODULE_STEADYVIEW,   // 3
    MODULE_STEADYVIEW_X, // 4
    MODULE_TYPES
} module_type_t;

// The bay pins stay inputs at least this long after every bay power-on reported by
// the bay power owner (module_power_changed()), and after the app start without a
// bay power switch (batch 1), for every type: a module's bootloader may drive its
// pins meanwhile (the True-D's drives D13 until ~1.45 s after its power-on).
#define MODULE_BOOT_MS 2000

typedef enum {
    MODULE_STATUS_NO_BAY,    // BoxPro
    MODULE_STATUS_NONE,      // Module None
    MODULE_STATUS_WIFI,      // WiFi enabled: the pins are left to the SDIO bus
    MODULE_STATUS_SDIO,      // pins found in use by the SDIO host / WiFi driver
    MODULE_STATUS_PINS_UNKNOWN, // their pin function (PG_CFG0) could not be read: left alone too
    MODULE_STATUS_UNPOWERED, // bay power off
    MODULE_STATUS_STARTING,  // waiting for the module to boot (MODULE_BOOT_MS)
    MODULE_STATUS_ACTIVE,
} module_status_t;

// Start-up: checks the settings, starts the worker.
void module_init();
// Module Bay page: stores Module and Mode in g_setting.module (not setting.ini) and
// applies them.  Both change together, so the worker never sets a new Module up with
// the Mode of the one before.
void module_settings_set(int type, int mode);
// Module or Mode changed (already in g_setting.module; host tests).
void module_settings_changed(bool mode_changed);
// Goggle 2, Source > Analog Module changed (already in g_setting.source): when the
// bay became the analog source its module gets its mode and channel again.
void module_source_changed();
// Every bay power command sent to the DM5680 (batch 2 hardware, src/driver/dm5680.c);
// a repeat of the state the bay is in changes nothing.
void module_power_changed(int on);
// WiFi page: call after g_setting.wifi.enable changed, before WiFi is started
// (returns once the bay pins are released and no pin check is reading them, at
// most 3 s) and after it was stopped.  False: WiFi is on, but the pins were not
// released within the 3 s: WiFi must not be started.
bool module_wifi_changed();
module_status_t module_status();

// Bay pin check (Module Bay page, "Check Pins"; reads, never drives).  The worker
// sets PG0-PG2 to GPIO inputs and reads them for 100 ms, only while it does not
// control the bay and never while WiFi is on or the pins may be in the SDIO function
// (or their function cannot be read), and the PG bank supply as the SoC reports it
// (PIO_POW_VAL).  Shows the wiring mod and a module that pulls or drives the lines
// before a Module is selected.  Afterwards the pins are GPIO inputs, unexported.
typedef struct {
    unsigned seq;     // number of checks done (0: none yet)
    bool running;     // a check is pending or running
    int pg_bank_mv;   // 1800 or 3300 (PIO_POW_VAL bit 6: < 2.0 V / > 2.5 V), 0: unknown
    int levels;       // bit i: PGi read high; -1: not read (why)
    int toggled;      // bit i: PGi changed while it was read
    const char *why;  // why the levels were not read ("Not read: ...", a language key), NULL: they were
} module_pin_check_t;
void module_check_pins();
module_pin_check_t module_pin_check_result();

// The configured module type (whatever the pins, power and source).
int module_num_channels();               // 0: None
const char *module_mode_names(int type); // Mode list options, NULL: none
// The bay module's stored channel in the type's list ("CH5", "R5"), "" for None.
const char *module_channel_name();

// The analog picture comes from the module bay: Goggle 1 always, Goggle 2 with
// Source > Analog Module = Expansion.
bool module_bay_is_source();
// A Module is selected and the goggle may drive the bay pins (WiFi off).
bool module_bay_controlled();

// The channel of the analog receiver now selected (SOURCE_AV_MODULE): the built-in
// receiver (Goggle 2, BoxPro) or the bay module.  Each has its own stored channel,
// always one of the 48 standard channels (1..48 = A1..L8), never folded:
//   built-in receiver   [source] analog_channel (as upstream)
//   bay module          Goggle 1: [source] analog_channel (its only analog receiver)
//                       Goggle 2: [module] channel
// A bay type with fewer channels (Switch: 8, channels 1-8 of the module's own band)
// shows and sends the stored channel's place in its band, (ch - 1) % 8 + 1, and a
// choice in that list keeps the stored band; nothing is changed by switching types.
int analog_channel_get();                  // stored, 1..48
// Stores 1..48 (setting.ini too); the bay module follows.  Does nothing and returns
// false while the goggle does not tune the analog source (analog_channel_tunable()).
bool analog_channel_set(int channel);
int analog_channel_count();                // length of the list: 8 (Switch in the bay) or 48
int analog_channel_pos();                  // the stored channel's place in the list, 1..count
int analog_channel_at(int pos);            // the stored channel that selects list entry pos
const char *analog_channel_name(int pos);  // name of list entry pos (1-based)
const char *analog_channel_current_name(); // analog_channel_name(analog_channel_pos())
// The goggle can tune the analog source (and shows its channel): the built-in
// receiver always, the bay module only while it is controlled.
bool analog_channel_tunable();

#endif
