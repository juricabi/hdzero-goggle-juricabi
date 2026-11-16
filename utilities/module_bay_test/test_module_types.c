// Host-side test of the real module types (src/module/switch.c, rapidfire.c,
// steadyview.c, steadyview_x.c) driven by the real worker (module.c, built as for
// the Goggle 2 emulator), with the bay pins replaced by a recording model: an input
// reads high (pulled up), an output its level.  Real time, about 70 s.
//
// Stock: with Module None (the default) no pin is ever touched, whatever happens.
// Every type, every power path: no pin is an output until 2.0 s after each bay
//   power-on (cold, a quick off / on, standby and back); SteadyView's own mode-set
//   pulse is the one power-up with driven pins (its protocol), and it comes later.
// Switch: the code sequence at power-up, and models of a True-D in its "1-8" bay
//   mode (MOD 1.16 and MOD 1.17) end on the stored channel, channel 8 included.
// rapidFIRE: the frames on the pins: every CS / CLK edge >= 100 us after the one
//   before, DATA changed only while CLK is low and >= 100 us before the next rise;
//   a model of MOD 1.17's receiver (its frame check and band / channel pairing)
//   takes every commanded channel; the SPI mode entry, the set-up commands, the
//   5 s resend; the trace's timing is printed for docs/module-bay.md.  The same
//   frame checks under a signal storm that cuts the worker's sleeps short.
//   TRACE_OUT=<file>: the rapidFIRE edges written there (for the True-D simulator).
// SteadyView X: soft_uart loaded >= 2.0 s after power-on, CS low; pins inputs at close.
// All: the pins are inputs again soon after power-off, also in mid-transfer.
//
//   make -C utilities/module_bay_test test

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/common.hh"
#include "core/settings.h"
#include "module/module.h"
#include "module/module_def.h"
#include "module/softspi.h"

#define SLACK_MS 300

// ---- stand-ins for the rest of the firmware ----

setting_t g_setting;

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    (void)file, (void)func, (void)line, (void)level, (void)fmt;
    return 0;
}

int ini_putl(const char *Section, const char *Key, long Value, const char *Filename) {
    (void)Section, (void)Key, (void)Value, (void)Filename;
    return 1;
}

char *channel2str(uint8_t is_hdzero, uint8_t is_lowband, uint8_t channel) {
    static char buf[8];
    (void)is_hdzero, (void)is_lowband;
    snprintf(buf, sizeof(buf), "#%d", channel);
    return buf;
}

hw_revision_t getHwRevision() {
    return HW_REV_2; // bay power switched by the power thread
}

// ---- bay pin model ----

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define MAX_EDGES 400000
typedef struct {
    uint64_t t;   // us since t0
    uint8_t line; // effective levels of the three lines after the change, bit i = pin i
    uint8_t out;  // which pins are outputs
} edge_t;

static pthread_mutex_t pin_lock = PTHREAD_MUTEX_INITIALIZER;
static edge_t edges[MAX_EDGES];
static int n_edges;
static uint64_t t0;
static int pin_out[SOFTSPI_PINS];   // output?
static int pin_level[SOFTSPI_PINS]; // output level
static int pin_calls;               // every call that could touch a pin (softspi, gpio, uart, insmod)

static uint8_t lines_now(void) {
    uint8_t v = 0;
    for (int i = 0; i < SOFTSPI_PINS; i++)
        v |= (pin_out[i] ? pin_level[i] : 1) << i;
    return v;
}

static uint8_t outs_now(void) {
    uint8_t v = 0;
    for (int i = 0; i < SOFTSPI_PINS; i++)
        v |= pin_out[i] << i;
    return v;
}

static void pins_changed(void) {
    const uint8_t l = lines_now(), o = outs_now();
    if (n_edges && edges[n_edges - 1].line == l && edges[n_edges - 1].out == o)
        return;
    if (n_edges < MAX_EDGES)
        edges[n_edges++] = (edge_t){now_us() - t0, l, o};
}

static void set(int pin, int out, int level) {
    pthread_mutex_lock(&pin_lock);
    pin_calls++;
    pin_out[pin] = out;
    if (out)
        pin_level[pin] = level;
    pins_changed();
    pthread_mutex_unlock(&pin_lock);
}

static void count_call(void) {
    pthread_mutex_lock(&pin_lock);
    pin_calls++;
    pthread_mutex_unlock(&pin_lock);
}

