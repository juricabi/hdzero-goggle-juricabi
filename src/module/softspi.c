#include "common.hh"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/filesystem.h"

#include "softspi.h"

// sysfs GPIO (utilities/module_bay_test/test_softspi.c points it at a directory of its own)
#ifndef SOFTSPI_GPIO_SYSFS
#define SOFTSPI_GPIO_SYSFS "/sys/class/gpio"
#endif

static const int pins_gpio[SOFTSPI_PINS] = {GPIO_MODULE_CLK, GPIO_MODULE_DAT, GPIO_MODULE_CS};
static int pins_fd[SOFTSPI_PINS] = {-1, -1, -1};
static int dir_fd[SOFTSPI_PINS] = {-1, -1, -1};

static int open_attr(int gpio, const char *attr, int flags) {
    char buf[128];
    snprintf(buf, sizeof(buf), SOFTSPI_GPIO_SYSFS "/gpio%d/%s", gpio, attr);
    return open(buf, flags | O_CLOEXEC);
}

static void write_attr(int fd, const char *text) {
    if (fd < 0)
        return;
    lseek(fd, 0, SEEK_SET);
    if (write(fd, text, strlen(text)) < 0) {
        // nothing to do about it here; the pin keeps its last state
    }
}

bool softspi_open() {
#ifdef EMULATOR_BUILD
    return false;
#else
    bool ok = true;
    for (int i = 0; i < SOFTSPI_PINS; i++) {
        if (pins_fd[i] < 0) {
            // (fails with EBUSY if still exported from an earlier run: that is fine)
            fs_printf(SOFTSPI_GPIO_SYSFS "/export", "%d", pins_gpio[i]);
            pins_fd[i] = open_attr(pins_gpio[i], "value", O_RDWR);
            dir_fd[i] = open_attr(pins_gpio[i], "direction", O_RDWR);
        }
        ok = ok && pins_fd[i] >= 0 && dir_fd[i] >= 0;
    }
    return ok;
#endif
}

void softspi_init() {
    softspi_open();
    softspi_drive_all(0);
}

void softspi_drive_all(int level) {
    for (int i = 0; i < SOFTSPI_PINS; i++)
        write_attr(dir_fd[i], level ? "high" : "low");
}

void softspi_drive_pin(softspi_pin_t pin, int level) {
    write_attr(dir_fd[pin], level ? "high" : "low");
}

void softspi_float_all() {
    for (int i = 0; i < SOFTSPI_PINS; i++)
        write_attr(dir_fd[i], "in");
}

void softspi_release_gpio(int port_num) {
#ifndef EMULATOR_BUILD
    char buf[128];
    // input first: a pin left driving high would feed an unpowered module through
    // its input protection diodes while the bay power is off
    fs_printf(SOFTSPI_GPIO_SYSFS "/export", "%d", port_num);
    snprintf(buf, sizeof(buf), SOFTSPI_GPIO_SYSFS "/gpio%d/direction", port_num);
    fs_printf(buf, "in");
    fs_printf(SOFTSPI_GPIO_SYSFS "/unexport", "%d", port_num);
#else
    (void)port_num;
#endif
}

void softspi_close() {
    for (int i = 0; i < SOFTSPI_PINS; i++) {
        if (pins_fd[i] < 0 && dir_fd[i] < 0)
            continue;
        write_attr(dir_fd[i], "in");
        if (dir_fd[i] >= 0)
            close(dir_fd[i]);
        if (pins_fd[i] >= 0)
            close(pins_fd[i]);
        dir_fd[i] = -1;
        pins_fd[i] = -1;
        fs_printf(SOFTSPI_GPIO_SYSFS "/unexport", "%d", pins_gpio[i]);
    }
}

void softspi_set_pin(softspi_pin_t pin, int value) {
    if (pins_fd[pin] < 0)
        return;
    if (write(pins_fd[pin], value ? "1" : "0", 1) < 0) {
        // see write_attr()
    }
}

int softspi_get_pin(softspi_pin_t pin) {
    uint8_t buf[1] = {'0'};
    if (pins_fd[pin] < 0)
        return 0;
    lseek(pins_fd[pin], 0, SEEK_SET);
    if (read(pins_fd[pin], buf, 1) != 1)
        return 0;
    return buf[0] == '1';
}

void softspi_set_direction(softspi_direction_t direction) {
    // "out" drives low first, as before (the RTC6705 read turns DAT around)
    write_attr(dir_fd[SOFTSPI_DAT], direction == SOFTSPI_OUTPUT ? "out" : "in");
}
