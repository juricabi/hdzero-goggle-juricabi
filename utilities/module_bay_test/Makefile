# Host-side tests for the module bay code (plain gcc, not part of the firmware build).
#   make -C utilities/module_bay_test test

CC ?= gcc
SRC = ../../src/module
OUT ?= /tmp/module_bay_test

# module.c built as for the Goggle 2 emulator, against recording module types
# (bay_pins_busy() is not compiled out there: its sysfs paths are variables)
ROOT = ../..
WORKER_CFLAGS = -O1 -g -std=gnu99 -DEMULATOR_BUILD -DHDZGOGGLE2=1 -D_GNU_SOURCE \
	-I$(ROOT)/src -I$(ROOT)/src/core -I$(ROOT)/lib/log/include -I$(ROOT)/lib/lvgl -I$(ROOT)/lib/minIni/src
WARN = -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers

all: $(OUT)/test_module_worker $(OUT)/test_module_types $(OUT)/test_settings_module $(OUT)/test_softspi

# the real softspi.c, built as for the goggle, against a stand-in for /sys/class/gpio
$(OUT)/test_softspi: test_softspi.c $(SRC)/softspi.c $(SRC)/softspi.h $(ROOT)/src/util/filesystem.c
	mkdir -p $(OUT)
	$(CC) $(filter-out -DEMULATOR_BUILD,$(WORKER_CFLAGS)) -DSOFTSPI_GPIO_SYSFS='"$(OUT)/sysfs_gpio"' $(WARN) \
		-Wno-unused-result -o $@ test_softspi.c $(SRC)/softspi.c $(ROOT)/src/util/filesystem.c

$(OUT)/test_module_worker: test_module_worker.c $(SRC)/module.c $(SRC)/module.h $(SRC)/module_def.h
	mkdir -p $(OUT)
	$(CC) $(WORKER_CFLAGS) -DSDIO_RECHECK_MS=500 -DMODULE_C='"$(SRC)/module.c"' $(WARN) -o $@ test_module_worker.c -lpthread

# the real Switch, rapidFIRE, SteadyView and SteadyView X types on the real worker,
# recording pins; SteadyView X built as for the goggle (not the emulator), its gpio /
# uart / insmod calls answered by the test (glibc marks system() warn_unused_result, musl does not)
TYPES_SRC = $(SRC)/module.c $(SRC)/switch.c $(SRC)/rapidfire.c $(SRC)/steadyview.c $(SRC)/module_def.c
$(OUT)/steadyview_x.o: $(SRC)/steadyview_x.c $(SRC)/steadyview_x.h $(SRC)/module_def.h
	mkdir -p $(OUT)
	$(CC) $(filter-out -DEMULATOR_BUILD,$(WORKER_CFLAGS)) -Dsystem=test_system $(WARN) -Wno-unused-result -c -o $@ $(SRC)/steadyview_x.c
$(OUT)/test_module_types: test_module_types.c $(TYPES_SRC) $(OUT)/steadyview_x.o $(SRC)/module.h $(SRC)/module_def.h $(SRC)/softspi.h
	mkdir -p $(OUT)
	$(CC) $(WORKER_CFLAGS) $(WARN) -o $@ test_module_types.c $(TYPES_SRC) $(OUT)/steadyview_x.o -lpthread

# settings_load() (src/core/settings.c) with the real minIni, as for the Goggle 1
# emulator (setting.ini in the current directory)
SETTINGS_CFLAGS = -O1 -g -std=gnu99 -DEMULATOR_BUILD -DHDZGOGGLE=1 -D_GNU_SOURCE \
	-I$(ROOT)/src -I$(ROOT)/src/core -I$(ROOT)/lib/log/include -I$(ROOT)/lib/lvgl -I$(ROOT)/lib/minIni/src
# (settings.c and minIni are upstream code: built without -Wall)
$(OUT)/test_settings_module: test_settings_module.c $(ROOT)/src/core/settings.c $(ROOT)/src/core/settings.h
	mkdir -p $(OUT)
	$(CC) $(SETTINGS_CFLAGS) $(WARN) -c -o $(OUT)/test_settings_module.o test_settings_module.c
	$(CC) $(SETTINGS_CFLAGS) -o $@ $(OUT)/test_settings_module.o $(ROOT)/src/core/settings.c $(ROOT)/lib/minIni/src/minIni.c

test: $(OUT)/test_module_worker $(OUT)/test_module_types $(OUT)/test_settings_module $(OUT)/test_softspi
	$(OUT)/test_softspi
	$(OUT)/test_settings_module
	$(OUT)/test_module_worker
	MODULE_TEST_BATCH1=1 $(OUT)/test_module_worker
	$(OUT)/test_module_types

clean:
	rm -f $(OUT)/test_module_worker $(OUT)/test_module_types $(OUT)/steadyview_x.o $(OUT)/test_settings_module $(OUT)/test_settings_module.o $(OUT)/test_softspi

.PHONY: all test clean