bool softspi_open() {
    count_call();
    return true;
}
void softspi_init() {
    softspi_open();
    softspi_drive_all(0);
}
void softspi_drive_all(int level) {
    for (int i = 0; i < SOFTSPI_PINS; i++)
        set(i, 1, level);
}
void softspi_drive_pin(softspi_pin_t pin, int level) { set(pin, 1, level); }
void softspi_float_all() {
    for (int i = 0; i < SOFTSPI_PINS; i++)
        set(i, 0, 0);
}
void softspi_close() { softspi_float_all(); }
void softspi_set_direction(softspi_direction_t direction) { set(SOFTSPI_DAT, direction == SOFTSPI_OUTPUT, 0); }
void softspi_release_gpio(int gpio) { set(gpio - GPIO_MODULE_CLK, 0, 0); }
void softspi_set_pin(softspi_pin_t pin, int value) {
    pthread_mutex_lock(&pin_lock);
    pin_calls++;
    pin_level[pin] = !!value; // an input keeps reading its pull-up
    pins_changed();
    pthread_mutex_unlock(&pin_lock);
}
int softspi_get_pin(softspi_pin_t pin) {
    pthread_mutex_lock(&pin_lock);
    pin_calls++;
    const int v = (lines_now() >> pin) & 1;
    pthread_mutex_unlock(&pin_lock);
    return v;
}

// SteadyView X (steadyview_x.c built without EMULATOR_BUILD): gpio, uart and the
// soft_uart module (TX = PG0 = CLK, RX = PG1 = DAT; insmod makes TX an output, high)
void gpio_open(int port_num) { (void)port_num, count_call(); }
void gpio_set(int port_num, bool val) { set(port_num - GPIO_MODULE_CLK, 1, val); }
static int uart_bytes;
int uart_open(int port) {
    (void)port;
    count_call();
    return 7;
}
int uart_write(int fd, uint8_t *data, int len) {
    (void)fd, (void)data;
    set(SOFTSPI_CLK, 1, 0); // (a start bit; the bits themselves are not modelled)
    set(SOFTSPI_CLK, 1, 1);
    uart_bytes += len;
    return len;
}
void uart_close(int fd) { (void)fd, count_call(); }
static int insmods, rmmods;
int test_system(const char *cmd) {
    if (strstr(cmd, "insmod")) {
        insmods++;
        set(SOFTSPI_CLK, 1, 1);
    } else if (strstr(cmd, "rmmod")) {
        rmmods++; // soft_uart leaves TX an output; steadyview_x_close() releases the pins
    }
    return 0;
}

// ---- bay power model (the power thread's reports and SteadyView's pulse) ----

#define MAX_POWER 64
static struct {
    uint64_t t; // us since t0
    int what;   // 1 on (reported), 0 off (reported), 2 SteadyView pulse: off, 3 its on
} power_ev[MAX_POWER];
static int n_power;
static bool bay_on;

static void power_event(int what) {
    pthread_mutex_lock(&pin_lock);
    if (n_power < MAX_POWER)
        power_ev[n_power++].t = now_us() - t0, power_ev[n_power - 1].what = what;
    pthread_mutex_unlock(&pin_lock);
}

static void power(int on) {
    power_event(on);
    bay_on = on;
    module_power_changed(on);
}

bool external_analog_power_cycle(void (*while_off)(void)) {
    if (!bay_on)
        return false;
    power_event(2);
    usleep(70000);
    if (while_off)
        while_off();
    usleep(50);
    power_event(3);
    return true;
}

static void trace_reset(void) {
    pthread_mutex_lock(&pin_lock);
    t0 = now_us();
    n_edges = 0;
    n_power = 0;
    pins_changed();
    pthread_mutex_unlock(&pin_lock);
}

// ---- test helpers ----

static int failures, checks;

static void check(int ok, const char *what) {
    printf("  %-84s %s\n", what, ok ? "ok" : "FAIL");
    failures += !ok;
    checks++;
}

static void sleep_ms(unsigned ms) { usleep(ms * 1000); }

static uint64_t t_ms(void) { return (now_us() - t0) / 1000; }

static void sleep_until_ms(uint64_t t) {
    while (t_ms() < t)
        usleep(2000);
}

// the three line levels at time t (us)
static uint8_t lines_at(uint64_t t) {
    uint8_t v = 7;
    for (int i = 0; i < n_edges && edges[i].t <= t; i++)
        v = edges[i].line;
    return v;
}

// output mask at time t (us)
static uint8_t outs_at(uint64_t t) {
    uint8_t v = 0;
    for (int i = 0; i < n_edges && edges[i].t <= t; i++)
        v = edges[i].out;
    return v;
}

// first time >= from (us) at which a pin is an output, UINT64_MAX: none
static uint64_t first_out_from(uint64_t from) {
    if (outs_at(from))
        return from;
    for (int i = 0; i < n_edges; i++)
        if (edges[i].t >= from && edges[i].out)
            return edges[i].t;
    return UINT64_MAX;
}

// time (us) from which all three pins are inputs for good, 0: they never are
static uint64_t floating_since(void) {
    for (int i = n_edges - 1; i >= 0; i--)
        if (edges[i].out)
            return i + 1 < n_edges ? edges[i + 1].t : 0;
    return edges[0].t;
}

