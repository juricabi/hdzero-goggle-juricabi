#include <stdbool.h>
#include <time.h>

#include "core/common.hh"
#include "core/settings.h"

#include "rapidfire.h"
#include "softspi.h"

// ImmersionRC rapidFIRE SPI API on the bay pins (CS, DATA, CLK; pkendall's
// analog-module-control, Goggle 1).  The goggle only writes: nothing here reads a
// pin or depends on an answer, so a module that only listens (a Furious True-D
// with MOD 1.17, BAY:RAPIDFIRE) gets the same as a rapidFIRE.
//
//   2.0 s after bay power-on (MODULE_BOOT_MS; the ExpressLRS backpack's
//   VRX_BOOT_DELAY), the pins inputs until then:
//   init      pins from input straight to all three high (CS first), 200 ms; all
//             low, 200 ms (the SPI mode entry, as the ExpressLRS backpack does it);
//             then CS high, CLK and DATA low (idle, kept while the module is
//             controlled)
//   mode      100 ms later 'D' '=' 0x01 crc mode
//   channel   right after: 'B' '=' 0x01 crc band (IMRC numbering), 100 ms, 'C' '='
//             0x01 crc channel (1-8); crc = (cmd + '=' + 1 + value) & 0xFF
//   again     5.0 s after bay power-on: band and channel once more (a module that
//             boots slower); channel changes in between wait for it
// Each command goes out 3 times, 10 ms apart.  A channel change is always band,
// then channel: a True-D takes a channel only with its band frame up to 1 s before.
//
// Frame (SPI mode 0, MSB first: the module samples DATA at the rising CLK edge):
//   CS low, DATA = bit 7 of byte 0, >= RF_PHASE_US
//   per bit: CLK high, >= RF_PHASE_US; CLK low, DATA = the next bit, >= RF_PHASE_US
//   CS high, DATA low (idle)
// So any two successive edges of CS and CLK are >= RF_PHASE_US apart on the pins (CS
// low to the first CLK rise, every CLK level, the last CLK fall to CS high), and DATA
// changes only right after a CLK fall, >= RF_PHASE_US before the next rise.  A True-D
// reads a bay edge <= 43.1 us after it in its worst case and needs >= 100 us per level
// and gap (furiousdfw docs/bay-standards.md, section 4); a real rapidFIRE takes up to
// ~80 kHz (>= 6.25 us per level).  81 waits per frame: >= 8.1 ms guaranteed; each wait
// overshoots by the timer slack (50 us for a normal thread), the wake-up latency and
// the sysfs writes, never less: ~13.5-14.8 ms per frame on a PC, an estimated
// ~12-20 ms on the goggle (not measured).

#define SPAM_COUNT 3

#define RF_API_DIR_GRTHAN  '>'
#define RF_API_DIR_EQUAL   '='
#define RF_API_BEEP_CMD    'S'
#define RF_API_BAND_CMD    'B'
#define RF_API_CHANNEL_CMD 'C'
#define RF_API_MODE_CMD    'D'

#define SPAM_GAP_MS 10  // between the repeats of a command
#define CMD_GAP_MS  100 // between two commands

#ifndef RF_PHASE_US
#define RF_PHASE_US 100 // the shortest time between two successive frame edges
#endif

