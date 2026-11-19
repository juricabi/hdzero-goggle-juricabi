# Module bay control

The goggle can set the channel (and on some modules the mode) of an analog
receiver module in the expansion module bay: dial, buttons, ELRS backpack. Menu:
**Module Bay** (Goggle 1 and Goggle 2, the entry before Go Sleep!). It follows
pkendall64's analog-module-control. This page describes it on `ripples-all`: ripples'
firmware 1.2.2 (LVTY-AI/hdzero-goggle 4fbb865) with the module bay ported from `all`.

**It needs a hardware mod, for every module type.** On a stock goggle no control
signal reaches the analog module: the goggle only powers it. The mod is three
wires on the bay board, from three pins of the connector that plugs into the
goggle to the channel pins 1-3 of the 9-pin module socket
([pkendall64's wiki](https://github.com/pkendall64/hdzero-goggle/wiki/Analog-Module-Backpack-Control),
for the original **Goggle 1**; on the **Goggle 2** it is unproven). The wires
connect the module to SoC pins that are also the WiFi bus: a 4.7 kΩ resistor in
each wire and the checklist of **[module-bay-mod-safety.md](module-bay-mod-safety.md)**
come first.

**After flashing, nothing changes until you choose a Module.** Module = None is
the default, and with None the goggle never touches the bay pins: exactly as a
stock goggle (host tests, below).

## The page

| Row | |
|---|---|
| Module | **None** (default), Switch (Fat Shark), rapidFIRE, SteadyView, SteadyView X |
| Mode | depends on the Module (below); greyed out when it has none ("Mode needs the bay power switch" for SteadyView on a goggle without one) |
| Check Pins | "Click to read the 3 bay pins (read only)": reads PG0-PG2 as inputs (never drives) and shows the PG bank supply |
| < Back | |

A change applies (and is stored) when its list is closed with a click. Below the
rows: the status line and a three-line note:

* Module control needs the 3-wire bay mod (resistors or level shifter).
* It pauses while WiFi is on: WiFi uses the same pins.
* Furious True-D with MOD 1.17: choose rapidFIRE.

**Status** (refreshed while the page is open or only highlighted in the menu):

| text | when |
|---|---|
| Off, bay pins untouched | Module = None |
| Paused while WiFi is on (orange) | WiFi enabled |
| Paused, pins in use by WiFi (orange) | the WiFi driver is loaded or PG0-PG2 are in their SDIO function (checked again every 5 s) |
| Paused, pin state unknown (orange) | PG0-PG2's function (PG_CFG0) could not be read: sunxi_dump missing, its lock busy for 1 s, or three reads that disagree (left alone, checked again every 5 s) |
| Waiting for bay power | the bay is switched off (batch 2 hardware) |
| Waiting for the module to boot | less than 2.0 s since the bay was powered on |
| Sending (green) and the module's channel, e.g. "R1", "CH5" | the goggle controls the module; on the Goggle 2 "(not video source)" follows while the built-in receiver is the analog source |
| No module bay | BoxPro |

**Check Pins** shows "Reading...", then for example "Pins 1 0 1, 3.3 V": the levels of
PG0, PG1 and PG2 (1, 0, or "toggling" for a line that changed during the 100 ms read)
and the PG bank supply from the SoC's own PIO_POW_VAL register (bit 6): "3.3 V",
"1.8 V" (orange: resistors alone cannot drive a 5 V module, module-bay-mod-safety.md
section 5) or "? V" (not readable). It reads only while the goggle does not control the
bay, WiFi is off and the pins are not in their SDIO function; otherwise "Not read: set
Module to None first", "Not read: switch WiFi off first", "Not read: pins in use by
WiFi", "Not read: pin state unknown" (PG_CFG0 not readable) or "Not read: no GPIO
access". Reading exports the pins as GPIO inputs and unexports them again; they stay
GPIO inputs, high-impedance like the "IO disabled" state `rc.sh` leaves them in. Use it
with Module = None, before choosing a Module.

Every string on the page is in all five language files. The Switch modes are "Wiring A
(standard)" to "Wiring F" (which SoC pin carries which band bit, below).

**setting.ini.** `[module] type`: pkendall64's numbering, 0 None, 1 Switch, 2
rapidFIRE, 3 SteadyView, 4 SteadyView X; any other value (5, the custom True-D
type of earlier test builds, included) loads as None. `[module] setting`: the Mode
entry. `[module] channel`: Goggle 2 only (below). `[module] pins` of earlier test
builds is ignored.

## Goggle 1 and Goggle 2

| | Goggle 1 | Goggle 2 |
|---|---|---|
| analog receivers | bay only | built-in RTC6715 and bay, Source > Analog Module = Built-in / Expansion; Source > Auto Detect (Dual) always uses the built-in one |
| bay module controlled when | always the analog source | also while Built-in is selected (when the bay is powered) |
| channel stored in setting.ini | `[source] analog_channel` (as upstream) | built-in: `[source] analog_channel` (as upstream); bay: `[module] channel` (new, starts from analog_channel) |
| WiFi | on the Expansion Module V2, through the bay connector | built in |
| the mod | pkendall64's wiki | unproven |

**Channels.** Each receiver keeps its own channel, always one of the 48 standard
channels (A1-8, B1-8, E1-8, F1-8, R1-8, L1-8); nothing is folded or overwritten.
With the bay as the analog source, the dial (tune, click / long press), the OSD
channel, the status bar and the Source page use the bay module's channel list. A
type with 8 channels (Switch) shows and sends the stored channel's place in its
band, `(ch - 1) % 8 + 1` (R5 shows as CH5); a choice in its list keeps the stored
band (CH2 from R5 stores R2), and an ELRS VTX channel is stored as it is (F4,
shown CH4). Changing the Module or Source > Analog Module back and forth gives the
same channel back. While the bay module is not controlled (Module None, WiFi on)
the goggle does not tune it, shows no channel for it and stores nothing for it,
also not from ELRS.

**Goggle 2, Source > Analog Module.** Switching to Expansion, or leaving Auto
Detect (Dual) with Expansion selected, sends the bay module its mode and channel
again (also when the bay stays powered, Power > AnalogRX Power = On), in case it was
changed by hand meanwhile. In Dual the built-in receiver is the analog source: the
dial, ELRS and the OSD use its channel, and the bay module keeps its own.

**Bay power** (batch 2 hardware). ripples switches it in `Analog_Module_Power()`
(source changes on the Source page, Power > AnalogRX Power, Scan Now, wake-up) and
off when the goggle goes to sleep; every one of these commands to the DM5680
co-processor goes through `DM5680_ExternalAnalog_Power()` (`src/driver/dm5680.c`),
which tells the module bay (a command that repeats the bay's state changes nothing;
the first one, at the app start, is always sent). While the bay is off the pins are
inputs. **Every Module keeps the pins inputs until 2.0 s after every bay power-on**,
then sets the module up: its mode, then its channel. Batch 1 Goggle 1 hardware has
no bay power switch: the app start counts as the power-on (2.0 s after it). The
2.0 s count from the goggle's power-on command to the DM5680; the switch itself
follows after an unmeasured delay. A True-D's stock bootloader holds bay pin 3 (D13) until
~1.45 s after its power-on (furiousdfw `docs/bay-standards.md` 5.1), the ExpressLRS VRx
backpack waits 2 s too (`VRX_BOOT_DELAY`).

### Differences from ripples' firmware

ripples already hides the channel of the Goggle 2's Expansion module and ignores
ELRS / dial channel changes for it (it cannot tune it), and sends the analog channel
over MSP in analog mode. With module control:

* a bay module under module control (a Module chosen, WiFi off) is tuned like the
  built-in receiver: dial (tune, click / long press), ELRS / MSP, the OSD channel,
  the status bar and the Source page, on the Goggle 1 too (ripples: no analog
  channel on the Goggle 1);
* an uncontrolled bay module (Module None, the default, or WiFi on) stays as in
  ripples: no channel shown, nothing applied, MSP GET_BAND_CHAN / GET_FREQ answer
  255 / 0 Hz (the backpack's "unknown"); also the Goggle 2's status bar shows no
  channel then (ripples showed the built-in receiver's);
* Goggle 2: the bay module's channel is a new key, `[module] channel`;
  `[source] analog_channel` keeps its meaning (the built-in receiver);
* `Analog_Module_Power()` sends its first command at the app start also when the bay
  is already in the wanted state (ripples sent nothing then), so that the module bay
  knows the bay power;
* the app's `aww` / `awr` commands run under the lock of `/sys/class/sunxi_dump`
  (`system_exec()` hands them to `system_exec_sunxi_dump()`), which the module bay's
  register reads take too.

## The pins and WiFi

The goggle drives SoC pins **PG0/PG1/PG2** = GPIO 192/193/194, named CLK/DAT/CS
after their role in the SPI-style protocols (pkendall64's analog-module-control,
written for the Goggle 1). With the standard rapidFIRE wiring (ExpressLRS: CS3 =
CLK, CS2 = DATA, CS1 = NSS) the mod presumably connects PG0 → socket pin 1 (CS3),
PG1 → pin 2 (CS2), PG2 → pin 3 (CS1); pkendall64's photos show the socket end, not
which goggle connector pin is which PG pin. Nobody has checked it on a goggle.

PG0-PG5 are also the SDIO bus (`sdc1`) of the WiFi chip (XR819, `xradio_wlan`) on
**both** goggles. The official images (Goggle 1 9.5.0, Goggle 2 9.5.1 packages)
carry the same sys_config: `[sdc1]` on PG00-PG05, function 2, 4-bit; `[wlan]` on
SDIO bus 1. At boot `rc.sh` switches PG0-PG5 to "IO disabled"
(`0x0300B0D8 = 0x22777777`); the SDIO host takes them back when WiFi starts. So
the goggle:

* drives the pins only with a Module chosen, WiFi disabled (WiFi page), the bay
  powered for 2.0 s, and the pins not in use by WiFi;
* releases them before the WiFi page starts WiFi (and lets a running Check Pins
  finish first), and takes them again after WiFi was stopped;
* leaves them alone while `xradio_wlan` is loaded or PG_CFG0 shows any of PG0-PG2
  in its SDC1 function (checked again every 5 s).

The PG bank supply (1.8 V or 3.3 V) is not settled by the images; it decides
whether resistors alone are enough: module-bay-mod-safety.md.

### Releasing the pins

A release request (bay power off, Module changed, WiFi on) stops a transfer at its
next abort point, then the type's close() makes the pins inputs. The pins stay
driven at most:

| Module | after the request |
|---|---|
| Switch | ~1 ms (its waits are abortable) |
| rapidFIRE | the frame on the wire: ≤ ~15 ms (13.5-14.8 ms per frame in the host test runs; the goggle not measured) |
| SteadyView | one register access plus its fixed waits (≤ ~12 ms); 70 ms during the mode set's power pulse |
| SteadyView X | during set-up: until insmod of soft_uart returns; then close() waits 100 ms and unloads it |

At a bay power-off the bay is already unpowered meanwhile (`dm5680.c` sends the
command first and only then tells the worker). module-bay-mod-safety.md has what
that means for the module.

## Module types

| Module | Channels | On the pins | Mode |
|---|---|---|---|
| Switch (Fat Shark) | 8, CH1-CH8 = channels 1-8 of the module's own band | static 3-bit code, active high, code n = CH n+1 | pin order, below |
| rapidFIRE | 48 (A B E F R L) | ImmersionRC rapidFIRE SPI (CS, DATA, CLK), each command 3x | rapidFIRE 1 / rapidFIRE 2 / Legacy (`D` 0 / 1 / 2) |
| SteadyView | 48 | RTC6705 register writes; mode set through a bay power cycle (batch 2 only) | Diversity / Mix 1-3 |
| SteadyView X | 48 | 115200 baud UART through `soft_uart.ko` (TX = PG0, RX = PG1), CS low | Diversity / Mix 1-3 |

After every set-up (power-on, Module chosen, WiFi off) and every mode change the
goggle sends the channel itself, whatever the type's set-up did. Module and Mode
are applied together, so a new Module is never set up with the old one's Mode.

### Switch

**Pin order.** The Mode list picks which pin carries which bit, because the mod's
wiring is not known. Each label gives the weight of each pin's bit:

| Mode (page label) | weights | bit 0 | bit 1 | bit 2 | note |
|---|---|---|---|---|---|
| Wiring A (standard) | PG0=1 PG1=2 PG2=4 | PG0 | PG1 | PG2 | pkendall64's order; with the presumed mod wiring (PG0 → bay pin 1) bit 0 is on bay pin 1, as in TBS's pinout (pin 1 = CH0) and on the True-D (bay pin 1 = D11 = its bit 0, measured 2026-11-13): **the one for a True-D in `BAY:1-8`** |
| Wiring B | PG0=4 PG1=1 PG2=2 | PG1 | PG2 | PG0 | rapidFIRE convention CH1 = DATA, CH2 = CS, CH3 = CLK |
| Wiring C | PG0=4 PG1=2 PG2=1 | PG2 | PG1 | PG0 | bit 0 on bay pin 3 (the ExpressLRS naming CS1 = NSS = pin 3); recommended here for the True-D until 2026-11-13, from an inference the measurement showed wrong |
| Wiring D | PG0=1 PG1=4 PG2=2 | PG0 | PG2 | PG1 | |
| Wiring E | PG0=2 PG1=1 PG2=4 | PG1 | PG0 | PG2 | |
| Wiring F | PG0=2 PG1=4 PG2=1 | PG2 | PG0 | PG1 | |

Finding the order on the bench: dial CH1, CH2, CH3, CH5 and see which channel the
module takes for each.

**Sequence** (what the goggle produces):

1. pins inputs until the set-up, 2.0 s after bay power-on;
2. code 2 for 200 ms, from inputs straight to its levels: not part of the
   rapidFIRE SPI-mode entry (all high, then all low), so a rapidFIRE in legacy
   mode stays in it;
3. the channel's code, held. Each pin is set on its own (three sysfs writes, a few
   hundred µs between the first and the last);
4. channel 8 (code 7, all high, what an unconnected bay reads) is preceded by code
   6 (channel 7) until 5.0 s after bay power-on and for at least 500 ms: a True-D
   in `BAY:1-8` takes 7 only after another code (MOD 1.16: taken; MOD 1.17: read
   twice). Codes 0-6 go out at once;
5. a release makes the pins inputs: the module reads its own pull-ups.

### rapidFIRE

The goggle only writes: nothing reads a pin or waits for an answer, so a module
that only listens (a True-D with MOD 1.17, `BAY:RAPIDFIRE`) gets exactly what a
rapidFIRE gets. Sequence (`src/module/rapidfire.c`; times after bay power-on from
the host test's trace, which has no sysfs cost):

| time | pins |
|---|---|
| 0-2.0 s | inputs |
| 2.001 s | straight to all three high, CS first (CLK never rises inside an open CS window), 200 ms |
| 2.202 s | all three low, 200 ms (SPI-mode entry, as the ExpressLRS backpack's `EnableSPIMode()`) |
| 2.403 s | CS high; idle from now on: CS high, CLK low, DATA low |
| 2.504 s | mode `D` x3 |
| 2.579 s | band `B` x3, then 100 ms |
| 2.755 s | channel `C` x3 |
| 5.001 s | band `B` x3, 100 ms, channel `C` x3 again (5.177 s), for a module that boots slower; channel and mode changes between 2.4 s and 5.0 s wait for this |
| later | each channel change: `B` x3, 100 ms, `C` x3 (~0.25 s in all); a mode change: 100 ms, `D` x3, then `B`, `C` |

Command: 5 bytes `cmd '=' 0x01 crc value`, crc = (cmd + '=' + 1 + value) & 0xFF;
`D` 0 rapidFIRE #1, 1 rapidFIRE #2, 2 legacy (GOROman/rapidFIRE_SPI's enum; only
0-2 are sent); `B` IMRC band 1 F, 2 R, 3 E, 4 B, 5 A, 6 L; `C` channel 1-8. Each
command 3 times, 10 ms from the end of one frame to the start of the next.

**Frame** (SPI mode 0, MSB first; the module samples DATA at the CLK rise): CS low,
DATA = bit 7; per bit: CLK high, CLK low and DATA = the next bit; CS high, DATA
low. After every pin write the goggle waits until the clock has passed an absolute
deadline 100 µs after that write, so:

* **guaranteed**: every CS / CLK level and gap (CS low to the first CLK rise, each
  CLK high and low, the last CLK fall to CS high) lasts **≥ 100 µs on the pins**,
  and DATA changes only while CLK is low, ≥ 100 µs before the next rise, whatever
  the scheduler does: a sleep can only end late, and one that ends early (a signal,
  an error) is repeated. 81 waits: ≥ 8.1 ms per frame;
* **typical**: each wait overshoots by the timer slack (50 µs for a normal
  thread), the wake-up latency and the sysfs writes: 13.5-14.8 ms per frame on a
  PC (host test); on the goggle not measured, estimated ~12-20 ms.

The requirement this meets: a True-D with MOD 1.17 reads a bay edge ≤ 43.1 µs
after it in its worst case and asks the goggle for ≥ 100 µs per level and gap
(furiousdfw `docs/bay-standards.md` 4); a real rapidFIRE takes up to ~80 kHz
(≥ 6.25 µs per level).

**Against MOD 1.17's receiver** (bay-standards 3.1; the host test runs a model of
it on the goggle's own pin trace): frames are taken without the SPI-mode entry;
`B` and `C` have the right length and checksum; every change is band x3, then
channel x3 0.11 s after the last band frame (MOD 1.17 pairs a channel only with a
band up to 1 s before it, and the senders' gaps are 0.1-0.35 s); `D` is a valid
frame that the True-D ignores; a channel command alone is never sent (MOD 1.17
takes one only after 2 s of silence); the 5.0 s resend of the same channel does
nothing on a True-D that already has it; the lines never float between frames
(idle levels), so no failed windows. **Against the ExpressLRS VRx backpack**
(`src/rapidfire.cpp`, the reference sender): the same 2.0 s boot delay, entry
(200 ms high, 200 ms low), command bytes, band mapping and 3 repeats with band
then 100 ms then channel. The backpack floats its pins between frames (the goggle
keeps idle levels), clocks at 1 kHz with 100 ms CS lead and lag (the goggle: ≥ 100
µs per level, 10 ms between frames) and never sends `D`. pkendall64's original
frames (≥ 8-16 µs per step) worked on real rapidFIRE modules; the goggle is now
slower than that and well inside the 80 kHz limit.

### SteadyView, SteadyView X

SteadyView latches its mix mode at power-up: the mode set switches the bay off for
70 ms and on again with CLK / DATA at the mode's levels. This is the one bay
power-on with driven pins, by the protocol, only with Module = SteadyView (never
select it for another module). Every bay power command goes through
`DM5680_ExternalAnalog_Power()`; the pulse (`DM5680_ExternalAnalog_PowerCycle()`)
runs under the same lock and only while the bay is on. Batch 1 Goggle 1 has no bay power switch: no
mode list. A SteadyView (non-X) needs its pin 8 lifted (RSSI on the module, GND
in the bay; pkendall64's wiki): module-bay-mod-safety.md section 6. SteadyView X
gets its mix mode at set-up, then its channel.

### Furious True-D

Firmware: **MOD 1.17** as released on 2026-11-13 (furiousdfw `release/1.17/`, tag
`trued-mod-1.17`; public: juricabi/true-d-v3-mod-firmware v1.17).  Its menu setting
**BAY** is `RAPIDFIRE` (default: a standard rapidFIRE SPI-mode listener), `1-8` (the
absolute 3-pin code), `STEP` (Fat Shark up / down) or `ORQA` (the Orqa FPV.One's faster
rapidFIRE, with answers: **not for the HDZero**).  In `RAPIDFIRE`, `1-8` and `STEP` its
application never drives a bay pin; `ORQA` pulls DATA low while it answers the Orqa's
query.  **BAY CLK** stays `D11` (the bay connector's order: pin 1 = D11 = rapidFIRE CLK,
pin 3 = D13 = CS, measured on the unit).  A band or channel counts when two of the
goggle's three copies agree, and this page's 2.0 s / ≥ 100 µs timing fits it.  Tested on
the unit in an Orqa FPV.One (`ORQA`, `1-8`, `STEP`); `RAPIDFIRE` from this goggle: not
yet on hardware (simulated with a model of this sender).

| True-D BAY | goggle Module | note |
|---|---|---|
| **RAPIDFIRE** (default) | **rapidFIRE** | the main way: 48 channels A B E F R L; no pull-ups on the True-D side |
| 1-8 | Switch, **Wiring A (standard)** | bay pin 1 = D11 = bit 0 (measured) if the mod's wire from PG0 goes to bay pin 1; channels 1-8 of the True-D's current band |

The custom "True-D" type of earlier test builds is gone (a stored `[module] type =
5` loads as None): MOD 1.17 has no LINK receiver any more.

**The bootloader.** The True-D's **stock bootloader** drives bay pin 3 (D13: the CS
wire, PG2 with the presumed mod wiring) high at 5 V for ~100 ms and then low until
~1.39 s after every reset or power-on, whatever the firmware; the goggle's 2.0 s
input hold keeps clear of it and the 4.7 kΩ resistors make it harmless. A **no-LED
bootloader** (furiousdfw `bootloader/`; public: true-d-v3-mod-firmware
`docs/bootloader.md`; written once with an ISP programmer) never drives a bay pin;
the owner's True-D has it since 2026-11-13. Both cases are covered in
module-bay-mod-safety.md (checklist step 12 tells which one a module has).

**Never** use a True-D with the stock firmware 3.9D, MOD 1.15 or MOD 1.16 in
`BAY:AUTO` in a wired bay: they drive D13 at 5 V push-pull for ~6 s after every
power-up. **Never** flash a True-D while it sits in a wired bay.

**Order:** update the True-D and set its BAY mode outside the goggle (or with
Module = None); the checklist of module-bay-mod-safety.md; then Module =
rapidFIRE; keep WiFi off while flying analog.

### Foxeer Wildfire

What is known (sources below): the Wildfire (72CH, and the newer Wildfire /
Wildfire Lite 80CH) is a diversity receiver for Fat Shark style bays with its own
OLED and 5-way button, USB firmware update (80CH), "compatible with all Fat Shark
goggles". Its manual's Settings list has SWITCHING (Fast / Slow / Off), OSD, SHOW
RSSI, **FS SWITCH (ON / OFF)**, ALL BANDS and FAN (search-result snippets). No SPI
or serial channel control is documented, and the ExpressLRS backpack has no
Wildfire target. So **Switch is the likely Module type, unverified**.

Not known: what FS SWITCH does exactly (the name suggests the Fat Shark goggle
channel lines, the three CS pins); whether it reads the pins as an absolute
channel 1-8 or as up / down steps, and in which band; whether its pins have
pull-ups, pull-downs or are ever driven; their logic level; its pin 8.

Bench procedure (after module-bay-mod-safety.md steps 1-14, Module None):
1. Wildfire in the bay, bay powered: Check Pins with FS SWITCH OFF, then ON.
   1 1 1: pull-ups; 0 0 0: pull-downs or nothing connected; "toggling": it drives a
   pin: stop. Multimeter across each resistor: ≤ 0.3 V.
2. FS SWITCH ON, Module Switch, Mode "Wiring A (standard)". Dial CH1, CH2, CH3,
   CH5 with a few seconds each and watch the Wildfire's channel:
   * it goes to channel n of its band for CHn (in some wiring): absolute, the
     Switch type fits; try the other wirings (B-F) until CH1-CH8 map to 1-8;
   * it moves by one channel per change, whatever the size: steps; the goggle has
     no step type, so it cannot be controlled;
   * nothing happens: FS SWITCH is something else, or the wiring is wrong.

HDZero issue #108 reports a bad picture from a Wildfire in the HDZero goggle that
was fine in an HDO2 (cause unknown); unrelated to the mod.

Sources: Foxeer product pages for the
[Wildfire 80CH](https://www.foxeer.com/foxeer-wildfire-5-8ghz-80ch-dual-receiver-g-607) and
[Wildfire Lite 80CH](https://www.foxeer.com/foxeer-wildfire-lite-4-9g-5-8ghz-80ch-dual-receiver-g-617);
the Foxeer manual
([PDF](http://download.foxeer.com/manual/MR1622/WildFire_Goggle_Receiver_User_Manual.pdf),
[transcription](https://manuals.plus/m/6992f2b851eddfc8f1905d4f2f449bce6b5c4256039b864e3c29ce2bda0c9b57))
as quoted by search results (the pages themselves could not be opened); the
ExpressLRS Backpack's `targets/`.

## ELRS / MSP in analog mode

* VTX channel (MSP_SET_BAND_CHAN / SET_FREQ): applied and stored only when the
  goggle tunes the analog source (built-in receiver, or a controlled bay module);
  a Switch module gets the channel's place in its band (A1, B1 ... L1 all select
  CH1).
* MSP_GET_BAND_CHAN / GET_FREQ: the analog channel; 255 / 0 for an uncontrolled
  bay module.
* Dial long-press confirm and ELRS page "Send VTX freq": the analog channel.

## Host tests (no hardware)

`make -C utilities/module_bay_test test` (plain gcc on a PC, ~2 minutes, real
time): **170 checks, all pass** (on `ripples-all`); `utilities/module_bay_test/mutants.sh`:
every mutant of `src/module` is killed; `utilities/module_bay_test/check_strings.py`:
every string of the page translated, in the font and fitting, in every language.

* `test_softspi` (15): the real `softspi.c` against a stand-in for `/sys/class/gpio`.
* `test_settings_module` (17): `settings_load()` (ripples' `settings.c`) with the real
  minIni: `[module] type` 0-4 load as they are; 5, -1, 99, 2^32 + 2, text, 0x10, empty
  load as None; setting and channel.
* `test_module_worker` (91, plus 3 as a batch 1 Goggle): `module.c` against
  recording module types whose init sends no channel: Module None touches nothing
  (power-on, power cycle, source change, WiFi on / off); every type waits 2.0 s
  after every power-on (cold, a quick off / on, power-on during a transfer, Module
  chosen or WiFi off within the 2 s); the rapidFIRE 5.0 s resend; release on
  power-off and WiFi on within one frame; Module and Mode set together; the pin
  check, also WiFi switched on while it reads; the SDIO interlock (`bay_pins_busy()`
  against stand-in sysfs files), the sunxi_dump lock and its three reads; stored
  channels and the source change; ripples' Dual (the built-in receiver, bay module
  untouched) and a repeated bay power command (nothing set up again).
* `test_module_types` (44): the real Switch, rapidFIRE, SteadyView and SteadyView
  X types on the real worker with a recording pin model: None touches no pin;
  every type and power path keeps the pins inputs for 2.0 s; the Switch sequence
  against models of a True-D's 1-8 mode (channel 8 included); the rapidFIRE frames
  (every CS / CLK edge ≥ 100 µs apart, DATA only while CLK is low, CS first at the
  entry), also under a signal storm that cuts the worker's sleeps short (11,000+
  signals, shortest gap 101 µs); a model of MOD 1.17's receiver takes every
  commanded channel; the pins inputs within one frame of a power-off.

## Untested on hardware

Everything that touches a real goggle:

* the PG bank voltage (PIO_POW_VAL) on each goggle, and the mod with series
  resistors (module-bay-mod-safety.md's checklist);
* whether PG0-PG2 reach the bay at all on the Goggle 2, and which module pin each
  reaches on both;
* that the sysfs exports work on PG0-PG2 after `rc.sh` disabled them, and that
  WiFi still starts after module control or Check Pins released them;
* the real timing of rapidFIRE, SteadyView and SteadyView X on the V536 (sysfs
  write cost, timer slack), and the DM5680's bay power switching delay;
* Switch pin orders, rapidFIRE, SteadyView, SteadyView X and a True-D with MOD
  1.17 on either goggle with this firmware; the Wildfire's FS SWITCH;
* the WiFi interlock (enable WiFi with a module active, disable it again) and the
  PG_CFG0 / PIO_POW_VAL reads through `/sys/class/sunxi_dump`.