static void select_type(int type, int setting) {
    g_setting.module.type = type;
    g_setting.module.setting = setting;
    module_settings_changed(false);
}

// ---- True-D "1-8" models, code bit i = line pin_of_bit[i] ----

// MOD 1.16 bayPoll(): one loop pass every 20 ms from loop_start_ms; a code taken
// after 300 ms the same, 7 only after another code was taken
static int trued16_channel(const softspi_pin_t pin_of_bit[3], uint64_t loop_start_ms, uint64_t end_ms, int start_channel,
                           int *taken) {
    int applied = -1, cand = -1, ch = start_channel;
    uint64_t cand_t = 0;
    *taken = 0;
    for (uint64_t t = loop_start_ms; t <= end_ms; t += 20) {
        const uint8_t l = lines_at(t * 1000);
        int n = 0;
        for (int b = 0; b < 3; b++)
            n |= ((l >> pin_of_bit[b]) & 1) << b;
        if (n != cand) {
            cand = n;
            cand_t = t;
        } else if (t - cand_t >= 300 && n != applied && (n != 7 || applied != -1)) {
            applied = n;
            ch = n; // channel n + 1 of its band
            (*taken)++;
        }
    }
    return ch;
}

// MOD 1.17 bayPoll() (BAY_18): read every 10 ms from start_ms (its boot screen;
// the loop is slower, 10 ms is the worst case for a short code); a code taken
// after 300 ms the same and when it differs from the one applied last; 7 only once
// another code has read the same twice in a row
static int trued17_channel(const softspi_pin_t pin_of_bit[3], uint64_t start_ms, uint64_t end_ms, int start_channel,
                           int *taken) {
    int applied = -1, cand = -1, ch = start_channel, other = 0;
    uint64_t cand_t = 0;
    *taken = 0;
    for (uint64_t t = start_ms; t <= end_ms; t += 10) {
        const uint8_t l = lines_at(t * 1000);
        int n = 0;
        for (int b = 0; b < 3; b++)
            n |= ((l >> pin_of_bit[b]) & 1) << b;
        if (n != cand) {
            cand = n;
            cand_t = t;
        } else {
            if (n != 7)
                other = 1;
            if (t - cand_t >= 300 && n != applied && (n != 7 || other)) {
                applied = n;
                ch = n;
                (*taken)++;
            }
        }
    }
    return ch;
}

// ---- rapidFIRE: frames on the pins ----

typedef struct {
    uint64_t start, end; // us: CS low, CS high
    int bits;
    uint8_t b[8];
    uint64_t min_gap;    // shortest time between two successive CS / CLK edges in the frame (CS low .. CS high)
    int data_bad;        // DATA changed while CLK was high, or < 100 us before a CLK rise
} frame_t;

static int decode_spi(frame_t *frames, int max, uint64_t from_us) {
    int n = 0, in = 0;
    uint8_t prev = 7;
    uint64_t last_edge = 0, last_dat = 0;
    frame_t f;
    for (int i = 0; i < n_edges; i++) {
        const uint8_t l = edges[i].line;
        const uint64_t t = edges[i].t;
        const int cs = (l >> SOFTSPI_CS) & 1, clk = (l >> SOFTSPI_CLK) & 1, dat = (l >> SOFTSPI_DAT) & 1;
        const int pcs = (prev >> SOFTSPI_CS) & 1, pclk = (prev >> SOFTSPI_CLK) & 1, pdat = (prev >> SOFTSPI_DAT) & 1;
        if (t >= from_us) {
            if (in && dat != pdat && cs == pcs) {
                if (clk)
                    f.data_bad = 1; // DATA while CLK high
                last_dat = t;
            }
            if (!in && pcs && !cs) {
                in = 1;
                memset(&f, 0, sizeof(f));
                f.start = t;
                f.min_gap = UINT64_MAX;
                last_edge = t;
                last_dat = dat != pdat ? t : 0;
            } else if (in && !cs && clk != pclk) {
                if (t - last_edge < f.min_gap)
                    f.min_gap = t - last_edge;
                last_edge = t;
                if (clk) {
                    if (last_dat && t - last_dat < 100)
                        f.data_bad = 1;
                    if (f.bits < 64)
                        f.b[f.bits / 8] |= dat << (7 - f.bits % 8);
                    f.bits++;
                }
            } else if (in && cs) {
                in = 0;
                f.end = t;
                if (f.bits) { // (the all-low step of the entry clocks nothing)
                    if (t - last_edge < f.min_gap)
                        f.min_gap = t - last_edge;
                    if (n < max)
                        frames[n++] = f;
                }
            }
        }
        prev = l;
    }
    return n;
}