static bool before(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

// Waits until RF_PHASE_US after the pin write that has just returned.  The edge
// happened inside that write, the next one happens inside the next write, which
// starts only once the clock has been read past an absolute CLOCK_MONOTONIC
// deadline: a sleep can only end late (timer slack, scheduling, preemption), and
// one that ends early for any reason (a signal, an error) is simply repeated, so
// the gap on the pins is >= RF_PHASE_US whatever the scheduler does.
static void phase_wait(void) {
    struct timespec t, now;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_nsec += RF_PHASE_US * 1000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    do {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (before(&now, &t));
}

// Every wait between frames is an abort point (module_sleep_ms()): a release
// request stops a transfer after the frame on the wire.
static bool send_cmd(uint8_t command, uint8_t value);

static void rapidfire_set_channel(int index) {
    // convert standard band index (A B E F R L) to IMRC band index
    static const uint8_t band_map[] = {5, 4, 3, 1, 2, 6};
    const int band = index / 8;
    if (index < 0 || band >= (int)ARRAY_SIZE(band_map))
        return;
    if (!send_cmd(RF_API_BAND_CMD, band_map[band]) || !module_sleep_ms(CMD_GAP_MS))
        return; // pins to be released
    send_cmd(RF_API_CHANNEL_CMD, index % 8 + 1); // IMRC channel is 1-based
}

static void rapidfire_set_mode() {
    const int mode = g_setting.module.setting;
    if (!module_sleep_ms(CMD_GAP_MS))
        return;
    send_cmd(RF_API_MODE_CMD, mode >= 0 && mode <= 2 ? mode : 0); // (only 0-2 exist)
    // the worker sends the channel next
}

static void rapidfire_init() {
    softspi_open(); // inputs
    // straight to high, no low step before the entry; CS first, so that CLK rises
    // with CS already high: a listener that saw CS float low takes no clock edge
    softspi_drive_pin(SOFTSPI_CS, 1);
    softspi_drive_pin(SOFTSPI_CLK, 1);
    softspi_drive_pin(SOFTSPI_DAT, 1);
    if (!module_sleep_ms(200))
        return;
    softspi_set_pin(SOFTSPI_CLK, 0);
    softspi_set_pin(SOFTSPI_CS, 0);
    softspi_set_pin(SOFTSPI_DAT, 0);
    if (!module_sleep_ms(200))
        return;
    softspi_set_pin(SOFTSPI_CS, 1);

    rapidfire_set_mode();
}

static void rapidfire_close() {
    softspi_close();
}

// CRC function for IMRC rapidfire API
// Input: byte array, array length
// Output: crc byte
static uint8_t crc8(uint8_t *buf, uint8_t bufLen) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < bufLen; ++i) {
        sum += buf[i];
    }
    return sum & 0xFF;
}

// One frame, from idle (CS high, CLK low, DATA low) back to idle.
static void send_spi(const uint8_t *buf, uint8_t len) {
    const int bits = len * 8;
    softspi_set_pin(SOFTSPI_CS, 0);
    softspi_set_pin(SOFTSPI_DAT, buf[0] & 0x80);
    phase_wait();
    for (int i = 0; i < bits; i++) {
        softspi_set_pin(SOFTSPI_CLK, 1); // the module samples DATA here
        phase_wait();
        softspi_set_pin(SOFTSPI_CLK, 0);
        if (i + 1 < bits)
            softspi_set_pin(SOFTSPI_DAT, (buf[(i + 1) / 8] << ((i + 1) % 8)) & 0x80);
        phase_wait();
    }
    softspi_set_pin(SOFTSPI_CS, 1);
    softspi_set_pin(SOFTSPI_DAT, 0); // idle: CS high, CLK and DATA low
}

// One command, SPAM_COUNT times (the rapidFIRE sometimes misses a packet).  False
// when it was cut short because the pins are to be released.
static bool send_cmd(uint8_t command, uint8_t value) {
    uint8_t cmd[5];
    cmd[0] = command;
    cmd[1] = RF_API_DIR_EQUAL;
    cmd[2] = 0x01;         // len
    cmd[3] = value;        // temporarily set byte 4 to the value for crc calc
    cmd[3] = crc8(cmd, 4); // reset byte 4 to crc
    cmd[4] = value;        // assign the value to byte 5

    for (int i = 0; i < SPAM_COUNT; i++) {
        send_spi(cmd, 5);
        if (!module_sleep_ms(SPAM_GAP_MS))
            return false;
    }
    return true;
}

// 48 channels, A B E F R L: the analog channel setting, its range check at load,
// the internal receiver and the ELRS VTX channel are all 48 wide (pkendall's
// table also had the IMRC X band as channels 49-56, which nothing else can hold).
module_def_t rapidfire_module = {
    .num_channels = 48,
    .name = module_standard_channel_name,
    .init = rapidfire_init,
    .close = rapidfire_close,
    .set_channel = rapidfire_set_channel,
    .set_mode = rapidfire_set_mode,
    // 'D' value = list entry: 0 rapidFIRE #1, 1 rapidFIRE #2, 2 legacy (the rapidFIRE
    // SPI API as GOROman/rapidFIRE_SPI has it; pkendall's list showed 0 as "legacy").
    // A True-D ignores 'D'.
    .mode_names = "rapidFIRE 1\nrapidFIRE 2\nLegacy",
    .boot_resend_ms = 5000, // a module that boots slower (a True-D reads from ~1.45 s, MOD 1.17)
};
