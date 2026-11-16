// Host test of src/module/softspi.c, built as for the goggle (not the emulator), against a
// directory that stands in for /sys/class/gpio (SOFTSPI_GPIO_SYSFS): which files it writes,
// what, and in which order the pins become inputs and outputs. The other module bay tests
// replace softspi.c by models; this one runs the real file.
//
//   make -C utilities/module_bay_test test

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/defines.h"
#include "module/softspi.h"

static int failures, checks;

static void check(int ok, const char *what) {
    printf("  %-82s %s\n", what, ok ? "ok" : "FAIL");
    failures += !ok;
    checks++;
}

static const char *path(const char *name) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s/%s", SOFTSPI_GPIO_SYSFS, name);
    return buf;
}

static const char *attr(int gpio, const char *name) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "%s/gpio%d/%s", SOFTSPI_GPIO_SYSFS, gpio, name);
    return buf;
}

static void put(const char *file, const char *text) {
    FILE *f = fopen(file, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

// a sysfs attribute is rewritten from offset 0 without truncation (as softspi.c does on the
// real one): what was written last is the start of the file
static int starts(const char *file, const char *text) {
    char buf[64] = "";
    FILE *f = fopen(file, "r");
    if (!f)
        return 0;
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strncmp(buf, text, strlen(text)) == 0;
}

static int all_dirs(const char *text) {
    return starts(attr(GPIO_MODULE_CLK, "direction"), text) && starts(attr(GPIO_MODULE_DAT, "direction"), text) &&
           starts(attr(GPIO_MODULE_CS, "direction"), text);
}

static void make_sysfs(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", SOFTSPI_GPIO_SYSFS, SOFTSPI_GPIO_SYSFS);
    if (system(cmd) != 0)
        exit(2);
    put(path("export"), "");
    put(path("unexport"), "");
    const int gpios[] = {GPIO_MODULE_CLK, GPIO_MODULE_DAT, GPIO_MODULE_CS};
    for (int i = 0; i < 3; i++) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s/gpio%d", SOFTSPI_GPIO_SYSFS, gpios[i]);
        mkdir(dir, 0755);
        put(attr(gpios[i], "value"), "0\n");
        put(attr(gpios[i], "direction"), "xx\n"); // function 7 ("IO disabled") after rc.sh: neither in nor out
    }
}

int main(void) {
    printf("softspi.c against a stand-in sysfs (%s):\n", SOFTSPI_GPIO_SYSFS);
    make_sysfs();

    check(softspi_open(), "open: the three pins exported, value and direction files open");
    check(starts(path("export"), "194") && all_dirs("xx"), "  exported (194 last), directions not written yet");
    softspi_float_all();
    check(all_dirs("in"), "float_all: every pin an input (Check Pins reads only after this)");

    put(attr(GPIO_MODULE_CLK, "value"), "1\n");
    put(attr(GPIO_MODULE_DAT, "value"), "0\n");
    put(attr(GPIO_MODULE_CS, "value"), "1\n");
    check(softspi_get_pin(SOFTSPI_CLK) == 1 && softspi_get_pin(SOFTSPI_DAT) == 0 && softspi_get_pin(SOFTSPI_CS) == 1,
          "get_pin reads each value file (1 0 1)");
    put(attr(GPIO_MODULE_CLK, "value"), "0\n");
    check(softspi_get_pin(SOFTSPI_CLK) == 0, "  and reads it again from the start (0)");

    softspi_drive_pin(SOFTSPI_DAT, 1);
    check(starts(attr(GPIO_MODULE_DAT, "direction"), "high") && starts(attr(GPIO_MODULE_CLK, "direction"), "in"),
          "drive_pin(DAT, 1): \"high\" (output and level in one write), the others untouched");
    softspi_drive_all(0);
    check(all_dirs("low"), "drive_all(0): every pin \"low\"");
    softspi_set_pin(SOFTSPI_CS, 1);
    check(starts(attr(GPIO_MODULE_CS, "value"), "1"), "set_pin(CS, 1): value file");
    softspi_set_direction(SOFTSPI_INPUT);
    check(starts(attr(GPIO_MODULE_DAT, "direction"), "in") && starts(attr(GPIO_MODULE_CS, "direction"), "low"),
          "set_direction(INPUT): DAT only");
    softspi_set_direction(SOFTSPI_OUTPUT);
    check(starts(attr(GPIO_MODULE_DAT, "direction"), "out"), "set_direction(OUTPUT): DAT \"out\"");

    softspi_close();
    check(all_dirs("in") && starts(path("unexport"), "194"), "close: every pin an input, then unexported (194 last)");
    put(attr(GPIO_MODULE_CS, "value"), "1\n");
    put(attr(GPIO_MODULE_CS, "direction"), "xx\n");
    softspi_set_pin(SOFTSPI_CS, 0);
    softspi_drive_pin(SOFTSPI_CS, 1);
    check(softspi_get_pin(SOFTSPI_CS) == 0 && starts(attr(GPIO_MODULE_CS, "value"), "1") &&
              starts(attr(GPIO_MODULE_CS, "direction"), "xx"),
          "after close: nothing written, reads 0 (files closed)");

    softspi_release_gpio(GPIO_MODULE_DAT);
    check(starts(path("export"), "193") && starts(attr(GPIO_MODULE_DAT, "direction"), "in") && starts(path("unexport"), "193"),
          "release_gpio(193): exported, input, unexported");

    char dir[256];
    snprintf(dir, sizeof(dir), "rm -rf '%s/gpio%d'", SOFTSPI_GPIO_SYSFS, GPIO_MODULE_DAT);
    if (system(dir) != 0)
        return 2;
    put(attr(GPIO_MODULE_CLK, "direction"), "xx\n");
    check(!softspi_open(), "a pin that cannot be exported: open fails");
    softspi_close();
    check(starts(attr(GPIO_MODULE_CLK, "direction"), "in"), "  close after a failed open: the pins it had are inputs again");

    snprintf(dir, sizeof(dir), "rm -rf '%s'", SOFTSPI_GPIO_SYSFS);
    if (system(dir) != 0)
        return 2;
    printf("%d checks, %s\n", checks, failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