static int frame_ok(const frame_t *f, char cmd, int value) {
    const uint8_t crc = (uint8_t)(f->b[0] + f->b[1] + f->b[2] + f->b[4]);
    return f->bits == 40 && f->b[0] == (uint8_t)cmd && f->b[1] == '=' && f->b[2] == 1 && f->b[3] == crc &&
           f->b[4] == value;
}

// SPI mode entry: all three driven high, then all low, each >= 190 ms (the pins
// change one after the other); returns its start (us), 0: none
static uint64_t find_entry(uint64_t from, uint64_t *low_at, uint64_t *low_end) {
    int i = 0;
    while (i < n_edges && !(edges[i].t >= from && edges[i].line == 7 && edges[i].out == 7))
        i++;
    if (i == n_edges)
        return 0;
    const uint64_t high = edges[i].t;
    while (i < n_edges && edges[i].line != 0)
        i++;
    if (i == n_edges)
        return 0;
    *low_at = edges[i].t;
    while (i < n_edges && edges[i].line == 0)
        i++;
    if (i == n_edges)
        return 0;
    *low_end = edges[i].t;
    return *low_at - high >= 190000 && *low_end - *low_at >= 190000 ? high : 0;
}

// MOD 1.17's rapidFIRE receiver (TrueD.ino PCINT0 stub + bayFrame() + bayPoll()'s
// expiry), every edge seen: returns the channels it takes (0..47 in A B E F R L
// order), listening from start_us
typedef struct {
    uint64_t t;
    int ch;
} taken_t;
static int trued17_rapidfire(uint64_t start_us, taken_t *out, int max, int start_channel) {
    static const int rf_band_to_table[7] = {-1, 3, 4, 2, 1, 0, 5}; // rapidFIRE 1 F .. 6 L -> A0 B1 E2 F3 R4 L5
    int bits = 0xFF, acc = 0, b0 = 0, arm = 0xFF, armc = 0, fails = 0, freec = 1, n = 0, cur = start_channel;
    uint8_t f[4] = {0};
    uint64_t arm_t = 0, fall_t = 0, win_t = start_us;
    uint8_t prev = lines_at(start_us);
    for (int i = 0; i < n_edges; i++) {
        if (edges[i].t <= start_us)
            continue;
        const uint8_t l = edges[i].line;
        const uint64_t t = edges[i].t;
        const int cs = (l >> SOFTSPI_CS) & 1, clk = (l >> SOFTSPI_CLK) & 1, dat = (l >> SOFTSPI_DAT) & 1;
        const int pcs = (prev >> SOFTSPI_CS) & 1, pclk = (prev >> SOFTSPI_CLK) & 1;
        prev = l;
        if (cs != pcs && !cs) { // CS fell: a window opens
            bits = 0;
            fall_t = t;
        } else if (cs != pcs && cs) { // CS rose: bayFrame()
            if (bits == 0xFF)
                continue;
            const int nb = bits;
            bits = 0xFF;
            const uint64_t quiet = fall_t - win_t;
            win_t = t;
            if (!nb)
                continue;
            if (arm != 0xFF && t - arm_t >= 1000000)
                arm = 0xFF; // (bayPoll()'s expiry)
            const int c = b0, b1 = f[0], b2 = f[1], b3 = f[2], b4 = f[3];
            int ch = -1;
            if (nb == 40 && b1 == '=' && b2 == 1 && b3 == ((c + '=' + 1 + b4) & 0xFF)) {
                if (c == 'B') {
                    arm = b4 && b4 <= 6 ? b4 : 0xFF;
                    armc = 0;
                    arm_t = t;
                    fails = 0;
                    freec = 0;
                } else if (c == 'C' && b4 && b4 <= 8) {
                    if (arm != 0xFF) {
                        if (t - arm_t < 1000000 && (!armc || armc == b4)) {
                            armc = b4;
                            ch = rf_band_to_table[arm] * 8 + b4 - 1;
                            freec = 1;
                        }
                    } else if (freec && quiet >= 2000000) {
                        ch = (cur & ~7) + b4 - 1; // alone: the current band
                    }
                }
            } else if (nb == 32 && (b2 == '>' || b2 == '?') && !b3 && b4 == ((b1 + b2) & 0xFF)) {
                // buzzer / query: nothing here
            } else { // a failed window
                freec = 0;
                if (nb > 40 || fails || armc)
                    arm = 0xFF;
                else
                    fails = 1;
            }
            if (ch >= 0 && ch != cur) {
                cur = ch;
                if (n < max)
                    out[n++] = (taken_t){t, ch};
            }
        } else if (!cs && clk && !pclk && bits != 0xFF && bits < 41) { // a bit
            bits++;
            acc = (acc << 1 | dat) & 0xFF;
            if (!(bits & 7)) {
                if (bits == 8)
                    b0 = acc;
                memmove(f, f + 1, 3);
                f[3] = acc;
            }
        }
    }
    return n;
}

