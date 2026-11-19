#!/usr/bin/env python3
"""The answers to the backpack's MSP_GET_BAND_CHAN / MSP_GET_FREQ (src/core/elrs.c), compiled on their own
with stand-ins for the goggle's state: ripples 1.2.2's answers (the stored analog channel), the module
bay's own channel only while the goggle tunes a bay module.

    utilities/module_bay_test/test_msp_answers.py [elrs.c]
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ELRS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "../../src/core/elrs.c")


def function(source, signature):
    start = source.index(signature)
    depth, end = 1, source.index("{", start) + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


src = open(ELRS, encoding="utf-8").read()
table = re.search(r"static const uint16_t freq_table\[ANALOG_CHANNEL_NUM\] = \{.*?\};", src, re.S).group(0)
code = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define ANALOG_CHANNEL_NUM 48
enum { SOURCE_HDZERO, SOURCE_HDMI_IN, SOURCE_AV_IN, SOURCE_AV_MODULE };
struct { int source; } g_source_info;
struct { struct { int hdzero_band; uint8_t analog_channel; } source; struct { int channel; } scan; } g_setting;
static bool tunable;
static int bay_channel;
__attribute__((unused)) static bool analog_channel_tunable(void) { return tunable; }
__attribute__((unused)) static int analog_channel_get(void) { return bay_channel; }
__attribute__((unused)) static uint8_t hdz_ch2index(int band, int channel) { return (uint8_t)(band * 10 + channel - 1); }
""" + table + "\n" + "\n\n".join(function(src, s) for s in ("static uint8_t msp_analog_channel(void)",
                                                          "static uint8_t msp_band_chan_answer(void)",
                                                          "static uint16_t msp_freq_answer(void)")) + r"""
int main(void) {
    int fails = 0;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s\n", name); fails++; } else printf("ok   %s\n", name); } while (0)
    g_setting.source.analog_channel = 5; // stored: 5 (index 4)
    bay_channel = 33;                    // the bay module's own channel

    g_source_info.source = SOURCE_AV_MODULE;
    tunable = false; // Goggle 1 Module None, Goggle 2 Expansion: ripples 1.2.2's answer, the stored channel
    CHECK("bay module not controlled: BAND_CHAN = the stored channel's index", msp_band_chan_answer() == 4);
    CHECK("bay module not controlled: FREQ = the stored channel's frequency", msp_freq_answer() == freq_table[4]);
    tunable = true; // built-in receiver, or a bay module under module control: the channel received
    CHECK("tuned: BAND_CHAN = the channel received", msp_band_chan_answer() == 32);
    CHECK("tuned: FREQ = the channel received", msp_freq_answer() == freq_table[32]);

    g_source_info.source = SOURCE_HDZERO; // HDZero: its band and channel, as ripples
    g_setting.source.hdzero_band = 0;
    g_setting.scan.channel = 3;
    CHECK("HDZero: BAND_CHAN", msp_band_chan_answer() == 2);
    CHECK("HDZero: FREQ", msp_freq_answer() == freq_table[2]);

    g_source_info.source = SOURCE_HDMI_IN; // no channel: 0, as ripples
    CHECK("HDMI in: BAND_CHAN 0", msp_band_chan_answer() == 0);
    CHECK("HDMI in: FREQ 0", msp_freq_answer() == 0);
    printf("%d checks, %s\n", 8, fails ? "FAILED" : "ALL PASSED");
    return fails != 0;
}
"""
with tempfile.TemporaryDirectory(prefix="msp-answers-") as d:
    c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
    open(c, "w").write(code)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-o", exe, c], check=True)
    sys.exit(subprocess.run([exe]).returncode)
