#ifndef __MODULE_SOFTSPI_H_
#define __MODULE_SOFTSPI_H_

#include <stdbool.h>
#include <sys/types.h>

// The three module bay pins GPIO_MODULE_CLK/DAT/CS (SoC PG0/PG1/PG2), named after
// their role in the SPI-style protocols.  Which bay pin each one reaches is not
// verified (docs/module-bay.md).  Only the module worker thread (module.c) calls
// these.
typedef enum {
    SOFTSPI_CLK,
    SOFTSPI_DAT,
    SOFTSPI_CS,
    SOFTSPI_PINS
} softspi_pin_t;

typedef enum {
    SOFTSPI_OUTPUT,
    SOFTSPI_INPUT
} softspi_direction_t;

// Exports the pins and opens their value and direction files; the pins stay
// inputs.  False when they are not available (emulator, no sysfs GPIO).
bool softspi_open();
// softspi_open(), then all three pins driven low (the SPI and switch modules set
// their own levels next).
void softspi_init();
// All three pins outputs at level, set through the direction file ("high"/"low")
// so the pin does not pass through the other level first.
void softspi_drive_all(int level);
// One pin an output at level, set through its direction file like softspi_drive_all().
void softspi_drive_pin(softspi_pin_t pin, int level);
// All three pins inputs again.
void softspi_float_all();
// Releases the pins: inputs, files closed, unexported.
void softspi_close();
void softspi_set_direction(softspi_direction_t direction);
// Makes a bay GPIO an input and unexports it (also for a pin another driver had,
// e.g. soft_uart.ko).
void softspi_release_gpio(int gpio);
void softspi_set_pin(softspi_pin_t pin, int value);
int softspi_get_pin(softspi_pin_t pin);

#endif
