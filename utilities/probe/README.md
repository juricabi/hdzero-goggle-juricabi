# HDMI audio / pin probe kit (Goggle 1, Goggle 2, BoxPro)

Background and the meaning of the results: docs/hdmi-audio.md (on `all`; on `ripples-all` it
comes with HDMI audio). The scripts do not depend on the firmware: the same kit as on `all`.
For the module bay: part 3 below and Module Bay > Check Pins ([docs/module-bay.md](../../docs/module-bay.md)).

## Two ways to run a script

* **SD-card mode.** Copy the script to the SD card root and name it `develop.sh`. The
  firmware runs `/mnt/extsd/develop.sh` **instead of** the goggle app
  (`mkapp/app/script/rc.sh`), so the script starts the goggle app itself, exactly once, on
  every path: normal end, refusal, error, Ctrl-C/kill. A separate guardian process starts
  it if the script hangs (after at most 2 min for the probe's part 1) or is killed with
  SIGKILL. **Delete or rename `develop.sh` afterwards.**
* **SSH mode.** `sh /mnt/extsd/<script>.sh` by hand. The script never starts the goggle app
  (and `bay_gpio_pattern.sh` refuses while the app runs).

Every script pauses the goggle app (SIGSTOP) only for short windows around chip or SoC
register access (well under 1 s each) and resumes it on every exit path; register values are
read with the app paused, three times (they must agree and look plausible) before anything
is written from them, and when in doubt nothing is written. Each script puts back exactly
what it changed. A power cycle undoes everything as well (rc.sh sets the pins at boot);
nothing is written to flash.

| file | what it does | what it touches (run time only) |
|---|---|---|
| `hdmi_audio_probe.sh` | **run this first.** The verdict for HDMI Out Audio on this board, and on GO the file the firmware requires for it (`hdmi_audio_go.txt` on the SD card; NO-GO removes it; docs/hdmi-audio.md, section 7). Sound cards, mixer controls, device tree, pin mux, GPIOs, I2S/PIO/CCU registers, PH_CFG0 read two ways (the `sunxi_dump` latch and `/dev/mem`) and the HDMI audio boot record `/tmp/hdmi_audio.boot`, IT66121 + IT66021 register dumps, dmesg, tool list | pauses the app for each register window; the bank register 0x0F of the two ITE chips (read with the app stopped, put back before it continues); the `sunxi_dump` address latch; one word of `/dev/mem` (PH_CFG0), read only, with `devmem` of the services package's busybox (`/mnt/app/services/busybox/busybox`, if present; never given a value); GPIO 131 (beeper) export; mounts debugfs (RAM) if needed and unmounts it; SD-card mode: starts the app. Never reads FIFO/data registers (I2S RX/TX FIFO, IT66121 0x17 DDC FIFO): a read there would take data out of a stream |
| `hdmi_audio_test.sh` + `tone1k_48k_s16le_2ch.raw` | optional active test: 1 kHz tone to HDMI out, 10 s recording from HDMI in | IT66121 audio registers (only if Reg04 shows no video/HDCP/RCLK reset: 0x14 as the stock init leaves it, or 0x00 with HDMI Out Audio in effect; Reg04 itself only as the constants 0x04 and 0x00); PH1-PH3 pin function: `PH_CFG0` only if it reads exactly the stock 0x35517751, and only as the constant 0x35513331 (left as found if HDMI Out Audio already switched it this boot; any other value: refused); I2S0 `CTL` mode/clock fields, IT66021 bank register; all put back |
| `bay_gpio_pattern.sh` | optional: drives GPIO 192/193/194 (PG0-PG2) high one after another, to find where they arrive and read their voltage | drives one pin at a time (the other two stay inputs), **bay empty**, SD-card mode; puts their data bit and function back |

## 1. Read-only probe (please always send this)

1. Copy `hdmi_audio_probe.sh` to the SD card root and rename it `develop.sh`.
2. Optional: connect an HDMI display to HDMI out.
3. Boot. The screen stays dark during part 1 (up to about a minute), then **2 beeps** and the
   goggle app starts.
4. Leave the goggle on for **2 more minutes** (part 2 runs 60 s after the beeps). For a run
   that also covers HDMI in: connect a source and select HDMI in within that minute.
5. Power off, send `hdmi_audio_probe_N.txt`, `hdmi_audio_dmesg_N.txt` (and
   `hdmi_audio_config_N.gz` if it exists) from the SD card. State the goggle model.

SSH alternative: `sh /mnt/extsd/hdmi_audio_probe.sh` (one pass, about a minute).

## 2. Optional: HDMI audio test

Needs a TV/monitor **with speakers** on HDMI out, and for part B an HDMI source that plays sound.

1. Copy `hdmi_audio_test.sh` as `develop.sh` **and** `tone1k_48k_s16le_2ch.raw` to the SD root.
2. Boot (the script starts the goggle app first), stay on the HDZero/menu screen (not HDMI
   in). 45 s after boot: **1 beep**, a 1 kHz tone should play on the TV for 15 s. **Note
   whether you hear it.** 2 beeps = done.
3. You now have 60 s: select **HDMI in** with a source playing sound. **3 beeps**: 10 s
   recording. **4 beeps** = finished.
4. Send `hdmi_audio_test_N.txt` and `hdmi_in_test_N.wav`, plus "tone heard: yes/no".

SSH: `sh hdmi_audio_test.sh out` or `sh hdmi_audio_test.sh in` (asks for Enter between steps).

## 3. Optional: module-bay pin finder (bay EMPTY, SD-card mode)

What it answers: (a) where GPIO 192 / 193 / 194 (PG0 / PG1 / PG2) arrive, (b) their high
level: about **3.3 V** or about **1.8 V**. The SoC itself reports port G's supply
(PIO_POW_VAL), and Module Bay > **Check Pins** shows it; this script is for when Check Pins
shows "? V", or to confirm which connector pin is PG0, PG1 and PG2 before soldering.

Where: per pkendall64's wiki ("Analog Module Backpack Control", Hardware mods; original
HDZero Goggle) the three signals are on the connector between the module-bay board and the
goggle, i.e. the **goggle-side connector of the bay board**. On a stock goggle they do
**not** reach the 9-pin analog module connector; they appear there only after the wiki's
3-wire mod. Goggle 2: expected to be the same, not documented.

