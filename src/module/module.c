#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>
#include <minIni.h>

#include "core/common.hh"
#include "core/osd.h"
#include "core/settings.h"
#include "ui/page_scannow.h"

#include "module.h"
#include "rapidfire.h"
#include "softspi.h"
#include "steadyview.h"
#include "steadyview_x.h"
#include "switch.h"

#if defined(HDZGOGGLE) || defined(HDZGOGGLE2)
#define MODULE_BAY 1
#else
#define MODULE_BAY 0 // BoxPro: no bay, nothing is ever driven
#endif

static module_def_t *modules[MODULE_TYPES] = {
    NULL, // None: the pins are never touched
    &switch_module,
    &rapidfire_module,
    &steadyview_module,
    &steadyview_x_module,
};
// settings_load() checks [module] type against 0..4
_Static_assert(MODULE_TYPES == 5, "the range check of [module] type in settings.c");

// While the bay pins are found in use by the SDIO host, check again this often.
#ifndef SDIO_RECHECK_MS
#define SDIO_RECHECK_MS 5000
#endif
// module_wifi_changed() waits at most this long for the pins to be released.
#define WIFI_RELEASE_MS 3000

// PIO PG_CFG0: function select of PG0..PG7, 4 bits each.  Function 2 of PG0-PG5
// is SDC1, the SDIO bus of the WiFi chip.
#define PIO_PG_CFG0     0x0300b0d8
#define PIO_FUNC_SDC1   2
// PIO_POW_VAL (V536 datasheet 1.6, p. 888): bit 6 = PG port power value, read only;
// 0: the bank supply is above 2.5 V, 1: below 2.0 V.
#define PIO_POW_VAL     0x0300b348
#define PIO_POW_VAL_PG  (1u << 6)
// pin check: how long and how often the pins are read
#define CHECK_MS        100
#define CHECK_STEP_MS   2

// What bay_pins_busy() looks at (variables so that the host test can point them
// at files of its own).  The emulator has no SoC registers and no SDIO host: no
// register dump there, and the pins count as free.
static const char *wlan_driver_path = "/sys/module/xradio_wlan";
#ifdef EMULATOR_BUILD
static const char *reg_dump_path = NULL;
#else
static const char *reg_dump_path = "/sys/class/sunxi_dump/dump";
#endif

