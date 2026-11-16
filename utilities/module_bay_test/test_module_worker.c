// Host-side test of the module bay worker (src/module/module.c, built as for the
// Goggle 2 emulator) with recording stand-ins for the module types, in real time
// (about 35 s).  The stand-ins' init() never sends a channel, so the checks below
// catch a worker that relies on a type's init to put the module on its channel.
//
// Checks: a stored type of the removed True-D (5) loads as None; Module None (the
// default) never touches the pins, whatever happens; every type (1-4) waits
// MODULE_BOOT_MS (2.0 s) after every bay power-on before its init (cold, a quick
// off / on, a power-on during a transfer, Module chosen or WiFi switched off within
// the 2 s); the rapidFIRE boot resend (5.0 s) with changes in between held back
// for it; the pin release on power-off; the WiFi interlock; that no request blocks
// its caller; the stored channels (Switch list, type round trips, built-in
// receiver vs bay module on the Goggle 2, nothing stored while the bay is not
// controlled); Module and Mode set together from the page (module_settings_set());
// the source change; the pin check, also WiFi switched on while it reads; and the
// SDIO half of the WiFi interlock: bay_pins_busy() against stand-in files for
// /sys/module/xradio_wlan and the sunxi_dump register answer (PG_CFG0), and the
// worker leaving the pins alone while they are in the SDC1 function.
//
// With MODULE_TEST_BATCH1=1 in the environment it runs as a batch 1 Goggle (no bay
// power switch, the bay powered with the goggle): the app start counts as the
// power-on (init >= 2.0 s after module_init()), and None touches nothing there either.
//
// Timing: lower bounds are exact (the worker must not act early: it rounds the
// power-on time up), upper bounds have SLACK_MS (300, -DSLACK_MS=... for a slow
// host) for a loaded host.
//
//   make -C utilities/module_bay_test test

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// (the Makefile names it: make SRC=<a copy of src/module> runs the tests against the copy)
#ifndef MODULE_C
#define MODULE_C "../../src/module/module.c"
#endif
// module.c reads the sunxi_dump answer with read(): through test_read(), which can make one
// read of a series answer another value (a latch moved by a process that takes no lock)
#include <unistd.h>
static ssize_t test_read(int fd, void *buf, size_t n);
#define read(fd, buf, n) test_read(fd, buf, n)
#include MODULE_C
#undef read

static int dump_reads, dump_other_at; // the dump_other_at-th read (from 1) after dump_reads = 0 answers another value
static ssize_t test_read(int fd, void *buf, size_t n) {
    const ssize_t r = read(fd, buf, n);
    if (r > 1 && dump_other_at && ++dump_reads == dump_other_at) {
        char *b = buf;
        ssize_t i = r - 1;
        while (i > 0 && (b[i] == '\n' || b[i] == ' '))
            i--;
        b[i] = b[i] == '2' ? '7' : '2'; // the last digit of the value: another pin function for PG0
    }
    return r;
}

#ifndef SLACK_MS
#define SLACK_MS 300
#endif

// ---- stand-ins for the rest of the firmware ----

setting_t g_setting;

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    (void)file, (void)func, (void)line, (void)level, (void)fmt;
    return 0;
}

static int ini_writes;
int ini_putl(const char *Section, const char *Key, long Value, const char *Filename) {
    (void)Section, (void)Key, (void)Value, (void)Filename;
    ini_writes++;
    return 1;
}

char *channel2str(uint8_t is_hdzero, uint8_t is_lowband, uint8_t channel) {
    static char buf[8];
    (void)is_hdzero, (void)is_lowband;
    snprintf(buf, sizeof(buf), "#%d", channel);
    return buf;
}

static bool batch1;
hw_revision_t getHwRevision() {
    return batch1 ? HW_REV_1 : HW_REV_2; // batch 2: bay power switched by the power thread
}

// bay pins for module.c's pin check: inputs reading 1, 0, 1; every call counted; a read
// before the pins were set to inputs is counted (review defect 4); get_pin can be made to
// hang (a check that does not end)
static int pins_exported, pin_calls, pins_inputs, reads_not_inputs, get_pin_hang_ms;
bool softspi_open() {
    pin_calls++;
    pins_exported = 1;
    pins_inputs = 0; // exported: still in the function rc.sh left them in
    return true;
}
void softspi_float_all() {
    pin_calls++;
    pins_inputs = pins_exported;
}
void softspi_close() {
    pin_calls++;
    pins_exported = 0;
    pins_inputs = 0;
}
int softspi_get_pin(softspi_pin_t pin) {
    pin_calls++;
    reads_not_inputs += !pins_inputs;
    if (get_pin_hang_ms) {
        const int ms = get_pin_hang_ms;
        get_pin_hang_ms = 0;
        usleep(ms * 1000);
    }
    return pin != SOFTSPI_DAT;
}