// Signal storm: SIGUSR1 is blocked in every thread but the module worker, so each
// kill() lands there and cuts its current sleep short (no SA_RESTART).
static volatile sig_atomic_t storm_on, storm_hits;
static void on_storm(int sig) {
    (void)sig;
    storm_hits++;
}
static void *storm_thread(void *arg) {
    (void)arg;
    while (storm_on) {
        kill(getpid(), SIGUSR1);
        usleep(29);
    }
    return NULL;
}

static void write_trace(const char *path, const char *what) {
    FILE *fp = fopen(path, "w");
    if (!fp)
        return;
    fprintf(fp, "# %s\n# t_us lines out (bit 0 PG0 = CLK, bit 1 PG1 = DATA, bit 2 PG2 = CS); power events: P t_us what\n",
            what);
    for (int i = 0; i < n_power; i++)
        fprintf(fp, "P %llu %d\n", (unsigned long long)power_ev[i].t, power_ev[i].what);
    for (int i = 0; i < n_edges; i++)
        fprintf(fp, "%llu %d %d\n", (unsigned long long)edges[i].t, edges[i].line, edges[i].out);
    fclose(fp);
}

static const char *type_name[MODULE_TYPES] = {"None", "Switch", "rapidFIRE", "SteadyView", "SteadyView X"};

