// Host-side test of the [module] keys in settings_load() (src/core/settings.c and
// minIni, built as for the Goggle 1 emulator: setting.ini in the current
// directory).  A stored Module outside 0..4 must load as None, whatever its text.
//
//   make -C utilities/module_bay_test test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log/log.h>

#include "core/settings.h"
#include "lang/language.h"
#include "util/filesystem.h"
#include "util/system.h"

// ---- stand-ins for the rest of the firmware ----

bool fs_file_exists(const char *filename) {
    (void)filename;
    return false;
}
bool language_config() { return false; }
bool log_file_open(const char *filename) {
    (void)filename;
    return false;
}
int system_exec(const char *command) {
    (void)command;
    return 0;
}
long fs_filesize(const char *filename) {
    (void)filename;
    return 0;
}
// (settings.c logs through lib/log, not built here)
int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) {
    (void)file, (void)func, (void)line, (void)level, (void)fmt;
    return 0;
}

// ---- test ----

static int failures, checks;

static void check(int ok, const char *what) {
    printf("  %-82s %s\n", what, ok ? "ok" : "FAIL");
    failures += !ok;
    checks++;
}

// setting.ini with a [module] section of these lines ("" none), then settings_load()
static void load(const char *module_lines) {
    FILE *f = fopen("setting.ini", "w");
    if (f) {
        fprintf(f, "[source]\nanalog_channel=12\n");
        if (*module_lines)
            fprintf(f, "[module]\n%s", module_lines);
        fclose(f);
    }
    settings_load();
}

int main(void) {
    char dir[] = "/tmp/module_settings_test.XXXXXX";
    if (!mkdtemp(dir) || chdir(dir) != 0) {
        printf("no temporary directory\n");
        return 1;
    }
    printf("[module] type in setting.ini:\n");
    load("");
    check(g_setting.module.type == 0 && g_setting.module.setting == 0, "no [module] section: None (the default), mode 0");
    static const struct {
        const char *text;
        int type;
    } cases[] = {
        {"0", 0}, {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4},
        {"5", 0},           // the removed True-D type of earlier test builds
        {"-1", 0}, {"99", 0},
        {"4294967298", 0},  // 2^32 + 2: would be 2 if narrowed to a 32-bit int first
        {"-4294967294", 0}, // -2^32 + 2: likewise
        {"abc", 0},         // no number: 0
        {"0x3", 3},         // minIni reads hex: SteadyView
        {"0x10", 0},
        {"", 0},
    };
    char lines[64], what[96];
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(lines, sizeof(lines), "type=%s\n", cases[i].text);
        load(lines);
        snprintf(what, sizeof(what), "type=%s loads as %d%s", cases[i].text, cases[i].type, cases[i].type ? "" : " (None)");
        check(g_setting.module.type == cases[i].type, what);
    }
    load("type=2\nsetting=1\nchannel=37\n");
    check(g_setting.module.type == 2 && g_setting.module.setting == 1 && g_setting.module.channel == 37,
          "type=2 setting=1 channel=37: rapidFIRE, mode 1, R5");
    load("channel=49\n");
    check(g_setting.module.channel == 12, "channel=49 (out of range): the built-in receiver's (12)");
    unlink("setting.ini");
    if (chdir("/") == 0)
        rmdir(dir);
    printf("%d checks, %s\n", checks, failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