// ---- recording module types ----

#define MAX_EVENTS 512
typedef struct {
    uint64_t t; // ms since the test's t0
    char op;    // 'i' init, 'c' close, 'h' channel, 'm' mode, 'a' cut short
    int arg;
} event_t;

static event_t events[MAX_EVENTS];
static int n_events;
static uint64_t t0;
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;

static void record(char op, int arg) {
    pthread_mutex_lock(&ev_lock);
    if (n_events < MAX_EVENTS)
        events[n_events++] = (event_t){now_ms() - t0, op, arg};
    pthread_mutex_unlock(&ev_lock);
}

static int init_setting = -1; // g_setting.module.setting seen by the last init
static void rec_init() {      // takes the pins, sends no channel
    init_setting = g_setting.module.setting;
    record('i', g_setting.module.type);
}
static void rec_close() { record('c', 0); }
// a channel change as rapidFIRE sends it: frames of ~15 ms (not interruptible)
// with abortable gaps between them
static void rec_channel(int ch) {
    record('h', ch);
    for (int i = 0; i < 6; i++) {
        if (i && !module_sleep_ms(i == 3 ? 100 : 10)) {
            record('a', i); // cut short: pins to be released
            return;
        }
        usleep(15000);
    }
}
static void rec_mode() { record('m', g_setting.module.setting); }
static char *rec_name(int ch) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "n%d", ch);
    return buf;
}

#define STAND_IN(n, modes, resend) \
    {.num_channels = n, .name = rec_name, .init = rec_init, .close = rec_close, .set_channel = rec_channel, \
     .set_mode = rec_mode, .mode_names = modes, .boot_resend_ms = resend}
module_def_t switch_module = STAND_IN(8, "a\nb\nc", 0);
module_def_t rapidfire_module = STAND_IN(48, "a\nb\nc", 5000);
module_def_t steadyview_module = STAND_IN(48, "a\nb\nc\nd", 0);
module_def_t steadyview_x_module = STAND_IN(48, "a\nb\nc\nd", 0);

// ---- test ----

static int failures, checks;

static void check(int ok, const char *what) {
    printf("  %-82s %s\n", what, ok ? "ok" : "FAIL");
    failures += !ok;
    checks++;
}

static uint64_t t_now(void) { return now_ms() - t0; }

static void sleep_until(uint64_t t) {
    while (t_now() < t)
        usleep(2000);
}

static int find(char op, int nth, event_t *out) {
    for (int i = 0; i < n_events; i++)
        if (events[i].op == op && nth-- == 0) {
            *out = events[i];
            return 1;
        }
    return 0;
}

// first event of type op at or after from
static int find_from(char op, uint64_t from, event_t *out) {
    for (int i = 0; i < n_events; i++)
        if (events[i].op == op && events[i].t >= from) {
            *out = events[i];
            return 1;
        }
    return 0;
}

// the event right after the first `op` at or after from
static int next_after(char op, uint64_t from, event_t *out) {
    for (int i = 0; i + 1 < n_events; i++)
        if (events[i].op == op && events[i].t >= from) {
            *out = events[i + 1];
            return 1;
        }
    return 0;
}

static int count(char op, uint64_t from, uint64_t to) {
    int n = 0;
    for (int i = 0; i < n_events; i++)
        n += events[i].op == op && events[i].t >= from && events[i].t < to;
    return n;
}

// a type's calls (init, channel, mode: what touches the pins) in [from, to)
static int driving(uint64_t from, uint64_t to) {
    return count('i', from, to) + count('h', from, to) + count('m', from, to);
}

static uint64_t timed_call_ms(void (*fn)(void)) {
    uint64_t a = now_ms();
    fn();
    return now_ms() - a;
}