int main(void) {
    char what[200];
    g_setting.module.type = MODULE_NONE;
    g_setting.module.setting = 0;
    g_setting.wifi.enable = false;
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
    g_setting.source.analog_channel = 1;
    g_setting.module.channel = 8; // A8
    // the worker (created in module_init()) takes SIGUSR1; every other thread blocks it
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_storm; // (no SA_RESTART: an interrupted sleep returns early)
    sigaction(SIGUSR1, &sa, NULL);
    module_init();
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, NULL);
    static const softspi_pin_t order0[3] = {SOFTSPI_CLK, SOFTSPI_DAT, SOFTSPI_CS}; // pin order 0: bit i on PG i

    // ---- stock ----
    printf("Module None (the default), as on a stock goggle:\n");
    trace_reset();
    power(1);
    sleep_ms(2500);
    analog_channel_set(20);
    module_source_changed();
    power(0);
    sleep_ms(10);
    power(1);
    g_setting.wifi.enable = true;
    module_wifi_changed();
    g_setting.wifi.enable = false;
    module_wifi_changed();
    sleep_ms(2500);
    check(n_edges == 1 && edges[0].out == 0 && pin_calls == 0,
          "power-on, dial, source change, power cycle, WiFi on / off: no pin touched at all");
    module_check_pins(); // a user action: reads the pins as inputs
    sleep_ms(300);
    check(n_edges == 1 && edges[0].out == 0 && pin_calls > 0, "  Check Pins reads them, never an output");
    power(0);
    sleep_ms(50);

    // ---- every type, every power path ----
    printf("Every type keeps the pins inputs until 2.0 s after every bay power-on:\n");
    for (int type = MODULE_SWITCH; type < MODULE_TYPES; type++) {
        g_setting.module.channel = 28; // F4
        select_type(type, 1);
        sleep_ms(50);
        trace_reset();
        power(1);                  // cold
        sleep_ms(1000);
        power(0);                  // source away and back within one power-thread tick
        sleep_ms(10);
        power(1);
        sleep_ms(2600);
        power(0);                  // standby, then wake
        sleep_ms(300);
        power(1);
        sleep_ms(2700);
        power(0);
        sleep_ms(300);
        int hold_ok = 1, drove = 0, pulses = 0, pulse_late = 1, nons = 0;
        uint64_t worst = UINT64_MAX;
        for (int i = 0; i < n_power; i++) {
            if (power_ev[i].what == 1) {
                nons++;
                // the next report after it (an off ends the window early)
                uint64_t until = power_ev[i].t + 2000000;
                for (int j = i + 1; j < n_power; j++)
                    if (power_ev[j].what <= 1) {
                        if (power_ev[j].t < until)
                            until = power_ev[j].t;
                        break;
                    }
                const uint64_t o = first_out_from(power_ev[i].t);
                if (o < until)
                    hold_ok = 0;
                if (until == power_ev[i].t + 2000000) {
                    if (o != UINT64_MAX && o - power_ev[i].t < worst)
                        worst = o - power_ev[i].t;
                    drove += o != UINT64_MAX && o < power_ev[i].t + 2600000;
                }
            } else if (power_ev[i].what == 2) {
                pulses++;
                uint64_t last_on = 0;
                for (int j = 0; j < i; j++)
                    if (power_ev[j].what == 1)
                        last_on = power_ev[j].t;
                pulse_late &= power_ev[i].t >= last_on + 2000000;
            }
        }
        snprintf(what, sizeof(what), "%s: 3 power-ons (cold, off / on in 10 ms, standby and back): first drive %.3f s after",
                 type_name[type], worst == UINT64_MAX ? -1.0 : worst / 1e6);
        check(nons == 3 && hold_ok && drove == 2 && worst >= 2000000, what);
        if (type == MODULE_STEADYVIEW) {
            snprintf(what, sizeof(what), "  its mode-set pulses (%d, the protocol's driven power-up) >= 2.0 s after each power-on",
                     pulses);
            check(pulses == 2 && pulse_late, what);
        } else {
            check(pulses == 0, "  no other power cycle");
        }
        // (SteadyView X: close() waits 100 ms for soft_uart before unloading it)
        check(floating_since() && floating_since() < power_ev[n_power - 1].t + (type == MODULE_STEADYVIEW_X ? 250000 : 60000),
              "  pins inputs after the last power-off");
        if (getenv("TRACE"))
            printf("    floating since %llu us, last power event %llu us (%d)\n", (unsigned long long)floating_since(),
                   (unsigned long long)power_ev[n_power - 1].t, power_ev[n_power - 1].what);
    }

    // ---- SteadyView register reads (review defect 10) ----
    printf("SteadyView, channel set-up and a change (each reads the RTC6705 register first):\n");
    g_setting.module.channel = 28; // F4
    select_type(MODULE_STEADYVIEW, 0);
    sleep_ms(50);
    trace_reset();
    power(1);
    sleep_ms(3500);
    analog_channel_set(5); // A5
    sleep_ms(400);
    {
        // the goggle drives DATA again only once CS is up, i.e. once the RTC6705 has let go of it
        int turns = 0, turns_bad = 0;
        for (int k = 1; k < n_edges; k++) {
            const int cs_out = (edges[k - 1].out >> SOFTSPI_CS) & 1;
            if (cs_out && !((edges[k - 1].out >> SOFTSPI_DAT) & 1) && ((edges[k].out >> SOFTSPI_DAT) & 1)) {
                turns++;
                turns_bad += !((edges[k].line >> SOFTSPI_CS) & 1);
            }
        }
        snprintf(what, sizeof(what), "register reads (%d): DATA an output again only after CS is up", turns);
        check(turns >= 2 && !turns_bad, what);
    }
    power(0);
    sleep_ms(100);

    // ---- Switch ----
    printf("Switch, channel 8 (A8) right after bay power-on:\n");
    g_setting.module.channel = 8;
    select_type(MODULE_SWITCH, 0);
    sleep_ms(50);
    trace_reset();
    power(1);
    sleep_until_ms(6500);
    check(lines_at(1999000) == 7 && first_out_from(0) >= 2000000, "inputs (read 7) until the set-up 2.0 s after power-on");
    check(lines_at(2100000) == 2, "code 2 for 200 ms at init");
    check(lines_at(2300000) == 6 && lines_at(4900000) == 6, "then code 6 (channel 7) until 5 s after power-on");
    check(lines_at(5100000 + SLACK_MS * 1000) == 7, "then code 7 (channel 8)");
    for (int loop = 3100; loop <= 3900; loop += 400) {
        int taken;
        const int ch = trued16_channel(order0, loop, 6500, 3, &taken);
        snprintf(what, sizeof(what), "True-D MOD 1.16 1-8 model, loop from %.1f s: ends on channel 8 (%d codes taken)",
                 loop / 1000.0, taken);
        check(ch == 7, what);
    }
    {
        int taken;
        const int ch = trued17_channel(order0, 1450, 6500, 3, &taken);
        snprintf(what, sizeof(what), "True-D MOD 1.17 1-8 model, read from 1.45 s: ends on channel 8 (%d codes taken)", taken);
        check(ch == 7, what);
    }
    power(0);
    sleep_ms(100);
    check(floating_since() && floating_since() < 6500000 + 100000, "power-off: pins inputs again");

    printf("Switch, channel 4 (A4), pin order PG0=4 PG1=1 PG2=2:\n");
    g_setting.module.channel = 4;
    select_type(MODULE_SWITCH, 1);
    sleep_ms(50);
    trace_reset();
    power(1);
    sleep_until_ms(4500);
    static const softspi_pin_t order1[3] = {SOFTSPI_DAT, SOFTSPI_CS, SOFTSPI_CLK};
    int taken;
    check(lines_at(2100000) == (1 << SOFTSPI_CS), "init: code 2 = bit 1 = PG2 high only");
    check(lines_at(2300000) == ((1 << SOFTSPI_DAT) | (1 << SOFTSPI_CS)) && trued16_channel(order1, 3500, 4500, 0, &taken) == 3 &&
              trued17_channel(order1, 1450, 4500, 0, &taken) == 3,
          "code 3 (PG1, PG2) at once after it, taken by both models: channel 4");
    int low7 = 0; // rapidFIRE legacy guard: never all high -> all low
    for (int i = 1; i < n_edges; i++)
        low7 |= edges[i - 1].line == 7 && edges[i].line == 0;
    check(!low7, "never straight from all high to all low (rapidFIRE SPI entry)");
    power(0);
    sleep_ms(100);

    // ---- rapidFIRE ----
    printf("rapidFIRE, R1 (33), mode rapidFIRE 2 (D=1):\n");
    g_setting.module.channel = 33;
    select_type(MODULE_RAPIDFIRE, 1);
    sleep_ms(50);
    trace_reset();
    power(1);
    sleep_until_ms(5000 + 1000);
    uint64_t low_at = 0, low_end = 0;
    const uint64_t entry = find_entry(0, &low_at, &low_end);
    check(entry >= 2000000 && entry < (2000 + SLACK_MS) * 1000ULL, "SPI mode entry 2.0 s after power-on (all high 200 ms, all low 200 ms)");
    int high_first = 1, first_out = 0;
    for (int i = 0; i < n_edges && edges[i].t <= entry; i++) {
        high_first &= edges[i].line == 7;
        if (!first_out)
            first_out = edges[i].out;
    }
    check(high_first && first_out_from(0) >= 2000000 && entry - first_out_from(0) < 2000,
          "  pins inputs before it, straight to all high");
    check(first_out == 1 << SOFTSPI_CS, "  CS driven high first (CLK then rises with CS high: no clock edge in a window)");
    static frame_t fr[256];
    const int nf = decode_spi(fr, 64, 0);
    int ok = nf == 15;
    const char cmds[15] = {'D', 'D', 'D', 'B', 'B', 'B', 'C', 'C', 'C', 'B', 'B', 'B', 'C', 'C', 'C'};
    const int vals[15] = {1, 1, 1, 2, 2, 2, 1, 1, 1, 2, 2, 2, 1, 1, 1}; // IMRC band 2 = RaceBand, channel 1
    for (int i = 0; ok && i < 15; i++)
        ok = frame_ok(&fr[i], cmds[i], vals[i]);
    snprintf(what, sizeof(what), "frames: D=1 x3, B=2 x3, C=1 x3 at set-up, B, C again (decoded %d)", nf);
    check(ok, what);
    if (nf == 15) {
        check(fr[0].start >= low_end + 100000 && fr[0].start < low_end + (100 + SLACK_MS) * 1000ULL, "  mode 100 ms after the entry");
        check(fr[9].start >= 5000000 && fr[9].start < (5000 + SLACK_MS) * 1000ULL, "  resend at 5.0 s after power-on");
        check(fr[6].start - fr[5].end >= 100000 && fr[6].start - fr[5].end < 350000,
              "  channel 0.1-0.35 s after the last band frame (MOD 1.17 pairs within 1 s)");
        check(fr[1].start - fr[0].end >= 10000, "  10 ms between repeats");
        printf("    trace (host): entry %.3f s, low %.3f-%.3f s, D %.3f s, B %.3f s, C %.3f s, resend B %.3f s C %.3f s\n",
               entry / 1e6, low_at / 1e6, low_end / 1e6, fr[0].start / 1e6, fr[3].start / 1e6, fr[6].start / 1e6,
               fr[9].start / 1e6, fr[12].start / 1e6);
    }
    check(lines_at(5900000) == (1 << SOFTSPI_CS), "idle between commands: CS high, CLK and DATA low");

    printf("rapidFIRE, channel changes (every band), frame timing, MOD 1.17 receiver model:\n");
    static const int changes[] = {28, 40, 1, 21, 15, 47, 33, 34}; // F4 R8 A1 E5 B7 L7 R1 R2
    const uint64_t t_changes = now_us() - t0;
    for (unsigned i = 0; i < ARRAY_SIZE(changes); i++) {
        analog_channel_set(changes[i]);
        sleep_ms(450);
    }
    const int nf2 = decode_spi(fr, 256, 0);
    uint64_t gap_min = UINT64_MAX, len_min = UINT64_MAX, len_max = 0, len_sum = 0;
    int data_bad = 0, all40 = 1;
    for (int i = 0; i < nf2; i++) {
        if (fr[i].min_gap < gap_min)
            gap_min = fr[i].min_gap;
        const uint64_t len = fr[i].end - fr[i].start;
        len_min = len < len_min ? len : len_min;
        len_max = len > len_max ? len : len_max;
        len_sum += len;
        data_bad += fr[i].data_bad;
        all40 &= fr[i].bits == 40;
    }
    snprintf(what, sizeof(what), "%d frames: every CS / CLK edge >= 100 us after the one before (shortest %llu us)", nf2,
             (unsigned long long)gap_min);
    check(nf2 == 15 + 6 * (int)ARRAY_SIZE(changes) && all40 && gap_min >= 100, what);
    check(!data_bad, "  DATA changed only while CLK is low, >= 100 us before the next rise");
    printf("    frame length (host): %.2f-%.2f ms, mean %.2f ms (81 waits of >= 100 us)\n", len_min / 1e3, len_max / 1e3,
           nf2 ? len_sum / 1e3 / nf2 : 0.0);
    taken_t tk[32];
    const int ntk = trued17_rapidfire(1450000, tk, 32, 27); // listening from 1.45 s, on F4 (27) before
    int seq_ok = ntk == 1 + (int)ARRAY_SIZE(changes) && tk[0].ch == 32 && tk[0].t < 3000000;
    for (unsigned i = 0; seq_ok && i < ARRAY_SIZE(changes); i++)
        seq_ok = tk[i + 1].ch == changes[i] - 1 && tk[i + 1].t > t_changes + i * 450000 &&
                 tk[i + 1].t < t_changes + (i + 1) * 450000;
    snprintf(what, sizeof(what), "MOD 1.17 model: R1 at set-up (%.2f s), then each change in its command (%d taken)",
             ntk ? tk[0].t / 1e6 : 0.0, ntk);
    check(seq_ok, what);
    const char *trace_out = getenv("TRACE_OUT");
    if (trace_out)
        write_trace(trace_out, "rapidFIRE: power-on at the first P event, R1, mode D=1, then F4 R8 A1 E5 B7 L7 R1 R2");

    printf("rapidFIRE under a signal storm (every sleep of the worker cut short again and again):\n");
    static const int storm_changes[] = {20, 9, 44}; // E4 B1 L4
    const uint64_t t_storm = now_us() - t0;
    storm_on = 1;
    pthread_t storm_tid;
    const int storm_started = pthread_create(&storm_tid, NULL, storm_thread, NULL) == 0;
    for (unsigned i = 0; i < ARRAY_SIZE(storm_changes); i++) {
        analog_channel_set(storm_changes[i]);
        sleep_ms(450);
    }
    storm_on = 0;
    if (storm_started)
        pthread_join(storm_tid, NULL);
    const int nfs = decode_spi(fr, 256, t_storm);
    uint64_t gap_storm = UINT64_MAX;
    int bad_storm = 0, ok_storm = nfs == 6 * (int)ARRAY_SIZE(storm_changes);
    for (int i = 0; i < nfs; i++) {
        gap_storm = fr[i].min_gap < gap_storm ? fr[i].min_gap : gap_storm;
        bad_storm += fr[i].data_bad;
        ok_storm &= fr[i].bits == 40;
    }
    snprintf(what, sizeof(what), "%d frames, %d signals taken by the worker: every CS / CLK edge >= 100 us apart (shortest %llu us)",
             nfs, (int)storm_hits, (unsigned long long)gap_storm);
    check(storm_started && storm_hits > 1000 && ok_storm && gap_storm >= 100 && !bad_storm, what);

    printf("rapidFIRE, power-off in mid-command:\n");
    analog_channel_set(48); // L8
    sleep_ms(50);
    const uint64_t t_off = now_us() - t0;
    power(0);
    sleep_ms(100);
    const uint64_t fl = floating_since();
    snprintf(what, sizeof(what), "pins inputs %.1f ms after the power-off report (< 40 ms: one frame)",
             fl > t_off ? (fl - t_off) / 1000.0 : 0.0);
    check(fl && fl < t_off + 40000, what);

    // ---- SteadyView X ----
    printf("SteadyView X:\n");
    select_type(MODULE_STEADYVIEW_X, 0);
    sleep_ms(50);
    trace_reset();
    insmods = rmmods = uart_bytes = 0;
    power(1);
    sleep_until_ms(1900);
    check(insmods == 0 && first_out_from(0) == UINT64_MAX, "nothing before 2.0 s after power-on (no soft_uart, inputs)");
    sleep_until_ms(2600);
    check(insmods == 1 && (outs_at(2500000) & (1 << SOFTSPI_CS)) && !(lines_at(2500000) & (1 << SOFTSPI_CS)) && uart_bytes > 0,
          "then soft_uart loaded, CS low, mode and channel sent");
    power(0);
    sleep_ms(400);
    check(rmmods == 1 && floating_since() && outs_at((now_us() - t0)) == 0, "power-off: soft_uart unloaded, pins inputs");
    select_type(MODULE_NONE, 0);
    sleep_ms(50);

    printf("%d checks, %s\n", checks, failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