/*
 * Threads: the dial (input thread, lvgl_mutex held), ELRS, the menu and whoever
 * sends a bay power command (dm5680.c) post requests under `lock` and return; the
 * worker thread alone touches the pins, with `lock` released during a transfer.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;  // to the worker: something changed
static pthread_cond_t idle;  // from the worker: nothing left to do
static bool started = false;

// requests
static bool bay_powered = false;
static uint64_t power_on_ms = 0;     // when the bay was switched on (batch 1: the app start), rounded up
static bool reinit = false;          // power switched: release the pins, set the module up again
static bool mode_pending = false;
static bool channel_pending = false; // the bay module's stored channel changed
static bool check_pending = false;   // pin check asked for
static bool check_busy = false;      // pin check running
static module_pin_check_t check_result = {.levels = -1};

// worker state
static module_def_t *active = NULL; // initialised module, holds the pins
static bool setup_channel = false;  // after init / set_mode: the channel goes out next, boot hold or not
static uint64_t resend_at_ms = 0;   // boot resend (module_def_t.boot_resend_ms) due
static bool sdio_busy = false;
static bool pins_unknown = false; // sdio_busy because PG_CFG0 could not be read (not because WiFi has the pins)

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The time of an event in whole ms, rounded up: a wait until it + MODULE_BOOT_MS,
// read back with now_ms() (rounded down), is never shorter than MODULE_BOOT_MS.
static uint64_t event_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
    return ms ? ms : 1; // (0 means "never")
}

static module_def_t *module_lookup(void) {
    int type = g_setting.module.type;
    if (!MODULE_BAY || type < 0 || type >= MODULE_TYPES)
        return NULL;
    return modules[type];
}

static bool pins_allowed(void) {
    return MODULE_BAY && !g_setting.wifi.enable;
}

// The module the bay should be running now (lock held).
static module_def_t *wanted(void) {
    if (!bay_powered || !pins_allowed())
        return NULL;
    return module_lookup();
}

// The pins are to be released (lock held).
static bool release_requested(void) {
    return active && (active != wanted() || reinit);
}

// ---- stored channels ----

static int valid_channel(int ch) {
    return ch >= 1 && ch <= ANALOG_CHANNEL_NUM ? ch : 33; // R1
}

// The bay module's stored channel (1..48).  Goggle 1 has no other analog receiver:
// upstream's [source] analog_channel.  Goggle 2 keeps that for its built-in
// receiver and stores the bay module's in [module] channel.
static int bay_channel(void) {
#if defined(HDZGOGGLE2)
    return valid_channel(g_setting.module.channel);
#else
    return valid_channel(g_setting.source.analog_channel);
#endif
}

static void bay_channel_store(int ch) {
#if defined(HDZGOGGLE2)
    g_setting.module.channel = ch;
    ini_putl("module", "channel", ch, SETTING_INI);
#else
    g_setting.source.analog_channel = ch;
    ini_putl("source", "analog_channel", ch, SETTING_INI);
#endif
}

// place of a 48-list channel in a list of count channels: (ch - 1) % count + 1
static int fold(int count, int ch) {
    if (count <= 0 || count >= ANALOG_CHANNEL_NUM)
        return ch;
    return (ch - 1) % count + 1;
}

// ---- bay pins and WiFi ----

// /sys/class/sunxi_dump/dump is one address latch for every process: the probe kit and the
// app's own awr / aww calls take this lock around their accesses (src/record/appmsg.h
// HDMIA_LOCK_FILE, src/util/system.c), and so does read_reg().
#define SUNXI_DUMP_LOCK "/tmp/.sunxi_dump.lock"
static const char *sunxi_dump_lock_path = SUNXI_DUMP_LOCK; // (a variable: the host test uses a file of its own)

static bool read_reg_once(uint32_t addr, uint32_t *val);

// Reads an SoC register through the sunxi_dump driver (as the awr script does): under the
// shared lock (not taken within 1 s: no read), three reads that must agree (a process that does
// not lock, such as a stock script, can still move the latch between the address and the value).
// False: the value is not known.
static bool read_reg(uint32_t addr, uint32_t *val) {
    if (!reg_dump_path)
        return false;
    int lk = open(sunxi_dump_lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lk < 0)
        return false;
    bool locked = false;
    for (int i = 0; i < 20 && !(locked = flock(lk, LOCK_EX | LOCK_NB) == 0); i++)
        usleep(50000); // at most 1 s, as the bridge
    uint32_t a = 0, b = 1, c = 2;
    const bool ok = locked && read_reg_once(addr, &a) && read_reg_once(addr, &b) && read_reg_once(addr, &c) && a == b && b == c;
    close(lk); // releases the lock
    if (ok)
        *val = a;
    return ok;
}

static bool read_reg_once(uint32_t addr, uint32_t *val) {
    char buf[64];
    bool ok = false;
    const char *dump = reg_dump_path;
    if (!dump)
        return false;
    int fd = open(dump, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;
    int n = snprintf(buf, sizeof(buf), "0x%08x", addr);
    if (write(fd, buf, n) == n) {
        lseek(fd, 0, SEEK_SET);
        n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            // the value is the last hex number of the answer
            char *last = NULL;
            for (char *p = strstr(buf, "0x"); p; p = strstr(p + 2, "0x"))
                last = p;
            if (last) {
                // a number, followed by nothing but the end of the line; and not the address
                // itself (an answer that only echoes the latch is not a value)
                char *end;
                unsigned long v = strtoul(last, &end, 16);
                ok = end > last + 2 && v != addr;
                while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
                    end++;
                ok = ok && *end == 0;
                *val = v;
            }
        }
    }
    close(fd);
    return ok;
}

// PG0-PG2 may belong to the WiFi SDIO bus right now: the WiFi driver is loaded, the SDIO
// host has them in its pin function, or their function cannot be read (then they are left
// alone too, and looked at again later).  Exporting them then could cut WiFi off.  *why:
// what was found (log), NULL when free.
static bool bay_pins_busy_why(const char **why) {
    *why = NULL;
    if (access(wlan_driver_path, F_OK) == 0) {
        *why = "WiFi driver loaded";
        return true;
    }
    if (!reg_dump_path)
        return false; // emulator: no SDIO host
    uint32_t cfg;
    if (!read_reg(PIO_PG_CFG0, &cfg)) {
        *why = "PG_CFG0 not readable (sunxi_dump missing, busy or answers disagree)";
        return true;
    }
    for (int pin = 0; pin < 3; pin++) {
        if (((cfg >> (4 * pin)) & 0x7) == PIO_FUNC_SDC1) {
            *why = "PG0-PG2 in the SDC1 (WiFi SDIO) function";
            return true;
        }
    }
    return false;
}

static int fit_channel(module_def_t *m, int channel) {
    return fold(m->num_channels, valid_channel(channel));
}

static void timed_wait(pthread_cond_t *cond, uint64_t until_ms) {
    struct timespec ts = {.tv_sec = until_ms / 1000, .tv_nsec = (until_ms % 1000) * 1000000};
    pthread_cond_timedwait(cond, &lock, &ts);
}

// ---- worker ----

// The pin check (worker thread, lock released): never while the worker controls
// the bay, WiFi is on or the SDIO host may have the pins.  Reading means exporting
// them and setting them to GPIO inputs first (after rc.sh they are "IO disabled",
// function 7, whose data register the datasheet leaves undefined).  Afterwards they
// stay GPIO inputs (function 0), unexported, as module control's close() leaves
// them: the SDIO host sets its own function again when WiFi starts, as it does after
// rc.sh's function 7 at every boot (docs/module-bay.md).
static void run_pin_check(bool controlling, bool wifi_on) {
    module_pin_check_t r = {.levels = -1};
    uint32_t pow = 0;
    if (read_reg(PIO_POW_VAL, &pow))
        r.pg_bank_mv = (pow & PIO_POW_VAL_PG) ? 1800 : 3300;
    // (the reasons are shown on the Module Bay page, through the language files)
    const char *busy_why = NULL;
    if (controlling) {
        r.why = "Not read: set Module to None first";
    } else if (wifi_on) {
        r.why = "Not read: switch WiFi off first";
    } else if (bay_pins_busy_why(&busy_why)) {
        r.why = strncmp(busy_why, "PG_CFG0", 7) == 0 ? "Not read: pin state unknown" : "Not read: pins in use by WiFi";
    } else if (!softspi_open()) {
        softspi_close();
        r.why = "Not read: no GPIO access";
    } else {
        softspi_float_all(); // inputs before the first read
        usleep(CHECK_STEP_MS * 1000);
        int first = -1, toggled = 0;
        for (int t = 0; t < CHECK_MS; t += CHECK_STEP_MS) {
            int now = 0;
            for (int pin = 0; pin < SOFTSPI_PINS; pin++)
                now |= softspi_get_pin((softspi_pin_t)pin) << pin;
            if (first < 0)
                first = now;
            toggled |= now ^ first;
            usleep(CHECK_STEP_MS * 1000);
        }
        softspi_close(); // inputs, unexported
        r.levels = first;
        r.toggled = toggled;
    }
    LOGI("module: pin check: PG bank %d mV, levels %d, toggled %d%s%s%s%s", r.pg_bank_mv, r.levels, r.toggled,
         r.why ? ", " : "", r.why ? r.why : "", busy_why ? ": " : "", busy_why ? busy_why : "");
    pthread_mutex_lock(&lock);
    r.seq = check_result.seq + 1;
    check_result = r;
    pthread_mutex_unlock(&lock);
}

// The current bay channel to the active module (lock held, released meanwhile).
static void send_channel(const char *why) {
    module_def_t *m = active;
    const int ch = fit_channel(m, bay_channel());
    pthread_mutex_unlock(&lock);
    LOGI("module: channel %d%s", ch, why);
    m->set_channel(ch - 1);
    pthread_mutex_lock(&lock);
}

static void *module_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        const uint64_t now = now_ms();
        module_def_t *want = wanted();
        uint64_t wake_at = 0;

        // pins released: bay off, Module changed, WiFi on, power cycled
        if (release_requested()) {
            module_def_t *m = active;
            pthread_mutex_unlock(&lock);
            LOGI("module: bay pins released");
            m->close();
            pthread_mutex_lock(&lock);
            active = NULL;
            setup_channel = false;
            resend_at_ms = 0;
            continue;
        }

        if (check_pending) {
            const bool controlling = active != NULL, wifi_on = g_setting.wifi.enable;
            check_pending = false;
            check_busy = true;
            pthread_mutex_unlock(&lock);
            run_pin_check(controlling, wifi_on);
            pthread_mutex_lock(&lock);
            check_busy = false;
            continue;
        }

        if (!active && want) {
            // every type: the pins stay inputs until MODULE_BOOT_MS after the last
            // power-on (power_on_ms is set whenever the bay is powered)
            const uint64_t on = power_on_ms;
            if (on && now < on + MODULE_BOOT_MS) {
                wake_at = on + MODULE_BOOT_MS;
            } else if (on) {
                pthread_mutex_unlock(&lock);
                const char *why;
                const bool busy = bay_pins_busy_why(&why);
                pthread_mutex_lock(&lock);
                if (busy != sdio_busy)
                    LOGW("module: bay pins %s%s", busy ? "left alone, checked again in 5 s: " : "free again", busy ? why : "");
                sdio_busy = busy;
                pins_unknown = busy && strncmp(why, "PG_CFG0", 7) == 0;
                if (want != wanted() || power_on_ms != on)
                    continue; // changed meanwhile (a new power-on starts the wait again)
                if (busy) {
                    wake_at = now + SDIO_RECHECK_MS;
                } else {
                    // init takes the pins and sets the mode; the channel follows at once
                    // (setup_channel).  While the module still boots, later channel
                    // changes wait for the boot resend.
                    reinit = false;
                    mode_pending = false;
                    channel_pending = false;
                    setup_channel = true;
                    resend_at_ms = 0;
                    if (want->boot_resend_ms > 0 && power_on_ms + want->boot_resend_ms > now)
                        resend_at_ms = power_on_ms + want->boot_resend_ms;
                    active = want;
                    pthread_mutex_unlock(&lock);
                    LOGI("module: type %d init", g_setting.module.type);
                    want->init();
                    pthread_mutex_lock(&lock);
                    continue;
                }
            }
        }

        if (active) {
            const bool boot_hold = resend_at_ms && now < resend_at_ms;
            if (setup_channel) {
                // after init / a mode change: the module is put on the current channel
                // whatever its type did (nothing a type does there is relied on)
                setup_channel = false;
                channel_pending = false;
                send_channel(" (set-up)");
                continue;
            }
            if (mode_pending && !boot_hold) {
                module_def_t *m = active;
                mode_pending = false;
                setup_channel = true;
                pthread_mutex_unlock(&lock);
                LOGI("module: mode %d", g_setting.module.setting);
                m->set_mode();
                pthread_mutex_lock(&lock);
                continue;
            }
            if (resend_at_ms && !boot_hold) {
                resend_at_ms = 0;
                channel_pending = false;
                send_channel(" (boot resend)");
                continue;
            }
            if (channel_pending && !boot_hold) {
                channel_pending = false;
                send_channel("");
                continue;
            }
            if (boot_hold && (!wake_at || resend_at_ms < wake_at))
                wake_at = resend_at_ms;
        }

        pthread_cond_broadcast(&idle);
        if (wake_at)
            timed_wait(&wake, wake_at);
        else
            pthread_cond_wait(&wake, &lock);
    }
    return NULL;
}

// ---- for the module types (worker thread) ----

bool module_abort_requested(void) {
    pthread_mutex_lock(&lock);
    const bool abort = release_requested();
    pthread_mutex_unlock(&lock);
    return abort;
}

bool module_sleep_ms(unsigned ms) {
    pthread_mutex_lock(&lock);
    const uint64_t until = event_ms() + ms; // (never shorter than ms)
    bool abort;
    while (!(abort = release_requested()) && now_ms() < until)
        timed_wait(&wake, until);
    pthread_mutex_unlock(&lock);
    return !abort;
}

int module_current_channel(void) {
    pthread_mutex_lock(&lock);
    module_def_t *m = active ? active : module_lookup();
    const int ch = m ? fit_channel(m, bay_channel()) - 1 : 0;
    pthread_mutex_unlock(&lock);
    return ch;
}

unsigned module_ms_since_power_on(void) {
    pthread_mutex_lock(&lock);
    const uint64_t on = power_on_ms;
    pthread_mutex_unlock(&lock);
    if (!on)
        return UINT_MAX;
    const uint64_t now = now_ms();
    const uint64_t since = now > on ? now - on : 0; // (on is rounded up)
    return since < UINT_MAX ? (unsigned)since : UINT_MAX;
}

// ---- requests ----

static int count_modes(const module_def_t *m) {
    int modes = 0;
    if (m && m->mode_names) {
        modes = 1;
        for (const char *p = m->mode_names; *p; p++)
            modes += *p == '\n';
    }
    return modes;
}

static void poke(void) {
    pthread_cond_signal(&wake);
}

void module_init() {
    if (!MODULE_BAY || started)
        return;

    if (g_setting.module.type < 0 || g_setting.module.type >= MODULE_TYPES) {
        LOGE("module: bad type %d in setting.ini, set to None", g_setting.module.type);
        g_setting.module.type = MODULE_NONE;
    }
    const int modes = count_modes(module_lookup());
    if (g_setting.module.setting < 0 || g_setting.module.setting >= (modes ? modes : 1))
        g_setting.module.setting = 0;

    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&wake, &attr);
    pthread_cond_init(&idle, &attr);
    pthread_condattr_destroy(&attr);

    // Without the bay power switch (batch 1) the bay is powered with the goggle and
    // the module booted long before the app; the app start counts as its power-on
    // all the same (MODULE_BOOT_MS more).  Batch 2 waits for the first bay power command.
    bay_powered = getHwRevision() < HW_REV_2;
    power_on_ms = bay_powered ? event_ms() : 0;

    pthread_t tid;
    if (pthread_create(&tid, NULL, module_worker, NULL) == 0) {
        pthread_detach(tid);
        started = true;
    } else {
        LOGE("module: worker thread not started");
    }
    LOGI("module: type %d, mode %d, WiFi %s, bay %s, channel %d", g_setting.module.type, g_setting.module.setting,
         g_setting.wifi.enable ? "on" : "off", bay_powered ? "always powered" : "power switched", bay_channel());
}

void module_settings_changed(bool mode_changed) {
    if (!started)
        return;
    pthread_mutex_lock(&lock);
    if (mode_changed)
        mode_pending = true;
    poke();
    pthread_mutex_unlock(&lock);
}

void module_settings_set(int type, int mode) {
    if (type < 0 || type >= MODULE_TYPES)
        type = MODULE_NONE;
    if (mode < 0)
        mode = 0;
    pthread_mutex_lock(&lock);
    // both under the lock: the worker never sets a new type up with the old one's mode
    const bool type_changed = type != g_setting.module.type, mode_changed = mode != g_setting.module.setting;
    g_setting.module.type = type;
    g_setting.module.setting = mode;
    if (started) {
        // a new type is set up from scratch (its mode included); same type: new mode only
        if (mode_changed && !type_changed)
            mode_pending = true;
        poke();
    }
    pthread_mutex_unlock(&lock);
}

void module_source_changed() {
    if (!started || !module_bay_is_source())
        return;
    // The bay module keeps its own channel while the built-in receiver is the
    // source, but nothing watched it meanwhile (its own buttons, a command it
    // missed): it gets its mode and channel again.  Not powered or not set up yet:
    // the set-up after power-on / init sends both anyway.
    pthread_mutex_lock(&lock);
    if (active)
        mode_pending = true; // set_mode(), then the channel
    poke();
    pthread_mutex_unlock(&lock);
}

// Called with every bay power command sent to the DM5680 (batch 2 hardware;
// src/driver/dm5680.c: Analog_Module_Power() on a source change, Power > AnalogRX
// Power, Scan Now and wake-up, and the switch-off at going to sleep): the only way
// the bay is powered on, apart from SteadyView's own mode-set pulse.  A command
// that repeats the state the bay is in changes nothing.  Off: the bay pins are
// released (inputs), so they do not feed the unpowered module.  On: the module
// missed everything sent while it was off and may be in its bootloader; the pins
// stay inputs and it is set up again MODULE_BOOT_MS later (and gets its channel
// again boot_resend_ms after power-on if its type asks for that).
void module_power_changed(int on) {
    if (!started)
        return;
    pthread_mutex_lock(&lock);
    if (bay_powered == (on != 0)) {
        pthread_mutex_unlock(&lock);
        return; // (the bay was in that state already)
    }
    bay_powered = on;
    if (on)
        power_on_ms = event_ms();
    reinit = true;
    poke();
    pthread_mutex_unlock(&lock);
}

bool module_wifi_changed() {
    if (!started)
        return true;
    bool released = true;
    pthread_mutex_lock(&lock);
    poke();
    if (g_setting.wifi.enable) {
        // the SDIO host takes PG0-PG2 when WiFi starts: let go of them first, and let
        // a pin check that is reading them (exported as GPIO inputs) finish, since its
        // close() would set them to GPIO input under the SDIO host's feet
        const uint64_t deadline = now_ms() + WIFI_RELEASE_MS;
        while ((active || check_busy) && now_ms() < deadline)
            timed_wait(&idle, deadline);
        if (active || check_busy) {
            LOGE("module: bay pins still held after %d ms: WiFi must not start", WIFI_RELEASE_MS);
            released = false;
        }
    }
    pthread_mutex_unlock(&lock);
    return released;
}

void module_check_pins() {
    if (!started)
        return;
    pthread_mutex_lock(&lock);
    check_pending = true;
    poke();
    pthread_mutex_unlock(&lock);
}

module_pin_check_t module_pin_check_result() {
    module_pin_check_t r = {.levels = -1, .why = "Not read: no GPIO access"}; // (no module bay)
    if (!started)
        return r;
    pthread_mutex_lock(&lock);
    r = check_result;
    r.running = check_pending || check_busy;
    pthread_mutex_unlock(&lock);
    return r;
}

module_status_t module_status() {
    module_status_t status;
    if (!MODULE_BAY)
        return MODULE_STATUS_NO_BAY;
    if (!module_lookup())
        return MODULE_STATUS_NONE;
    if (g_setting.wifi.enable)
        return MODULE_STATUS_WIFI;
    pthread_mutex_lock(&lock);
    if (!bay_powered)
        status = MODULE_STATUS_UNPOWERED;
    else if (active)
        status = MODULE_STATUS_ACTIVE;
    else if (sdio_busy)
        status = pins_unknown ? MODULE_STATUS_PINS_UNKNOWN : MODULE_STATUS_SDIO;
    else
        status = MODULE_STATUS_STARTING;
    pthread_mutex_unlock(&lock);
    return status;
}

int module_num_channels() {
    module_def_t *m = module_lookup();
    return m ? m->num_channels : 0;
}

const char *module_channel_name() {
    module_def_t *m = module_lookup();
    if (!m)
        return "";
    return m->name(fit_channel(m, bay_channel()) - 1);
}

const char *module_mode_names(int type) {
    if (!MODULE_BAY || type < 0 || type >= MODULE_TYPES || !modules[type])
        return NULL;
    return modules[type]->mode_names;
}

bool module_bay_is_source() {
#if defined(HDZGOGGLE2)
    // Expansion; Dual (auto_protocol_detect) always uses the built-in receiver
    return g_setting.source.analog_module == SETTING_SOURCES_ANALOG_MODULE_EXTERNAL &&
           !g_setting.source.auto_protocol_detect;
#elif defined(HDZGOGGLE)
    return true;
#else
    return false;
#endif
}

bool module_bay_controlled() {
    return module_lookup() && pins_allowed();
}

// ---- the analog channel of the receiver now selected ----

int analog_channel_get() {
    if (module_bay_is_source())
        return bay_channel();
    return valid_channel(g_setting.source.analog_channel);
}

bool analog_channel_set(int channel) {
    if (channel < 1 || channel > ANALOG_CHANNEL_NUM || !analog_channel_tunable())
        return false;
    if (!module_bay_is_source()) {
        if (g_setting.source.analog_channel != channel) {
            g_setting.source.analog_channel = channel;
            ini_putl("source", "analog_channel", channel, SETTING_INI);
        }
        return true;
    }
    if (bay_channel() == channel)
        return true;
    bay_channel_store(channel);
    if (started) {
        pthread_mutex_lock(&lock);
        channel_pending = true; // (unpowered: the power-on set-up sends the current one)
        poke();
        pthread_mutex_unlock(&lock);
    }
    return true;
}

int analog_channel_count() {
    if (module_bay_is_source() && module_num_channels() > 0)
        return module_num_channels();
    return ANALOG_CHANNEL_NUM;
}

int analog_channel_pos() {
    return fold(analog_channel_count(), analog_channel_get());
}

int analog_channel_at(int pos) {
    const int n = analog_channel_count();
    const int ch = analog_channel_get();
    if (pos < 1 || pos > n)
        return ch;
    if (n >= ANALOG_CHANNEL_NUM)
        return pos;
    return (ch - 1) / n * n + pos; // same band, place pos
}

const char *analog_channel_name(int pos) {
    const int n = analog_channel_count();
    if (pos < 1 || pos > n)
        pos = 1;
    module_def_t *m = module_lookup();
    if (module_bay_is_source() && m)
        return m->name(pos - 1);
    return channel2str(0, 0, pos);
}

const char *analog_channel_current_name() {
    return analog_channel_name(analog_channel_pos());
}

bool analog_channel_tunable() {
    return !module_bay_is_source() || module_bay_controlled();
}