// waits until the worker has nothing left to do (at most ~2 s)
static void settle(void) {
    usleep(20000);
    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&lock);
        const bool busy = channel_pending || mode_pending || setup_channel || release_requested() ||
                          (!active && wanted() && !sdio_busy);
        pthread_mutex_unlock(&lock);
        if (!busy)
            break;
        usleep(10000);
    }
    usleep(250000); // a transfer in progress
}

// writes text to path (the stand-in for a sysfs file), "": removes it
static void put_file(const char *path, const char *text) {
    if (!*text) {
        unlink(path);
        return;
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void power_on(void) { module_power_changed(1); }
static void power_off(void) { module_power_changed(0); }
static void wifi_changed(void) { module_wifi_changed(); }

// the time (test ms) of a power-on report: taken just before the call, so the
// worker's own (rounded up) time is >= it
static uint64_t report_on(void) {
    const uint64_t t = t_now();
    power_on();
    return t;
}

static const char *type_name[MODULE_TYPES] = {"None", "Switch", "rapidFIRE", "SteadyView", "SteadyView X"};

static int run_batch1(void) {
    char what[160];
    event_t e;
    printf("Batch 1 Goggle (no bay power switch), rapidFIRE:\n");
    g_setting.module.type = MODULE_RAPIDFIRE;
    g_setting.wifi.enable = false;
    t0 = now_ms();
    module_init();
    sleep_until(2000 + SLACK_MS);
    snprintf(what, sizeof(what), "app start counts as the bay power-on: init %llu ms after module_init() (>= 2000)",
             find('i', 0, &e) ? (unsigned long long)e.t : 0ULL);
    check(find('i', 0, &e) && e.t >= 2000 && e.t < 2000 + SLACK_MS && driving(0, 2000) == 0, what);
    check(next_after('i', 0, &e) && e.op == 'h', "  then the channel");
    g_setting.module.type = MODULE_NONE;
    module_settings_changed(false);
    settle();
    const int calls = pin_calls, n = n_events;
    analog_channel_set(9);
    module_source_changed();
    g_setting.wifi.enable = true;
    module_wifi_changed();
    g_setting.wifi.enable = false;
    module_wifi_changed();
    settle();
    check(pin_calls == calls && n_events == n, "Module None: nothing touches the pins (dial, source, WiFi on / off)");
    printf("%d checks, %s\n", checks, failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}

int main(void) {
    char what[160];
    event_t e;
    uint64_t t;

    batch1 = getenv("MODULE_TEST_BATCH1") != NULL;
    g_setting.wifi.enable = false;
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
    g_setting.source.analog_channel = 12; // built-in receiver: B4
    g_setting.module.channel = 33;        // bay module: R1
    if (batch1)
        return run_batch1();

    printf("Settings:\n");
    g_setting.module.type = 5; // the True-D type of earlier test builds
    g_setting.module.setting = 2;
    t0 = now_ms();
    module_init();
    check(g_setting.module.type == MODULE_NONE && g_setting.module.setting == 0,
          "a stored type 5 (the removed True-D) loads as None, its mode as 0");

    printf("Module None (the default), Goggle 2 with the bay power switch:\n");
    usleep(300000);
    check(n_events == 0 && pin_calls == 0, "nothing sent while the bay is unpowered");
    check(module_status() == MODULE_STATUS_NONE, "status: bay pins not used");
    t0 = now_ms();
    n_events = 0;
    check(timed_call_ms(power_on) < 50, "power-on report returns at once (power thread)");
    sleep_until(2600);
    check(!analog_channel_set(5) && g_setting.module.channel == 33 && !analog_channel_tunable(),
          "  dial / ELRS: the bay module is not tuned, its stored channel kept");
    module_source_changed();
    power_off();
    usleep(10000);
    power_on();
    g_setting.wifi.enable = true;
    wifi_changed();
    g_setting.wifi.enable = false;
    wifi_changed();
    sleep_until(5200);
    check(n_events == 0 && pin_calls == 0 && module_status() == MODULE_STATUS_NONE,
          "power-on, power cycle, source change, WiFi on / off: no type call, no pin touched");

    printf("Every type waits 2.0 s after every bay power-on (pins inputs until then):\n");
    for (int type = MODULE_SWITCH; type < MODULE_TYPES; type++) {
        g_setting.module.type = type;
        g_setting.module.setting = 0;
        power_off();
        module_settings_changed(false);
        settle();
        const uint64_t on1 = report_on();
        sleep_until(on1 + 1000);
        power_off();
        usleep(10000); // one tick of the power thread: source switched away and back
        const uint64_t on2 = report_on();
        sleep_until(on2 + 2000 + SLACK_MS);
        snprintf(what, sizeof(what), "%s: power-on, off / on 1 s later: init %lld ms after the last on, none before",
                 type_name[type], find_from('i', on1, &e) ? (long long)(e.t - on2) : -1LL);
        check(driving(on1, on2 + 2000) == 0 && find_from('i', on1, &e) && e.t >= on2 + 2000 && e.t < on2 + 2000 + SLACK_MS,
              what);
        check(next_after('i', on1, &e) && e.op == 'h', "  then the channel");
    }

    // a power-on while the module is being set up (standby and back): released, 2 s again
    g_setting.module.type = MODULE_SWITCH;
    module_settings_changed(false);
    settle();
    analog_channel_set(analog_channel_at(3));
    usleep(20000); // a transfer is going out
    t = t_now();
    power_off();
    power_on();
    sleep_until(t + 2000 + SLACK_MS);
    check(find_from('c', t, &e) && e.t < t + 50 && driving(t + 1, t + 2000) == 0 && find_from('i', t, &e) && e.t >= t + 2000,
          "power off / on during a transfer: released at once, init 2.0 s after the on");

    // Module chosen 0.5 s after power-on: init 2.0 s after the power-on
    g_setting.module.type = MODULE_NONE;
    module_settings_changed(false);
    power_off();
    settle();
    const uint64_t on3 = report_on();
    sleep_until(on3 + 500);
    g_setting.module.type = MODULE_STEADYVIEW;
    module_settings_changed(false);
    sleep_until(on3 + 2000 + SLACK_MS);
    check(driving(on3, on3 + 2000) == 0 && find_from('i', on3, &e) && e.t >= on3 + 2000 && e.t < on3 + 2000 + SLACK_MS,
          "Module chosen 0.5 s after power-on: init 2.0 s after the power-on");
    // WiFi switched off 1 s after power-on: init 2.0 s after the power-on
    g_setting.wifi.enable = true;
    wifi_changed();
    power_off();
    settle();
    const uint64_t on4 = report_on();
    sleep_until(on4 + 1000);
    g_setting.wifi.enable = false;
    wifi_changed();
    sleep_until(on4 + 2000 + SLACK_MS);
    check(driving(on4, on4 + 2000) == 0 && find_from('i', on4, &e) && e.t >= on4 + 2000 && e.t < on4 + 2000 + SLACK_MS,
          "WiFi switched off 1 s after power-on: init 2.0 s after the power-on");
    // the bay long powered: a new Module is set up at once
    t = t_now();
    g_setting.module.type = MODULE_RAPIDFIRE;
    module_settings_changed(false);
    usleep(150000);
    check(find_from('i', t, &e) && e.t < t + 100, "bay powered > 2 s: a new Module is set up at once");

    printf("rapidFIRE boot resend, power-off:\n");
    g_setting.module.channel = 33;
    power_off();
    settle();
    const uint64_t on = report_on();
    sleep_until(on + 3000);
    analog_channel_set(5); // dial during the module's boot
    check(find_from('i', on, &e) && e.t >= on + 2000 && e.t < on + 2000 + SLACK_MS, "init 2.0 s after power-on");
    check(next_after('i', on, &e) && e.op == 'h' && e.arg == 32,
          "right after init (which sends nothing): the stored channel R1 (set-up)");
    sleep_until(on + 4800);
    check(count('h', on, on + 4800) == 1, "channel change at 3.0 s held back until the 5.0 s resend");
    sleep_until(on + 5000 + SLACK_MS);
    check(find_from('h', on + 4800, &e) && e.t >= on + 5000 && e.t < on + 5000 + SLACK_MS && e.arg == 4,
          "held channel (5) sent with the 5.0 s resend");
    check(module_status() == MODULE_STATUS_ACTIVE, "status: active");
    check(g_setting.module.channel == 5 && g_setting.source.analog_channel == 12,
          "dial stored the bay module's channel, not the built-in receiver's");
    settle();
    t = t_now();
    analog_channel_set(7);
    sleep_until(t + SLACK_MS);
    check(find_from('h', t, &e) && e.t < t + SLACK_MS && e.arg == 6, "later channel change sent at once");
    settle();
    // ripples: every bay power command is reported (src/driver/dm5680.c), also one that
    // repeats the state (Analog_Module_Power(1) at wake-up, Power > AnalogRX Power, Scan Now)
    t = t_now();
    power_on();
    usleep(300000);
    check(count('c', t, 1000000) == 0 && count('i', t, 1000000) == 0 && count('h', t, 1000000) == 0 &&
              module_status() == MODULE_STATUS_ACTIVE,
          "a repeated power-on report (the same command again): nothing released, set up or sent");

    analog_channel_set(8); // power-off during a transfer
    usleep(50000);
    t = t_now();
    check(timed_call_ms(power_off) < 50, "power-off report returns at once");
    sleep_until(t + SLACK_MS);
    check(find_from('c', t, &e) && e.t < t + 15 + 50, "pins released after power-off, transfer cut short (< 65 ms)");
    check(find_from('a', t, &e), "  (it stopped between frames)");
    analog_channel_set(9);
    t = t_now();
    usleep(200000);
    check(count('h', t, 1000000) == 0, "nothing sent while unpowered");

    printf("WiFi:\n");
    const uint64_t on5 = report_on();
    sleep_until(on5 + 5000 + SLACK_MS); // past the boot resend
    settle();
    int closes = count('c', 0, 1000000);
    analog_channel_set(4); // a transfer is going out when WiFi is switched on
    usleep(50000);
    g_setting.wifi.enable = true;
    t = t_now();
    uint64_t waited = timed_call_ms(wifi_changed);
    check(count('c', 0, 1000000) == closes + 1, "WiFi on during a transfer: pins released before returning");
    check(find_from('a', t - 100, &e) && waited < 15 + 50, "  the transfer cut short after its frame (< 65 ms)");
    printf("    (waited %llu ms)\n", (unsigned long long)waited);
    check(module_status() == MODULE_STATUS_WIFI, "status: paused while WiFi is on");
    check(!analog_channel_set(3) && g_setting.module.channel == 4 && !analog_channel_tunable(),
          "WiFi on: bay module not tunable, its stored channel kept");
    t = t_now();
    usleep(300000);
    check(count('h', t, 1000000) == 0 && count('i', t, 1000000) == 0, "nothing sent while WiFi is on");
    g_setting.wifi.enable = false;
    uint64_t back = t_now();
    wifi_changed();
    usleep(300000);
    check(count('i', back, 1000000) == 1, "WiFi off: module set up again (bay long powered: at once)");
    check(next_after('i', back, &e) && e.op == 'h' && e.arg == 3, "  on its stored channel (4)");

    g_setting.module.type = MODULE_NONE;
    t = t_now();
    module_settings_changed(false);
    usleep(300000);
    check(count('c', t, 1000000) == 1 && module_status() == MODULE_STATUS_NONE, "Module None: pins released");
    int writes = ini_writes;
    check(!analog_channel_set(20) && g_setting.module.channel == 4 && ini_writes == writes,
          "Module None: ELRS/dial cannot change the stored bay channel");

    printf("Switch (8 channels) and type round trips:\n");
    g_setting.module.type = MODULE_SWITCH;
    g_setting.module.channel = 37; // R5
    t = t_now();
    module_settings_changed(false);
    settle();
    check(g_setting.module.channel == 37, "type Switch: stored channel R5 (37) kept");
    check(analog_channel_count() == 8 && analog_channel_pos() == 5, "  shown as list entry 5 (CH5)");
    check(count('i', t, 1000000) == 1 && next_after('i', t, &e) && e.op == 'h' && e.arg == 4,
          "  switch init, then code 4 (CH5)");
    t = t_now();
    analog_channel_set(analog_channel_at(2)); // dial: CH2
    settle();
    check(g_setting.module.channel == 34 && find_from('h', t, &e) && e.arg == 1, "dial CH2: stored R2 (band kept), code 1");
    analog_channel_set(28); // ELRS VTX F4
    settle();
    check(g_setting.module.channel == 28 && analog_channel_pos() == 4, "ELRS F4: stored F4 (28), shown CH4");
    g_setting.module.setting = 1;
    t = t_now();
    module_settings_changed(true);
    settle();
    check(find_from('m', t, &e) && e.arg == 1 && next_after('m', t, &e) && e.op == 'h' && e.arg == 3,
          "pin order change: mode, then the channel code again");
    g_setting.module.type = MODULE_RAPIDFIRE;
    g_setting.module.setting = 0;
    t = t_now();
    module_settings_changed(false);
    settle();
    check(g_setting.module.channel == 28 && analog_channel_count() == 48 && analog_channel_pos() == 28,
          "type rapidFIRE: back on F4 (48 channels)");
    check(next_after('i', t, &e) && e.op == 'h' && e.arg == 27, "  init, then F4");
    g_setting.module.type = MODULE_SWITCH;
    module_settings_changed(false);
    settle();
    g_setting.module.type = MODULE_STEADYVIEW_X;
    module_settings_changed(false);
    settle();
    check(g_setting.module.channel == 28, "Switch -> SteadyView X round trip: F4 kept");

    printf("Module Bay page: Module and Mode set together (module_settings_set()):\n");
    g_setting.module.setting = 0;
    t = t_now();
    module_settings_set(MODULE_STEADYVIEW, 3);
    settle();
    check(find_from('i', t, &e) && e.arg == MODULE_STEADYVIEW && init_setting == 3 && count('m', t, 1000000) == 0 &&
              next_after('i', t, &e) && e.op == 'h',
          "new Module with a Mode: one init that sees the new Mode, then the channel");
    t = t_now();
    module_settings_set(MODULE_STEADYVIEW, 1);
    settle();
    check(count('i', t, 1000000) == 0 && find_from('m', t, &e) && e.arg == 1 && next_after('m', t, &e) && e.op == 'h',
          "same Module, new Mode: mode, then the channel, no init");
    t = t_now();
    module_settings_set(MODULE_STEADYVIEW, 1);
    settle();
    check(count('i', t, 1000000) + count('m', t, 1000000) + count('h', t, 1000000) == 0, "nothing changed: nothing sent");
    t = t_now();
    module_settings_set(MODULE_TYPES, 0);
    settle();
    check(g_setting.module.type == MODULE_NONE && module_status() == MODULE_STATUS_NONE && count('c', t, 1000000) == 1,
          "a Module outside 0..4: None, pins released");
    module_settings_set(MODULE_STEADYVIEW_X, 0);
    settle();

    printf("Goggle 2 built-in receiver and Source > Analog Module:\n");
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_INTERNAL;
    check(analog_channel_count() == 48 && analog_channel_tunable() && analog_channel_get() == 12,
          "built-in receiver: 48 channels, its own channel (B4)");
    t = t_now();
    analog_channel_set(40);
    settle();
    check(g_setting.source.analog_channel == 40 && g_setting.module.channel == 28 && count('h', t, 1000000) == 0,
          "built-in receiver tuned: bay module and its channel untouched");
    g_setting.module.type = MODULE_SWITCH;
    module_settings_changed(false);
    settle();
    check(analog_channel_count() == 48 && analog_channel_get() == 40, "type Switch with Built-in selected: built-in list unchanged");
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
    t = t_now();
    module_source_changed();
    settle();
    check(find_from('m', t, &e) && next_after('m', t, &e) && e.op == 'h' && e.arg == 3,
          "toggled to Expansion: mode and channel sent again (CH4 of F4)");
    check(analog_channel_get() == 28 && analog_channel_pos() == 4 && g_setting.source.analog_channel == 40,
          "  bay F4 shown as CH4, built-in channel kept (L8)");
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_INTERNAL;
    module_source_changed();
    check(analog_channel_get() == 40, "toggled back to Built-in: L8 again");
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
    // ripples: Dual (Source > Auto Detect) always uses the built-in receiver, Expansion or not
    g_setting.source.auto_protocol_detect = true;
    check(!module_bay_is_source() && analog_channel_tunable() && analog_channel_count() == 48 && analog_channel_get() == 40,
          "Dual with Expansion selected: the built-in receiver is the source (L8, 48 channels)");
    t = t_now();
    analog_channel_set(41);
    settle();
    check(g_setting.source.analog_channel == 41 && g_setting.module.channel == 28 && count('h', t, 1000000) == 0,
          "  tuned in Dual: the built-in receiver's channel, bay module and its channel untouched");
    g_setting.source.auto_protocol_detect = false;
    t = t_now();
    module_source_changed();
    settle();
    check(module_bay_is_source() && analog_channel_get() == 28 && find_from('m', t, &e) && next_after('m', t, &e) && e.op == 'h',
          "  Dual off: the bay module is the source again (F4), mode and channel sent again");
    g_setting.source.analog_channel = 40;

    printf("Pin check (reads only):\n");
    module_pin_check_t pc = module_pin_check_result();
    const unsigned seq = pc.seq;
    module_check_pins();
    settle();
    pc = module_pin_check_result();
    check(pc.seq == seq + 1 && pc.levels == -1 && pc.why && !pins_exported,
          "while the worker controls the bay: levels not read, pins left to it");
    g_setting.module.type = MODULE_NONE;
    module_settings_changed(false);
    settle();
    module_check_pins();
    settle();
    pc = module_pin_check_result();
    check(pc.seq == seq + 2 && pc.levels == 5 && pc.toggled == 0 && !pc.why && !pins_exported && !pc.running,
          "Module None: PG0 1, PG1 0, PG2 1 read as inputs, pins unexported again");
    check(reads_not_inputs == 0, "  set to inputs before the first read (not left \"IO disabled\")");
    check(pc.pg_bank_mv == 0, "  PG bank supply unknown without the register dump");
    g_setting.wifi.enable = true;
    module_wifi_changed();
    module_check_pins();
    settle();
    pc = module_pin_check_result();
    check(pc.seq == seq + 3 && pc.levels == -1 && pc.why && !pins_exported, "WiFi on: levels not read");
    g_setting.wifi.enable = false;
    module_wifi_changed();
    // WiFi switched on while a check reads the pins (exported as GPIO inputs)
    module_check_pins();
    usleep(30000);
    const int reading = pins_exported;
    g_setting.wifi.enable = true;
    waited = timed_call_ms(wifi_changed);
    snprintf(what, sizeof(what), "WiFi on during a pin check: returns once the check let go of the pins (%llu ms)",
             (unsigned long long)waited);
    check(reading && !pins_exported && waited >= 40 && waited < 100 + SLACK_MS, what);
    pc = module_pin_check_result();
    check(pc.seq == seq + 4 && pc.levels == 5 && !pc.running, "  (that check read the pins before WiFi started)");
    g_setting.wifi.enable = false;
    module_wifi_changed();
    // review defect 5: the pins not released within 3 s: WiFi must not start
    get_pin_hang_ms = 3600;
    module_check_pins();
    usleep(30000);
    g_setting.wifi.enable = true;
    t = t_now();
    const bool released = module_wifi_changed();
    waited = t_now() - t;
    snprintf(what, sizeof(what), "pins still held after 3 s (a check that hangs): WiFi refused after %llu ms",
             (unsigned long long)waited);
    check(!released && waited >= 3000 && waited < 3000 + SLACK_MS, what);
    g_setting.wifi.enable = false;
    module_wifi_changed();
    usleep(800000); // the hanging check ends
    g_setting.wifi.enable = true;
    check(module_wifi_changed(), "  WiFi on again once nothing holds the pins: allowed");
    g_setting.wifi.enable = false;
    module_wifi_changed();

    printf("WiFi SDIO interlock (bay_pins_busy(), stand-in sysfs files):\n");
    char dir[] = "/tmp/module_bay_test.XXXXXX";
    if (!mkdtemp(dir)) {
        check(0, "temporary directory");
    } else {
        char drv[64], dump[64], lockf[64];
        snprintf(drv, sizeof(drv), "%s/xradio_wlan", dir);
        snprintf(dump, sizeof(dump), "%s/dump", dir);
        snprintf(lockf, sizeof(lockf), "%s/sunxi_dump.lock", dir);
        wlan_driver_path = drv;
        reg_dump_path = dump;
        sunxi_dump_lock_path = lockf; // never the host's /tmp/.sunxi_dump.lock
        static const struct {
            const char *driver, *dump;
            bool busy;
            const char *what;
        } cases[] = {
            // review defect 6: a register that cannot be read counts as busy
            {"", "", true, "no driver, register dump missing: busy (pin function unknown)"},
            {"", "garbage\n", true, "no driver, register dump echoes only the address: busy"},
            {"", "garbage garbage garbage\n", true, "no driver, register dump answers garbage: busy"},
            {"", "0x0300b0d8: 0x777 \n", false, "a short value, trailing blank (0x777: PG0-PG2 IO disabled): free"},
            {"x", "", true, "WiFi driver loaded: busy"},
            {"", "0x0300b0d8: 0x22777777\n", false, "PG0-PG5 IO disabled (rc.sh, 0x22777777): free"},
            {"", "0x0300b0d8: 0x22222222\n", true, "PG0-PG5 in SDC1 (function 2): busy"},
            {"", "0x0300b0d8: 0x22777277\n", true, "only PG2 in SDC1: busy"},
            {"", "0x0300b0d8 0x22777727\n", true, "only PG1 in SDC1 (answer without a colon): busy"},
            {"", "0x0300b0d8: 0x22772777\n", false, "only PG3 in SDC1 (not a bay pin): free"},
            {"", "0x0300b0d8: 0x22777771\n", false, "PG0 output (function 1, module control): free"},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const char *why;
            put_file(drv, cases[i].driver);
            put_file(dump, cases[i].dump);
            check(bay_pins_busy_why(&why) == cases[i].busy, cases[i].what);
        }
        {
            // the three reads must all agree: one of them moved by another latch user is unreadable
            const char *why = NULL;
            put_file(drv, "");
            put_file(dump, "0x0300b0d8: 0x22777777\n");
            for (int at = 1; at <= 3; at++) {
                dump_reads = 0;
                dump_other_at = at;
                const bool busy = bay_pins_busy_why(&why);
                dump_other_at = 0;
                char what[96];
                snprintf(what, sizeof(what), "read %d of 3 answers another value: busy (pin function unknown)", at);
                check(busy && why && strncmp(why, "PG_CFG0", 7) == 0, what);
            }
            // the shared sunxi_dump lock held by another process for more than 1 s: no read, busy
            int lk = open(lockf, O_RDWR | O_CREAT, 0644);
            const bool held = lk >= 0 && flock(lk, LOCK_EX) == 0;
            const uint64_t t0 = t_now();
            const bool busy = bay_pins_busy_why(&why);
            const uint64_t dt = t_now() - t0;
            if (lk >= 0)
                close(lk);
            check(held && busy && why && strncmp(why, "PG_CFG0", 7) == 0 && dt >= 900 && dt < 1000 + SLACK_MS,
                  "sunxi_dump lock held elsewhere: no read after about 1 s, busy (pin function unknown)");
            check(!bay_pins_busy_why(&why), "lock free again: read, free");
        }
        // Check Pins with the pin function unknown: not read, and says so
        put_file(drv, "");
        put_file(dump, "");
        g_setting.module.type = MODULE_NONE;
        module_settings_changed(false);
        settle();
        module_check_pins();
        settle();
        pc = module_pin_check_result();
        check(pc.levels == -1 && pc.why && strcmp(pc.why, "Not read: pin state unknown") == 0 && pc.pg_bank_mv == 0,
              "Check Pins, register dump not readable: \"Not read: pin state unknown\", bank voltage unknown");
        put_file(dump, "0x0300b0d8: 0x22777777\n");
        module_check_pins();
        settle();
        pc = module_pin_check_result();
        check(pc.levels == 5 && !pc.why, "Check Pins, PG0-PG2 IO disabled (rc.sh): read");

        // the worker leaves the pins alone while they are busy and checks again
        g_setting.module.type = MODULE_RAPIDFIRE;
        power_off();
        module_settings_changed(false);
        settle();
        put_file(dump, "0x0300b0d8: 0x22222222\n");
        t = report_on();
        sleep_until(t + 2000 + SLACK_MS);
        check(count('i', t, 1000000) == 0 && module_status() == MODULE_STATUS_SDIO,
              "pins in SDC1 at set-up: no init, status \"pins in use by WiFi\"");
        // their function not readable: left alone as well, but the status says why (not WiFi)
        put_file(dump, "");
        sleep_until(t_now() + SDIO_RECHECK_MS + SLACK_MS);
        check(count('i', t, 1000000) == 0 && module_status() == MODULE_STATUS_PINS_UNKNOWN,
              "pin function not readable: no init, status \"pin state unknown\" (not \"in use by WiFi\")");
        put_file(dump, "0x0300b0d8: 0x22777777\n");
        uint64_t freed = t_now();
        sleep_until(freed + SDIO_RECHECK_MS + SLACK_MS);
        check(find_from('i', freed, &e) && e.t < freed + SDIO_RECHECK_MS + SLACK_MS && module_status() == MODULE_STATUS_ACTIVE,
              "pins freed: set up at the next check");
        unlink(drv);
        unlink(dump);
        rmdir(dir);
    }

    printf("%d checks, %s\n", checks, failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
