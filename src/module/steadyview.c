#include <unistd.h>

#include <log/log.h>

#include "core/common.hh"
#include "core/settings.h"

#include "driver/hardware.h"

#include "softspi.h"
#include "steadyview.h"


#define RX5808_READ_CTRL_BIT      0x00
#define RX5808_WRITE_CTRL_BIT     0x01
#define RX5808_ADDRESS_R_W_LENGTH 5
#define RX5808_DATA_LENGTH        20
#define RX5808_PACKET_LENGTH      25

#define SYNTHESIZER_REG_A 0x00
#define SYNTHESIZER_REG_B 0x01

static const uint16_t frequencyTable[48] = {
    5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, // A
    5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866, // B
    5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, // E
    5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880, // F
    5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917, // R
    5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621  // L
};

static int current_channel = -1;
static steadyview_mix_mode_t mix_mode = -1;

static const uint32_t periodMicroSec = 100;
static void rtc6705WriteRegister(uint32_t data);
static uint32_t rtc6705ReadRegister(uint8_t read);

static void steadyview_set_channel(int index) {
    uint16_t f = frequencyTable[index];
    uint32_t data = ((((f - 479) / 2) / 32) << 7) | (((f - 479) / 2) % 32);
    uint32_t newRegisterData = SYNTHESIZER_REG_B | (RX5808_WRITE_CTRL_BIT << 4) | (data << 5);

    uint32_t currentRegisterData = SYNTHESIZER_REG_B | (RX5808_WRITE_CTRL_BIT << 4) | rtc6705ReadRegister(SYNTHESIZER_REG_B);

    if (newRegisterData != currentRegisterData) {
        rtc6705WriteRegister(SYNTHESIZER_REG_A | (RX5808_WRITE_CTRL_BIT << 4) | (0x8 << 5));
        rtc6705WriteRegister(newRegisterData);
    }
    current_channel = index;
}

// The SteadyView reads its mix mode from CLK/DAT as it powers up.  This mode-set
// pulse is the one bay power-on with the pins driven, by the protocol: it happens
// only with Module = SteadyView, from its set-up (itself >= MODULE_BOOT_MS after the
// bay's power-on) or a Mode change.  Every other power-on keeps the pins inputs.
static void set_mode_pins(void) {
    softspi_set_pin(SOFTSPI_CLK, mix_mode & 0x01);
    softspi_set_pin(SOFTSPI_DAT, mix_mode & 0x02);
}

static void steadyview_set_mixmode(steadyview_mix_mode_t mode) {
    mix_mode = mode;

    // bay off 70 ms, the mode pins set at its end, bay on: done by the bay power
    // owner (hardware-goggle*.c), which keeps its power thread waiting meanwhile
    if (!external_analog_power_cycle(set_mode_pins)) {
        if (module_abort_requested())
            return; // the bay was switched off: the pins are released next
        LOGW("steadyview: no bay power switch (batch 1), mix mode %d not latched", mode);
        set_mode_pins();
    }
    // (abort points: the waits; a release request ends the mode set there)
    if (!module_sleep_ms(100))
        return;

    softspi_set_pin(SOFTSPI_CLK, 0);
    softspi_set_pin(SOFTSPI_DAT, 1);
    if (!module_sleep_ms(505))
        return;

    uint16_t f = frequencyTable[current_channel];
    uint32_t data = ((((f - 479) / 2) / 32) << 7) | (((f - 479) / 2) % 32);
    uint32_t registerData = SYNTHESIZER_REG_B | (RX5808_WRITE_CTRL_BIT << 4) | (data << 5);

    rtc6705WriteRegister(SYNTHESIZER_REG_A | (RX5808_WRITE_CTRL_BIT << 4) | (0x8 << 5));
    usleep(3000);
    rtc6705WriteRegister(SYNTHESIZER_REG_A | (RX5808_WRITE_CTRL_BIT << 4) | (0x8 << 5));
    usleep(9000);
    rtc6705WriteRegister(registerData);
    usleep(10);
}

static void steadyview_set_mode() {
    // the mode sequence ends with the channel register (the worker sends the channel
    // again afterwards; steadyview_set_channel() skips it when it reads back the same)
    current_channel = module_current_channel();
    steadyview_set_mixmode(g_setting.module.setting);
}

static void steadyview_init() {
    softspi_init();

    if (!module_sleep_ms(50))
        return;
    softspi_set_pin(SOFTSPI_CLK, 0);
    softspi_set_pin(SOFTSPI_CS, 1);
    softspi_set_pin(SOFTSPI_DAT, 1);
    if (!module_sleep_ms(100))
        return;

    steadyview_set_mode();
}

static void steadyview_close() {
    softspi_close();
}

static void rtc6705WriteRegister(uint32_t data) {
    softspi_set_pin(SOFTSPI_CS, 0);
    for (int i = 0; i < RX5808_PACKET_LENGTH; i++) {
        softspi_set_pin(SOFTSPI_DAT, data & 0x01);
        softspi_set_pin(SOFTSPI_CLK, 1);
        softspi_set_pin(SOFTSPI_CLK, 0);
        data >>= 1;
    }
    softspi_set_pin(SOFTSPI_CS, 1);
    softspi_set_pin(SOFTSPI_DAT, 1);
    usleep(10u);
}

static uint32_t rtc6705ReadRegister(uint8_t read) {
    uint32_t buf = read | (RX5808_READ_CTRL_BIT << 4);
    uint32_t registerData = 0;

    softspi_set_pin(SOFTSPI_CS, 0);
    usleep(periodMicroSec);

    // Write register address and read bit
    for (uint8_t i = 0; i < RX5808_ADDRESS_R_W_LENGTH; ++i) {
        softspi_set_pin(SOFTSPI_DAT, buf & 0x01);
        softspi_set_pin(SOFTSPI_CLK, 1);
        softspi_set_pin(SOFTSPI_CLK, 0);
        buf >>= 1;
    }

    // Change pin from output to input
    softspi_set_direction(SOFTSPI_INPUT);

    // Read data 20 bits
    for (uint8_t i = 0; i < RX5808_DATA_LENGTH; i++) {
        softspi_set_pin(SOFTSPI_CLK, 1);
        if (softspi_get_pin(SOFTSPI_DAT)) {
            registerData = registerData | (1 << (5 + i));
        }
        softspi_set_pin(SOFTSPI_CLK, 0);
    }

    // CS up first: the RTC6705 stops driving DATA; only then the goggle drives it again,
    // straight to its idle high (no low step, no moment with both driving it)
    softspi_set_pin(SOFTSPI_CS, 1);
    softspi_drive_pin(SOFTSPI_DAT, 1);

    return registerData;
}

module_def_t steadyview_module = {
    .num_channels = 48,
    .name = module_standard_channel_name,
    .init = steadyview_init,
    .close = steadyview_close,
    .set_channel = steadyview_set_channel,
    .set_mode = steadyview_set_mode,
    .mode_names = "Diversity\nMix 1\nMix 2\nMix 3",
    .boot_resend_ms = 0,
};