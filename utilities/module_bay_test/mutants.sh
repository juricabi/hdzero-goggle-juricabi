#!/bin/bash
# Mutation check of the module bay tests: every mutant of src/module must make `make test` fail.
#   utilities/module_bay_test/mutants.sh
# Each mutant is one edit (a perl substitution) of a copy of src/module; the tests run against
# the copy (make SRC=...). Exit status 1 if a mutant survives or does not apply.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$HERE/../.." && pwd)
O=$(mktemp -d /tmp/module_bay_mutants.XXXXXX)
trap 'rm -rf "$O"' EXIT
n=0
bad=0
run() { # name file perl-expression
    n=$((n + 1))
    local d=$O/m$n
    mkdir -p "$d/src"
    cp -r "$W/src/module" "$d/src/"
    perl -0pi -e "$3" "$d/src/module/$2"
    if cmp -s "$d/src/module/$2" "$W/src/module/$2"; then
        echo "NOT APPLIED $1"
        bad=1
        return
    fi
    if make -C "$HERE" test OUT="$d/out" SRC="$d/src/module" > "$d/log" 2>&1; then
        echo "SURVIVED $1"
        bad=1
    else
        # the first failing check or compiler error (not the cc line, whose -Werror matched "error")
        echo "killed   $1: $(grep -m1 -E 'FAIL$|error:|Error [0-9]' "$d/log" | sed 's/  */ /g')"
    fi
}
# the final review's own mutants
run "boot wait 1.9 s" module.h 's/#define MODULE_BOOT_MS 2000/#define MODULE_BOOT_MS 1900/'
run "rapidFIRE edges 60 us apart" rapidfire.c 's/#define RF_PHASE_US 100 /#define RF_PHASE_US 60 /'
run "SDIO check ignores PG2" module.c 's/for \(int pin = 0; pin < 3; pin\+\+\) \{\n        if \(\(\(cfg/for (int pin = 0; pin < 2; pin++) {\n        if (((cfg/'
run "power-on time not rounded up" module.c 's|\(ts.tv_nsec \+ 999999\) / 1000000;|ts.tv_nsec / 1000000;|'
# review defects 4, 5, 6, 10
run "defect 4: pins read without setting them to inputs" module.c 's/        softspi_float_all\(\); \/\/ inputs before the first read\n//'
run "defect 5: WiFi allowed although the pins are held" module.c 's/            released = false;\n//'
run "defect 6: unreadable PG_CFG0 counts as free" module.c 's/\*why = "PG_CFG0 not readable \(sunxi_dump missing, busy or answers disagree\)";\n        return true;/return false;/'
run "defect 6: an echoed address taken as the value" module.c 's/ && v != addr;/;/'
run "defect 10: DATA driven again while CS is low" steadyview.c 's/    softspi_set_pin\(SOFTSPI_CS, 1\);\n    softspi_drive_pin\(SOFTSPI_DAT, 1\);/    softspi_set_direction(SOFTSPI_OUTPUT);\n    softspi_set_pin(SOFTSPI_CS, 1);\n    softspi_set_pin(SOFTSPI_DAT, 1);/'
# softspi.c (test_softspi)
run "softspi: float_all writes nothing" softspi.c 's/        write_attr\(dir_fd\[i\], "in"\);\n\}/        (void)0;\n}/'
run "softspi: close does not unexport" softspi.c 's/        fs_printf\(SOFTSPI_GPIO_SYSFS "\/unexport", "%d", pins_gpio\[i\]\);/        (void)0;/'
# module.c's register read and status (on all, in utilities/probe/test/c_mutants.sh)
run "read_reg ignores the lock" module.c 's/const bool ok = locked && read_reg_once/const bool ok = read_reg_once/'
run "read_reg two of three" module.c 's/ && a == b && b == c;/ \&\& a == b;/'
run "unreadable pin function shown as WiFi" module.c 's/status = pins_unknown \? MODULE_STATUS_PINS_UNKNOWN : MODULE_STATUS_SDIO;/status = MODULE_STATUS_SDIO;/'
# ripples: every bay power command is reported; Dual uses the built-in receiver
run "a repeated power report sets the module up again" module.c 's/    if \(bay_powered == \(on != 0\)\) \{\n        pthread_mutex_unlock\(&lock\);\n        return; \/\/ \(the bay was in that state already\)\n    \}\n//'
run "Dual counted as the bay being the source" module.c 's/ &&\n           !g_setting.source.auto_protocol_detect;/;/'
echo "$n mutants"
exit $bad
