#include <stdlib.h>
#include <unistd.h>

#include "core/common.hh"
#include "core/settings.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "softspi.h"
#include "steadyview_x.h"

static const steadyview_x_mix_mode_t modes[4] = {
    STEADYVIEW_X_DIVERSITY,
    STEADYVIEW_X_MIX_1,
    STEADYVIEW_X_MIX_2,
    STEADYVIEW_X_MIX_3
};

static int uart_fd = -1;

static void send_command(uint8_t *bytes, uint8_t length) {
    uint8_t crc = 0;
    if (uart_fd < 0)
        return;
    for (int i=1 ; i<length-2; i++) {
        crc ^= bytes[i];
    }
    bytes[length-2] = crc;

    // we try a few times becasue the software uart is not super accurate at 115200 baud!
    // logic analyser show that 2nd or 3rd packet after delay is correct, but let it be 5 for stable result
    for (int i=0; i<5 ; i++) {
        uart_write(uart_fd, bytes, length);
        if (!module_sleep_ms(1))
            break; // pins to be released
    }
}

static void steadyview_x_set_channel(int index) {
    uint8_t data[] = {0x02, 0x06, 0x31, index, 0x01, 0x03};
    send_command(data, sizeof(data));
}

void steadyview_x_set_mixmode(steadyview_x_mix_mode_t mode) {
    if ((unsigned)mode >= ARRAY_SIZE(modes))
        mode = 0;
    uint8_t data[] = {0x02, 0x06, 0x34, modes[mode], 0x01, 0x03};
    send_command(data, sizeof(data));
}

static void steadyview_x_set_mode() {
    if (uart_fd >= 0) {
        steadyview_x_set_mixmode(g_setting.module.setting);
    }
}

// soft_uart.ko (built with gpio_tx = 192 = CLK/PG0 and gpio_rx = 193 = DAT/PG1 as
// its defaults) gives /dev/ttySOFT0; CS (PG2) is held low.  Init sends the mix
// mode; the worker sends the channel next.
static void steadyview_x_init() {
#ifndef EMULATOR_BUILD
    gpio_open(GPIO_MODULE_CS);
    gpio_set(GPIO_MODULE_CS, 0);

    system("insmod /mnt/app/ko/soft_uart.ko");

    if (!module_sleep_ms(100))
        return; // pins to be released (close() unloads soft_uart)
    uart_fd = uart_open(4);
#endif
    steadyview_x_set_mode();
}

static void steadyview_x_close() {
#ifndef EMULATOR_BUILD
    if (uart_fd >= 0)
        uart_close(uart_fd);
    uart_fd = -1;
    usleep(100000);
    system("rmmod soft_uart");
    // soft_uart leaves its TX pin an output (idle high) and frees it: inputs, so
    // nothing drives the unpowered module
    softspi_release_gpio(GPIO_MODULE_CLK);
    softspi_release_gpio(GPIO_MODULE_DAT);
    softspi_release_gpio(GPIO_MODULE_CS);
#endif
}

module_def_t steadyview_x_module = {
    .num_channels = 48,
    .name = module_standard_channel_name,
    .init = steadyview_x_init,
    .close = steadyview_x_close,
    .set_channel = steadyview_x_set_channel,
    .set_mode = steadyview_x_set_mode,
    .mode_names = "Diversity\nMix 1\nMix 2\nMix 3",
    .boot_resend_ms = 0,
};