**How to measure: only as in [docs/module-bay-mod-safety.md](../../docs/module-bay-mod-safety.md),
checklist step 4**: one hook clip on one connector pin per boot, attached with the goggle
**off** and checked with the ohmmeter first (above 10 Ω to ground, to the supply and to each
neighbouring connector pin); never move a probe on the connector while the goggle is on.
Only the pin under test is ever an output; the other two stay inputs, so a clip that touches
a neighbour by mistake does not join two driven outputs.

1. Remove any module from the bay. Copy `bay_gpio_pattern.sh` as `develop.sh`. Goggle off:
   attach the clip (step 4). Boot.
2. Pattern (3 rounds): 10 s nothing driven (all inputs); then 1 beep -> GPIO 192 (PG0) high
   for 10 s; 2 beeps -> GPIO 193 (PG1); 3 beeps -> GPIO 194 (PG2). Note after how many beeps
   your pin goes high, and its voltage. Power off before you move the clip to the next pin.
3. 4 long beeps: finished, pins put back, app started. **One long beep** right after boot
   means **refused**: nothing was driven, the app starts normally, the reason is in
   `bay_gpio_N.txt`.

It refuses unless the goggle app is not running, the Wi-Fi driver (xradio) is not loaded and
no SDIO card is on mmc1, PG0-PG2 are in the state rc.sh leaves them (function "IO disable"
in `PG_CFG0`, read three times alike) and GPIO 192-194 are not requested. The device tree
gives PG0-PG5 to the SDIO host (sdc1, Wi-Fi), so `pinmux-pins` names the sdmmc as their
owner on every goggle; that is logged, not a reason to refuse. Afterwards each pin gets its
original data bit and function back (`PG_CFG0` PG0-PG2 fields only, verified) and is
released. Send `bay_gpio_N.txt` and the contact/voltage notes.

## Tests on a PC

* `sh utilities/probe/test/run_tests.sh`: shellcheck; the shared safety block identical in
  all three scripts; then every script under dash and busybox ash against a fake goggle
  (`test/stub/`: a model of the shared `sunxi_dump` latch, the ITE chips, the GPIO driver
  and a real fake app process). It checks the app's process state (never left stopped,
  never started twice, not stopped outside register windows), that every register is back,
  also when a script is killed at many points (TERM/INT/HUP; SIGKILL: the guardian), that an
  interleaved or implausible latch read leads to no write, that PH_CFG0 is written only from
  the stock value and only as one of the two constants, that an IT66121 Reg04 with a video
  reset bit set leads to no IT66121 write, that `devmem` is only ever called to read, that
  the pin finder never has two of PG0-PG2 outputs at a time (sampled at every `sleep`), that
  no FIFO register is read, and that only commands of the goggle's busybox are used.
  `ROOTFS=<extracted rootfs>` checks the commands against a real image and, with
  `qemu-arm`, runs everything under the goggle's own busybox 1.27 ash as well (it has no
  `command` builtin, and a failed `exec` redirection ends it). `QUICK=1` for fewer kill
  points. busybox ash prefers its own applets to `PATH`: in the busybox passes the stubs
  that share an applet's name are shell functions, and before a shell is used every stub
  must resolve to ours in it (else FAIL, and no script runs under that shell).
  **Not as root**: it refuses (a stub that were bypassed would reach the host's real
  `i2cset`, `mount` and `kill`); from a root shell use `utilities/dev/run_probe.sh run`,
  which runs it as an unused uid in its own pid namespace.
* `sh utilities/probe/test/mutants.sh`: 18 mutants with one safety property broken each
  (e.g. the app never resumed, no trap, a single latch read, PH_CFG0 written from any
  value, Reg04 unchecked, devmem given a value, the pin finder driving two pins); the tests
  must fail on every one. `MUT_OLD=<dir>` also runs an older kit (the review's mutant). Not
  as root either (`utilities/dev/run_probe.sh mutants`).
* The C tests of the HDMI audio code (`it66121_seq_test.sh`, `it66021_edid_test.sh`,
  `hdmi_boot_test.sh`, `pcmbridge_test.sh`, `c_mutants.sh` on `all`) come to `ripples-all`
  with that code. The module bay's register read and status mutants are in
  `utilities/module_bay_test/mutants.sh` here.
