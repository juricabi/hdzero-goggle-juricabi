CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror

.PHONY: all run clean

all: test_dvr_auto

test_dvr_auto: test_dvr_auto.c ../../src/core/dvr_auto.c ../../src/core/dvr_auto.h
	$(CC) $(CFLAGS) -I../../src test_dvr_auto.c ../../src/core/dvr_auto.c -o $@

run: test_dvr_auto
	./test_dvr_auto

clean:
	rm -f test_dvr_auto
