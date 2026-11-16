#ifndef _MODULE_DEF_H
#define _MODULE_DEF_H

#include <stdbool.h>

// One module bay type.  All functions run on the module worker thread (module.c),
// only while the goggle may drive the bay pins and the bay is powered; channel
// numbers are 0-based here.  init() is the first call after a bay power-on, never
// earlier than MODULE_BOOT_MS (module.h) after it; before that and after close()
// the pins are inputs.
//
// The worker sends the channel itself (set_channel) right after init() and after
// every set_mode(), so neither has to: after init the module is always on the
// current channel, whatever the type does in init.
typedef struct {
    int num_channels;           // 48 (A1..L8) or 8 (channels 1-8 of the module's own band)
    char *(*name)(int channel);
    void (*init)();             // take the pins, set the module up with its mode (not the channel)
    void (*close)();            // release the pins (inputs)
    void (*set_channel)(int channel);
    void (*set_mode)();         // apply g_setting.module.setting (the module may lose its channel)
    const char *mode_names;     // options of the Module Bay "Mode" list, NULL: none
    int boot_resend_ms;         // > 0: the channel is sent again this long after bay power-on
} module_def_t;

char *module_standard_channel_name(int channel);

// For the module types, on the worker thread only.
//
// True once the pins are to be released (bay power off or switched again, Module
// changed, WiFi on): a transfer stops at its next abort point
// (between packets, repeats and set-up steps) and returns; the worker then calls
// close().
bool module_abort_requested(void);
// Sleeps ms milliseconds.  Returns false, early, as soon as module_abort_requested().
bool module_sleep_ms(unsigned ms);
// Milliseconds since the goggle switched the bay power on (without a bay power
// switch, batch 1: since the goggle app started; the module was powered before).
unsigned module_ms_since_power_on(void);
// The bay module's stored channel in the active type's list (0-based), for a type
// whose set_mode() writes it as part of the mode sequence (SteadyView).
int module_current_channel(void);

#endif
