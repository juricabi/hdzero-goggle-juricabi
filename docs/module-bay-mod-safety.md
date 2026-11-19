# Module bay wiring mod: what to build, why, and how to check it

## First: is the PG bank 1.8 V or 3.3 V? Settle it before you solder

The three pins of the mod, **PG0-PG2**, belong to port G of the V536 SoC. Port G has
its own supply, VCC-PG, which the SoC allows at 1.8 V or at 3.3 V. Which one the HDZero
goggle uses is not documented, and **the evidence points to 1.8 V**:

* The official Goggle 1 system configuration (`sys_config.fex` inside
  `Recovery/HDZG_OS.bin` of the recovery package `HDZEROGOGGLE_Rev20250319.zip`) lists
  the PMIC's regulators in `[regulator0]`. `vcc-pg` is on **`axp2101_bldo1`** (line
  1636: `regulator11 = "axp2101_bldo1 none vcc18-pc vcc-lvds vcc18-lcd vcc18-dsi vcc-tv
  vcc18-hdmi vcc18-cmbcsi0 vcc18-cmbcsi1 vcc-efuse vcc18-pf vcc-pg vcc18-wifi
  vcc-dcxo-io"`): every load on that regulator whose name carries a voltage is a 1.8 V
  load. The 3.3 V supplies are elsewhere (`vcc-io`, `vcc33-pc`, `vcc-pa`, `vcc-pd` on
  `dcdc1`, which the file sets to 3.3 V; `vcc-pf` on `aldo4`, 3.3 V). `vcc-pg` appears
  on no other regulator.
* The file does not set bldo1's voltage: `;bldo1_vol = 1001800` (1.8 V, on) in
  `[power_sply]` (line 47) and `;pc_bias = "axp233:bldo1:1800"` in `[gpio_bias]` (line
  27) are both commented out. The voltage then comes from the PMIC's default or the boot
  loader, which the file does not show.
* One name points the other way: `[wlan]` gives the WiFi chip's power and I/O supply as
  `wlan_power` / `wlan_io_regulator = "vcc33-wifi"` (lines 1189-1190), a name that no
  regulator in `[regulator0]` carries.

So: **probably 1.8 V, unproven: a guess from names** (the independent review of
2026-11-13 read the same lines again). Names are not a measurement, and nobody has
measured a goggle; Check Pins settles it. [analysis]

**How it is settled, with nothing soldered:** Module Bay > **Check Pins** (checklist
step 3). It shows the bank supply as the SoC itself reports it (PIO_POW_VAL bit 6:
below 2.0 V shows as "1.8 V" in orange, above 2.5 V as "3.3 V"; "? V" if the register
could not be read). If it cannot tell, the pin finder with a meter (step 4).

**What it decides:**

| Check Pins shows | what to build | why |
|---|---|---|
| "Pins … , 3.3 V" | **path R**: a 4.7 kΩ resistor in each wire (section 2) | a 3.3 V high reaches a 5 V True-D's input in practice, not by guarantee (section 3) |
| "Pins … , 1.8 V" (orange) | **path S**: the same resistors plus the LM339 level shifter of section 5, or no mod | resistors alone are **safe** at 1.8 V (section 2: at most 0.62 mA into a pin's clamp), but they **do not work**: a 1.8 V high (at most 1.98 V) is far below the 3.0 V a 5 V ATmega needs, so the True-D would never see a high, i.e. no clock and no command |
| "Pins … , ? V" | nothing yet: the pin finder (step 4) | |

## How to read this page

This is the one page to follow for the hardware side of module bay control
(firmware side: [module-bay.md](module-bay.md); flashing the goggle: first-flash.md
of the `all` branch, for its images; this branch has no flashing guide yet). Nothing here has been tried on a real goggle. Every
statement carries one of three marks:

* **[analysis]**: worked out from datasheet numbers or from the code; no hardware
  involved;
* **[sim]**: seen in a simulation: the goggle firmware's host tests
  (`utilities/module_bay_test`) or the True-D project's board simulator;
* **[hw]**: needs hardware; not known until someone measures it.

Facts cited from the bay pin research (furiousdfw `docs/bay-pin-research.md`)
keep its evidence grades: **(a)** manufacturer document, **(b)** published
schematic, design file, firmware source or measurement, **(c)** inferred from
products that work in these bays, **(d)** unknown, **(snippet)** seen only as a
search-engine snippet (weak).

## Short version

1. Settle the bank voltage first (the box above): Check Pins, before any soldering.
2. **Path R (3.3 V)**: three wires from three SoC pins of the goggle (PG0-PG2, which are
   also the WiFi chip's bus) to the channel pins 1-3 of the module socket, with a
   **4.7 kΩ resistor in each wire, soldered at the goggle-side connector pin**. Nothing
   else on the bay board protects those pins. **Path S (1.8 V)**: the same resistors,
   then the LM339 shifter of section 5 between them and the socket. [analysis]
3. Follow the checklist of section 8 from step 1 to step 26. Every step says what you
   should read and at what number to stop.
4. True-D: MOD 1.17, `BAY:RAPIDFIRE`, goggle Module = **rapidFIRE**. Never put a
   True-D with the stock firmware (3.9D), MOD 1.15 or MOD 1.16 in `BAY:AUTO` into
   a wired bay. [analysis]
5. With a wired bay, never: touch the goggle-side connector with a probe while the
   goggle is on; insert or remove a module while the goggle is on; plug a USB cable into
   a True-D while it sits in the bay. [analysis]
6. Fly with WiFi off. Module control pauses while WiFi is on. [analysis, sim]

## 1. The three wires

**What they are.** The goggle drives SoC pins **PG0, PG1, PG2** of the Allwinner
V536 (GPIO 192, 193, 194 in the code: `GPIO_MODULE_CLK/DAT/CS`, from pkendall64's
analog-module-control, (b)). The same pins are the **SDIO bus of the WiFi chip**:
PG0 = SDC1 CLK, PG1 = CMD, PG2 = D0 (the official images' sys_config: `[sdc1]` on
PG00-PG05, function 2, (b)). On the Goggle 1 the WiFi chip sits on the Expansion
Module V2, behind the bay board's goggle connector; on the Goggle 2 it is built in.

**Where they go** (pkendall64's wiki "Analog Module Backpack Control", for the
Goggle 1, (b)): "Solder 3 enamelled copper wires (or other small gauge wire) to the
3 pins show on the connector that plugs into the HDZero goggles", route them to the
other side of the bay board "leaving space for the screw heads and keeping the
wires away from where the module will press down on the board", and solder them to
pins 1, 2 and 3 of the 9-pin module socket ("Feed the Pin 2 wire under the module
connector and solder to the connector leg"). The wiki's photos show the three wires
on three adjacent pins of one row of the 2×10 goggle connector, labelled Pin 1, 2,
3 after the socket pin each goes to. The text does not say which SoC pin each
connector pin is. From the code's roles and the standard rapidFIRE wiring (CS3 =
CLK, CS2 = DATA, CS1 = NSS: ExpressLRS, (b)) the mod presumably connects **PG0 →
socket pin 1 (CS3, clock), PG1 → pin 2 (CS2, data), PG2 → pin 3 (CS1, chip
select)**. [hw] for the actual connector pins: nobody has checked continuity on a
goggle. Checklist step 20 checks the order after the mod; the pin finder (step 4) can
check it before.

**The stock bay board does nothing on these pins.** Socket pins 1-3 are not
connected on a stock bay board. An open-source clone of the Goggle 1 bay board
(NOSDracing/HDZero-Module-Bay, Gerber net attributes, (b)) has no components at
all, only traces: J2 pins 1-3 not connected, 4 Video, 5 Audio_Left, 6
Audio_Right, 7 not connected, 8 GND, 9 +5V. HDZero's own documentation (a): "HDZero
goggle provides 5V power to analog receiver and in-takes its CVBS output. There are
no control signals from the goggle to analog receiver." What the official board
carries is not documented (d). So **your resistors are the only protection** between
the module and the SoC (and the WiFi bus). [analysis]

**Goggle 2.** The mod is documented for the Goggle 1 only. Whether PG0-PG2 reach
the Goggle 2's bay connector at all is unknown. [hw]

## 2. Why 4.7 kΩ in each wire

### The numbers used

| | value | source |
|---|---|---|
| V536 VCC-PG (PG bank supply) | 1.62-1.98 V (1.8 V) or 2.97-3.63 V (3.3 V); abs. max 3.96 V | V536 datasheet rev. 1.6, (a) |
| V536 V_IH / V_IL | ≥ 0.7 × VCC / ≤ 0.3 × VCC; input max VCC + 0.3 V | (a) |
| V536 V_OH / V_OL | ≥ VCC − 0.2 V / ≤ 0.2 V (no load current stated) | (a) |
| V536 internal pull-up / input leakage | 80-120 kΩ / ≤ 10 µA | (a) |
| V536 clamp (injection) current limit | **not given**; this page uses ≤ 1 mA per pin as the design limit (a common choice for CMOS pins, not an Allwinner number) | (d) |
| ATmega328P at 5 V (True-D), V_IH / V_IL | ≥ 0.6 × VCC / ≤ 0.3 × VCC (VCC 2.4-5.5 V); pin max VCC + 0.5 V | Microchip DS40002061B, Common DC Characteristics, (a) |
| ATmega328P I/O pull-up | 20-50 kΩ | (a) |
| ATmega328P output | ~26 Ω source resistance at 5 V (4.47 V at 20 mA, 25 °C, Fig. 31-355); 40 mA per pin abs. max | (a) |
| RTC6715 receiver chip (the "dumb" modules' channel pins) | V_IH 0.7 × V_IO, V_IL 0.3 × V_IO, internal pull-ups of unstated strength, logic abs. max 5 V | RichWave datasheet, (a) |
| clamp diode forward drop | 0.3 V taken as the worst case (a lower drop means more current) | [analysis] |

The final review re-derived every current on this page from the ATmega328P and LM339
datasheets; the V536 rows could not be re-fetched for that review and rest on the
earlier reading of the datasheet.

**PG bank: 1.8 V or 3.3 V** is the question of the box at the top. The SoC reports
it: PIO_POW_VAL (0x0300B348) bit 6 = 1: below 2.0 V, 0: above 2.5 V (V536 datasheet
p. 888, (a)). **Check Pins** shows it.

### What can push current into PG0-PG2, with and without the resistor

R = 4.7 kΩ in each wire. "Into the clamp" means the goggle pin is an input (or "IO
disabled", as at boot) and the excess current flows through its protection diode
into VCC-PG. [analysis] for every number; when each case can happen is from the code
[analysis] and the host tests [sim].

| case | duration | 3.3 V bank | 1.8 V bank | without resistor |
|---|---|---|---|---|
| **True-D stock bootloader**: D13 (bay pin 3, the CS wire = PG2 with the presumed wiring; measured on the owner's True-D 2026-11-13) driven 5 V push-pull, goggle pin input: (5 − VCC − 0.3) / R | 100 ms at every True-D power-on or reset, (b) + sim | **0.30 mA** | **0.62 mA** | ~50 mA at 3.3 V (1.4 V over ~26 Ω), more at 1.8 V: far above any injection limit and the AVR's own 40 mA |
| the same pulse while the goggle drives that pin low: 5 V / R | cannot happen: the goggle keeps every pin an input until 2.0 s after bay power-on [sim] | 1.06 mA | 1.06 mA | a short between two outputs |
| bootloader's low phase (0.1-1.39 s) against a goggle high: VCC / R | cannot happen, same reason [sim] | 0.70 mA | 0.38 mA | a short |
| **stock True-D firmware 3.9D** (also MOD 1.15, MOD 1.16 in `BAY:AUTO`): D13 is the Dock-King link's TX, 5 V push-pull, idle high | ~6 s after every power-up, (b) | 0.30 mA into the clamp; 1.06 mA once the goggle drives (from 2.0 s) | 0.62 / 1.06 mA | tens of mA for seconds: **likely damage** |
| **goggle drives while WiFi is on**: GPIO output against the WiFi chip's drivers on CMD / D0 | the firmware never does it: no drive while WiFi is enabled, while the WiFi driver is loaded or while PG0-PG2 are in the SDIO function, and it lets go before WiFi starts [sim] | not limited by R (the contention is on the bus side of it) | the same | the same |
| **WiFi running with a module in the bay**: the SDIO host drives CLK / CMD / D0 at VCC-PG; a module output (bootloader pulse) or pull-up meets it | while WiFi runs and the module is powered | ≤ 1.06 mA (5 V output vs SDIO low); pull-ups 20-50 kΩ: ≤ 0.2 mA | the same | tens of mA |
| goggle high into an **unpowered module** (its clamp to its 0 V rail): (VCC − 0.3) / R | ≤ one rapidFIRE frame after a bay power-off (< 40 ms in the host test) [sim]; SteadyView's mode set, 70 ms by design | 0.64 mA | 0.32 mA | mA to tens of mA, can half-power the module's MCU |
| **True-D on USB, goggle off**: bootloader pulse into an unpowered goggle: (5 − 0.3) / R | 100 ms per reset, (b) | 1.0 mA | 1.0 mA | tens of mA into the goggle's unpowered rail |
| module pull-ups (True-D `BAY:1-8` / `STEP`, 20-50 kΩ) into a goggle input: (5 − VCC − 0.3) / (R_PU + R) | whenever the module is powered, also with Module = None | ≤ 57 µA | ≤ 117 µA | ≤ 70 / 145 µA |

**Never plug the True-D's USB in while it sits in a wired bay.** The "True-D on USB"
row is 1.0 mA, exactly at the design limit, at every reset, and a PC opening the port
resets the module. While a PC flashes it, the bootloader holds D13 as an output for the
whole session (furiousdfw `docs/bay-standards.md` 5.4), and the module's USB 5 V may also
reach bay pin 9 (furiousdfw `docs/bench-test.md` step 7 measures whether it does on a
given unit), which would put the PC's 5 V onto the goggle's bay supply. [analysis]

With the **no-LED bootloader** (furiousdfw `bootloader/`, flashed once with an ISP
programmer) and MOD 1.17 the True-D never drives a bay pin outside an ISP session
(furiousdfw: [analysis] and [sim], not yet on the unit, [hw]); the bootloader rows
then disappear. Keep the resistors anyway: they also cover a module flashed with
other firmware, a module on USB, ISP, and a goggle driving an unpowered module.

**Most modules have no resistor of their own on these lines.** JAFaR, FS-SkyRF and
the True-D connect their MCU pins directly; the only resistors found on bay lines
are 1 kΩ inside some modules, on their own receiver-chip lines (bay pin research,
(b)). So every number above assumes nothing on the module side.

### Why this value

* **Lower bound**, ≤ 1 mA into a clamp from a 5 V output: R ≥ 1.4 kΩ (3.3 V
  bank), ≥ 2.9 kΩ (1.8 V bank). **1 kΩ is too low**: 1.4 mA (3.3 V) or 2.9 mA (1.8
  V). [analysis]
* **Upper bound**, a goggle low must read low at the module through its strongest
  pull-up: 5 V × R / (R + 20 kΩ) ≤ 1.5 V (AVR V_IL) gives R ≤ 8.6 kΩ. With 4.7 kΩ:
  0.95 V at a 20 kΩ pull-up, 0.43 V at 50 kΩ, 0 V in RAPIDFIRE (no pull-up).
  [analysis]
* **Speed**: 4.7 kΩ × ~25 pF (wire and module pin) = 0.12 µs, against the goggle's
  shortest step: ≥ 100 µs per rapidFIRE clock level, 8.7 µs per SteadyView X UART
  bit. [analysis]
* **The WiFi bus**: soldered at the goggle-side connector pin with < 5 mm of stub,
  the resistor hides the wire and the module from the bus: at the 25-50 MHz SDIO
  clock the bus sees about 4.7 kΩ (≤ 0.7 mA peaks at 3.3 V) instead of 10-20 cm of
  wire and a module input. [analysis]; that WiFi keeps working with the mod [hw]
  (checklist step 21).
* **Nothing found argues for another value.** No published design puts series
  resistors between goggle and module at all (bay pin research, (b)/(d)); the
  True-D project checked 4.7 kΩ from the module side and keeps it (furiousdfw
  `docs/bay-standards.md` 5.6). 3.3 kΩ-6.8 kΩ would also meet both bounds at
  3.3 V; 4.7 kΩ leaves margin both ways.

### "Dumb" modules: the low must reach about 1.0 V

A module that passes the pins straight to an RTC6715 receiver chip reads a low
only below 0.3 × V_IO ≈ **1.0 V** (3.3 V V_IO), against internal pull-ups whose
strength the datasheet does not give (a). Through 4.7 kΩ the low stays below
1.0 V only if that pull-up is above ~11 kΩ (3.3 V × 4.7 / (4.7 + R_int) ≤ 1.0 V).
[analysis]; the real value [hw]: check "low ≤ 1.0 V" at the module side (the note in
checklist step 20). The True-D is different: an AVR input, low below 1.5 V.

## 3. The honest margin at 3.3 V

In `BAY:RAPIDFIRE` the True-D has no pull-ups, so it sees the goggle's high
exactly: V_OH ≥ VCC-PG − 0.2 V (a).

| | ATmega328P V_IH at its supply | margin of a 3.3 V high (3.1-3.3 V) |
|---|---|---|
| guaranteed (0.6 × VCC, (a)) | 3.0 V at 5.0 V, 3.15 V at 5.25 V, **3.3 V at 5.5 V** | +0.1 to +0.3 V at 5.0 V; −0.05 to +0.15 V at 5.25 V; none at 5.5 V; negative if VCC-PG sits at its 2.97 V minimum |
| typical (Fig. 31-356, 25 °C; the −40 to 105 °C curves nearly overlap, (a)) | ≈ 2.65 V at 5.0 V, ≈ 2.9 V at 5.5 V | ≈ +0.45 to +0.65 V at 5.0 V; ≈ +0.2 to +0.4 V at 5.5 V |

So a 3.3 V bank drives a True-D's input high **in practice, not by guarantee**.
[analysis] The module's supply is the bay's 5 V (pin 9); its tolerance is not
documented (d), and whether the True-D's ATmega runs directly from it or from a
regulator is not known (d). Measure the bay supply (checklist step 2); at 5.25 V
or more the guaranteed margin is gone, and the typical one shrinks by ~0.05 V per
0.1 V of supply. [analysis]

## 4. While WiFi is on

* The goggle never drives PG0-PG2 while WiFi is enabled, while the WiFi driver
  (`xradio_wlan`) is loaded, or while PG_CFG0 shows any of them in the SDIO
  function (checked again every 5 s); it releases them (inputs, unexported) before
  the WiFi page starts WiFi, and waits for a running Check Pins to finish. WiFi does
  not start while module control still holds the pins: the WiFi page waits up to 3 s
  for their release and otherwise refuses with a message. [analysis, sim]
* The module still sees the SDIO traffic on its pins. A rapidFIRE listener (True-D
  MOD 1.17 `BAY:RAPIDFIRE`) accepts only checksummed frames and switches its
  receiver off for 200 ms on a pin storm (furiousdfw, [sim]). A module that reads a
  static code (True-D `BAY:1-8` / `STEP`, Fat Shark style) may change channel.
  [analysis]
* Whether the WiFi chip's I/O is powered while WiFi is **off** is unknown. If it is
  not, a goggle high on CLK / CMD / D0 back-feeds the WiFi chip through its input
  clamps, limited only by the SoC's drive (the resistor is on the module side, not
  between SoC and WiFi chip). This applies to pkendall64's code as much as to this
  firmware; only with Module = None is it ruled out. [hw]
* Checklist steps 5, 21 and 25 check that WiFi still connects: after Check Pins, with
  the mod, and with module control running.

## 5. If Check Pins says 1.8 V

With a 1.8 V bank the 4.7 kΩ resistors still keep every case of section 2 below
0.62 mA, but **a 5 V module cannot see the goggle's high**: 1.8 V (at most 1.98 V)
against the ATmega's 3.0 V V_IH (typically ~2.65 V). No resistor value fixes it,
and open-drain does not either (the released goggle pin is clamped at ~2.1 V).
[analysis, furiousdfw bay-standards 5.6] One direction is enough: goggle out,
module in.

### The option: one LM339 comparator per goggle (all three lines)

Chosen because it needs **only the bay's 5 V and GND** (both on the bay board, (b)
from the clone), puts **no clamp, pull-up or accelerator on the SDIO bus**, and
**TI's** LM339 is rated for inputs up to 36 V whether it is powered or not (below).

Per line (three times; the LM339's fourth comparator is spare):

```
goggle connector pin (PG0/1/2)
   └─ R1 4.7 kΩ (the mod's resistor, at the pin, < 5 mm stub)
        └──●── LM339 IN+          LM339 OUT ──●── R4 1 kΩ ── module socket pin 1/2/3
           │                                  │
           R2 1 MΩ to +5 V (bay)              R3 10 kΩ to +5 V (bay)
                                              (R5 18 kΩ to GND: only for a 3.3 V module)
   LM339 IN− : the reference, 5 V × 1 k / (4.7 k + 1 k) = 0.88 V (one divider for all three)
   LM339 VCC: bay +5 V, 100 nF to GND at the pin; GND: bay GND
   spare comparator: IN+ to GND, IN− to the reference, output open
```

Parts: **an LM339 made by Texas Instruments** (LM339N, DIP-14, in a DIP-14 socket on
the board; or LM339D, SO-14), not any maker's (below); 1/8 W resistors 3 × 4.7 kΩ,
3 × 1 MΩ, 3 × 10 kΩ, 3 × 1 kΩ, 1 × 4.7 kΩ + 1 × 1 kΩ (1 %) for the reference,
optionally 3 × 18 kΩ; one 100 nF ceramic capacitor (a second one on the reference is
good practice). Build it on a scrap of perfboard, insulate it with Kapton, and place
it where neither the module nor a screw head can press on it; whether the HDZero bay
has room for it is [hw].

LM339 numbers (TI SLCS006Z, non-B, (a)): supply 2-36 V; absolute maximum input
voltage −0.3 to 36 V on either input, given "with respect to network ground" and with
no condition on the supply, so also above VCC and with VCC = 0 ("either or both inputs
can go to 30 V without damage"); common-mode range 0 to VCC − 1.5 V (25 °C), VCC −
2.0 V (full temperature); input bias 25 nA typ, 250 nA max, flowing out of the pin;
output open collector, V_OL ≤ 400 mV at 4 mA, sinks ≥ 6 mA (16 mA typ) at V_OL 1.5 V,
output short to ground unlimited; response 1.3 µs (5 mV overdrive), 0.3 µs (TTL step).

**Why TI's part.** The design needs inputs that may sit above an unpowered VCC: with
the bay switched off (batch 2 hardware) and WiFi running, the SDIO host drives CLK /
CMD / D0 at VCC-PG into R1 and the comparator inputs while the comparator has no
supply. TI's absolute maximum ratings give the input range against ground,
independent of VCC. The final review found this only in TI's LM339 datasheet (other
makers' sheets were not re-read for this page). For another maker's LM339, check its
datasheet for all three before you use it:

* the absolute-maximum input voltage: stated against GND (for example −0.3 to 36 V),
  not as "VCC + 0.3 V";
* the behaviour with VCC unpowered: inputs of at least 2.5 V while VCC = 0 must be
  allowed;
* input clamp diodes: none from the inputs to VCC. With such a diode an unpowered
  comparator takes (VCC-PG − 0.6 V) / 4.7 kΩ ≈ 0.26 mA per line from the SDIO bus and
  feeds it into the bay's unpowered 5 V rail.

If the datasheet does not state all three, use TI's part.

**How it behaves** [analysis for all; nothing built, [hw]]:

* **Thresholds.** Goggle low: ≤ 0.2 V + 4.8 µA (through R2) × 4.7 kΩ ≈ 0.22 V at IN+, below
  0.88 V → output low. Goggle high: ≥ 1.42 V (V_OH at the 1.62 V minimum supply),
  above 0.88 V → output high. At least 0.5 V margin each side; a ±5 % bay supply
  moves the reference to 0.83-0.93 V.
* **Levels at the module.** 5 V module (True-D): high 5 V through R3 + R4 (no
  pull-up in RAPIDFIRE), low ≤ 0.4 V; with a 20 kΩ pull-up (1-8 / STEP) the low is
  0.4 + 4.6 × 1 k / 21 k ≈ 0.62 V. Both well inside 3.0 V / 1.5 V.
* **Goggle pins idle** (Module = None, the first 2.0 s, the goggle app stopped):
  R2 lifts IN+ to the goggle pin's clamp (~2.1 V), output high: the module reads
  all three lines high, as from an unconnected bay with pull-ups (code 7). The
  goggle pin takes (5 − 2.1) V / 1 MΩ ≈ 3 µA into its clamp: inside its 10 µA
  leakage figure. IN+ at 2.1 V is inside the common-mode range (≤ 3.0 V). If the SoC
  has its internal pull-down (80-120 kΩ) on a pin, IN+ sits at 0.37-0.53 V instead and
  that output is low: harmless, and checklist step 18 shows it.
* **The True-D bootloader pulse never reaches the goggle**: D13 (bay pin 3) now meets
  the comparator's output, not PG2. D13 high against an output low: (5 − 0.4) V / 1 kΩ
  = 4.6 mA, within the LM339's sink and the AVR's 40 mA (and only if the goggle
  were driving low, which it does not in the first 2.0 s); D13 low against the
  output high: 5 V / 11 kΩ = 0.45 mA.
* **WiFi on, same pins.** The bus sees the < 5 mm stub, then 4.7 kΩ to a
  comparator input (a few pF) and the 1 MΩ: a DC pull toward 5 V of ≈ 3 µA (an SDIO
  pull-up of 100 kΩ at 1.8 V is 18 µA), and about 4.7 kΩ at the SDIO clock: the
  same as the plain resistor mod. The comparator cannot follow 25-50 MHz; the
  module sees slow garbage while WiFi runs (as it would without the shifter).
  **WiFi with this shifter is expected to work as well as with the plain mod;
  neither is tested.** [analysis, hw]
* **Bay unpowered** (batch 2 switches the bay): the comparator is unpowered; TI's
  inputs have no clamp to the supply (36 V input range regardless of VCC), so the
  goggle pin sees 4.7 kΩ + 1 MΩ to a 0 V rail: ≤ 2 µA; the module sees only
  open-collector outputs and pull-ups to the unpowered rail. No back-feeding either
  way. [analysis]
* **Speed.** ≤ ~1.3 µs comparator delay plus the output rise through 10 kΩ ×
  ~25 pF = 0.25 µs, against ≥ 100 µs rapidFIRE levels: fine. SteadyView X (8.7 µs
  UART bits) gets ~1 µs of skew between falling and rising edges: probably fine,
  [hw]. SteadyView's register read-back cannot pass a one-way shifter: the goggle
  then reads its own idle level and always writes the channel register (which is
  what it needs). [analysis]

**Options not chosen**, and why [analysis]:

* 74HCT / 74AHCT buffers at 5 V: TTL inputs need ≥ 2.0 V for a high (a): 1.8 V is
  not enough.
* Dual-supply translators (SN74LVC1T45 / 2T45 and the like): good parts, but their
  A side needs the goggle's 1.8 V as a supply, and whether any 1.8 V rail reaches the
  bay board is unknown (d). Where the two reference voltages would come from is
  exactly what nobody knows for this board.
* Auto-direction boards (TXS0108E, TXB0108): edge-rate one-shots and (TXS) 10 kΩ
  pull-ups on the goggle side. Directly on the SDIO bus they add load and actively
  drive edges on a 25-50 MHz bus; behind the 4.7 kΩ resistor they fight it. **WiFi
  with one of these hanging on CLK / CMD / D0 is doubtful.**
* BSS138 MOSFET boards: also need the 1.8 V reference, put 10 kΩ pull-ups to 1.8 V
  on all three lines (CLK included), and a BSS138's V_GS(th) of up to 1.5 V leaves
  little of a 1.8 V gate drive. The True-D in RAPIDFIRE has no pull-up on its side
  either.

### A 3.3 V module in the 1.8 V case (rapidFIRE, SteadyView, Wildfire)

* A 1.8 V high is below a 3.3 V CMOS input's 0.7 × 3.3 V = 2.31 V: a 3.3 V module
  needs the shifter too, unless its own input threshold is lower (unknown for each,
  below). [analysis]
* Fit **R5 = 18 kΩ** from each comparator output to GND: the high becomes
  5 V × 18 / (10 + 18) = **3.2 V** (above 2.31 V, below a 3.3 V part's 3.6-4 V
  limits), the low stays ≤ 0.4 V (+ ≤ 0.3 V through 1 kΩ against a 10 kΩ pull-up).
  [analysis]
* **SteadyView**: its channel lines go to RTC6715-class receiver chips: V_IH 2.31
  V, V_IL 0.99 V, logic abs. max 5 V (a). 3.2 V / ≤ 0.7 V fit. What else sits on those
  lines in a SteadyView is unknown (d). Its pin 8 is an RSSI output: lift it (section 6).
* **rapidFIRE**: its controller and input thresholds are not published (d). A
  3.3 V ESP drives it directly in the ExpressLRS backpack, so 3.2-3.3 V highs work
  (c). Whether its pins tolerate 5 V is unknown (d): use R5.
* **Foxeer Wildfire**: levels, pull-ups and whether it ever drives these pins are
  unknown (d): checklist steps 7-11 first; use R5 unless it measures as a 5 V part.

## 6. The 9-pin module socket, pin by pin

Pin 1 is the channel-pin end, pin 9 the +5 V end. Some projects number from the
other end: **identify the pins by +5 V and GND with a meter, not by number**
(checklist steps 2 and 7). Grades from the bay pin research.

| pin | what the HDZero bay does | what a module may put there |
|---|---|---|
| 1 | stock: not connected; with the mod: PG0 (SDIO CLK) through 4.7 kΩ (b) | rapidFIRE CLK (CS3) (b); SteadyView SPI clock (b); True-D D11 (measured 2026-11-13), never driven by MOD 1.17; RTC6715 CS pin with pull-up (a); Wildfire (d) |
| 2 | stock: not connected; with the mod: PG1 (SDIO CMD) (b) | rapidFIRE DATA (CS2), the module answers queries on it (b); SteadyView DATA, driven by the module on read-back (b); True-D D12, never driven (MOD 1.17) (b); Wildfire (d) |
| 3 | stock: not connected; with the mod: PG2 (SDIO D0) (b) | rapidFIRE NSS (CS1) (b); SteadyView chip select (b); True-D D13 (measured 2026-11-13): the stock bootloader's pulse, the stock firmware's link TX (b); Wildfire (d) |
| 4 | video into the TP2825 decoder (termination unknown) (b) | video out, ~1 Vpp into 75 Ω (a) |
| 5 | audio left, always mixed into recordings (b) | audio (rapidFIRE 6.5 MHz carrier audio) (b) |
| 6 | audio right (b) | audio (6.0 MHz) (b) |
| 7 | official board unknown, probably GND; the clone leaves it open (b)/(d) | GND (rapidFIRE, SteadyView) (b) |
| 8 | **GND** (pkendall64's wiki; the clone, (b)) | **differs by vendor**: GND on rapidFIRE (b); **RSSI output on the SteadyView, lift its pin 8** as the wiki shows (b); tied to +5 V on one old DIY design (FS-SkyRF, by its schematic's pin order, not verified physically: it would short the bay's 5 V) (b); RSSI on C5VRX (b); **True-D and Wildfire unknown** (d): checklist step 7 |
| 9 | +5 V from the goggle, switched on batch 2 goggles (b); current rating unknown (d) | supply in |

A SteadyView with pin 8 lifted still works in an HDZero bay: it gets GND from pin 7
(c).

## 7. Mechanical short risk

Weak evidence (forum posts seen only as search snippets), but cheap to prevent:

* HDZero V1 bay: screw heads and exposed solder points touching the back of
  modules: "screw heads that would contact my furious fpv module and it DID cause a
  issue"; another user suspected this killed a rapidFIRE (snippet).
* Early True-D V3 units: "the header on the top module panel could touch the bottom
  panel … causing a 5V-to-GND short", fixed from V3.5 (snippet).
* A forum post describes a "fried 5V voltage regulator BEC inside the HDZero
  goggles" (snippet).

So: inspect the bay and the module (checklist steps 6, 7 and 14), cover screw heads
and exposed solder points facing the module with **Kapton tape**, and route the mod
wires (and a shifter board) clear of the screws and of where the module presses on
the board, as pkendall64's wiki says. [analysis]

## 8. The checklist

Read it once to the end before you start. Then do the steps in order, 1 to 26. Each
step says what to do, what you should read, and **stop if**: the reading at which you
do not go on.

**Rules for every step**

* **Stop** means: switch everything off (goggle and bench supply) and do not go on
  until the cause is found and fixed. After a fix with the soldering iron on the bay
  board, start again at step 16; after any other fix, repeat the step that stopped you.
* **Never put a probe, a clip or a wire on the goggle-side 2×10 connector, its pins or
  the resistors at it while the goggle is on.** Those are the SoC's own pins; a probe
  that slips can short one to its neighbour. Attach clip leads there only with the
  goggle off; then switch on, read, and switch off before you move them. With the
  goggle on, measure only in the **holes of the 9-pin module socket** with the bay
  empty: push thin probe tips into the holes, never lay them across the socket's top.
* **Insert or remove a module only with the goggle off.**
* **Never plug a USB cable into a True-D while it sits in a wired bay** (section 2),
  and never flash it there.
* Ohm readings: wait 5 s for the value to settle (capacitors charge). "OL" (over range)
  counts as above every limit on this page.
* Switch everything off at once if the module gets too hot to keep a finger on its
  case for 5 s, or if anything smells.
* **Your path**: **R** (resistors) if step 3 showed 3.3 V, **S** (shifter) if it showed
  1.8 V. Where a step has a block for each path, do only yours.

**You need**: a multimeter (DC volts, ohms, diode test) with thin probe tips that fit
into the module socket's holes; two insulated mini hook clips (steps 4 and 23, both
optional); a current-limited bench supply, 5.0 V, with a limit you can set to 0.1 A and
0.5 A (step 10); two 10 kΩ resistors and an LED with a 1 kΩ resistor (steps 11-12);
Kapton tape; a fine-tip soldering iron; three 4.7 kΩ resistors (5 % or better) and thin
enamelled or insulated wire; for path S also the parts of section 5 and a DIP-14
socket. A monitor on the goggle's HDMI out helps you read the menu while your hands are
on the meter.

### A. Before any soldering: the PG bank voltage (goggle on, bay empty)

1. **Set up.** Flash this firmware and check the untouched goggle first (as in
   first-flash.md of the `all` branch, sections 1-4). Leave the bay empty. Module Bay:
   Module = **None** (the default; the Status line says "Off, bay pins untouched": with
   None the goggle never touches the bay pins). WiFi Module: Enable = Off, then Apply
   Settings (click it, then click again to confirm). Power: AnalogRX Power = **On**
   (only batch 2 goggles have this row; on a batch 1 goggle the bay is always powered).
2. **The socket's pins and the bay supply, in the socket holes.** Meter on DC volts.
   Of the two holes at each end of the socket, find the pair that reads 4.75-5.25 V:
   the hole at the red lead (for a positive reading) is **pin 9** (+5 V), the one at the
   black lead **pin 8** (GND); the hole at the far end of the row is **pin 1**. Write the
   voltage down (section 3 uses it). Then holes 1, 2 and 3 to hole 8, one at a time
   (on a stock board they are not connected). Switch the goggle off, mark pin 1 on the
   bay board (a dot of paint marker), and with the meter on ohms write down hole 9 to
   hole 8 after 5 s (step 17 compares with it).
   *Stop if* no end pair reads 4.75-5.25 V, any of holes 1-3 reads above 0.5 V, or hole 9
   to hole 8 reads 100 Ω or less with the goggle off.
3. **Check Pins, twice.** Goggle on. Module Bay > Check Pins (hint: "Click to read the
   3 bay pins (read only)"), click. The row shows "Reading...", then for example
   "Pins 1 1 0, 3.3 V": the levels of PG0, PG1 and PG2 in that order (1, 0, or
   "toggling" for a pin that changed during the 100 ms read) and the PG bank voltage.
   Wait 5 s and click again. Write both results down. The levels mean nothing yet (no
   wire is fitted; they depend on the SoC's internal pull resistors); the voltage
   decides:
   * both "3.3 V": **path R**;
   * both "1.8 V" (orange): **path S** (the resistors and the shifter of section 5), or
     no mod (then stop here: nothing else on this page applies);
   * both "? V": step 4 decides.

   If the row says "Not read: …" instead: "set Module to None first" → Module = None
   (step 1); "switch WiFi off first" → WiFi as in step 1; "pins in use by WiFi" →
   switch the goggle off and on with WiFi off; "pin state unknown" → reboot and retry (if
   it stays, report it: the pin finder of step 4 will refuse too); then click again.
   *Stop if* the two reads show different voltages (one 1.8 V, the other 3.3 V), or the
   row still says "Not read: no GPIO access" on both of 2 clicks after a fresh boot.
4. **Only if step 3 showed "? V": the pin finder.** (Otherwise go on with step 5. The
   same procedure can also confirm, before you solder, which connector pin is PG0, PG1
   and PG2; step 20 checks the order after the mod anyway.) `utilities/probe/
   bay_gpio_pattern.sh` drives PG0, PG1, PG2 high one after the other, only the pin under
   test an output, the other two inputs (`utilities/probe/README.md`, part 3); here you
   read them with clip leads, one connector pin per boot:
   * goggle off; bay empty; WiFi off (the script refuses while the WiFi driver is
     loaded); copy the script to the SD card root and name it `develop.sh`;
   * goggle still off: attach a hook clip (meter red lead) to the leg of one of the
     three connector pins of pkendall64's photos, so that it touches nothing else; push
     a short piece of solid wire into socket hole 8 and clip the black lead to it; with
     the meter on ohms, the clip must read above 10 Ω to hole 8, above 10 Ω to hole 9
     (for this one reading hold the black probe tip in hole 9, goggle off), and above
     10 Ω to each neighbouring pin of the connector (the black probe tip on the
     neighbour's leg, goggle off). Then it touches no ground or supply pin and no other
     signal; with the bay board in, the SoC's protection diodes can make a signal pin read
     anything from a few hundred Ω upward, which is fine. Switch the meter to DC volts;
   * power on and listen: 10 s nothing driven, then 1 beep → PG0 high for 10 s, 2 beeps →
     PG1, 3 beeps → PG2; three rounds, about 3 minutes; 4 long beeps: finished (the goggle
     app then starts). One long beep right after boot means refused: nothing was driven,
     the reason is in `bay_gpio_N.txt` on the card;
   * note after how many beeps your pin goes high and the voltage; power off; move the
     clip to the next pin; repeat for all three; finally delete `develop.sh` from the
     card.

   Highs of 3.0-3.6 V: **path R**. Highs of 1.6-2.0 V: **path S**.
   *Stop if* the clip reads 10 Ω or less to hole 8, hole 9 or a neighbouring connector pin
   with the goggle off (it touches a ground or supply pin or another signal, or you cannot
   place it on that pin alone: then do not run this step at all); a pin never reads above 0.5 V in all three rounds (not a PG
   pin: check against the photos); or a high reads between 2.0 V and 3.0 V, or above
   3.6 V.
5. **WiFi after Check Pins.** (Skip it if your goggle has no WiFi.) WiFi Module: Enable =
   On, Apply Settings; connect your phone or PC as you usually do. It must connect within
   60 s. Then Enable = Off, Apply Settings.
   If it does not connect: switch the goggle off and on (WiFi then starts at boot,
   before any Check Pins) and wait 60 s. If it connects now, the pins that Check Pins
   left as GPIO inputs were not handed back to WiFi: a firmware problem, not a wiring
   one. Note it, report it, from now on reboot before you use WiFi after a Check Pins,
   and go on.
   *Stop if* WiFi does not connect within 60 s after the fresh boot either.

### B. The module on the bench (module out of the goggle)

For a True-D, furiousdfw `docs/bench-test.md` steps 1-7 are the same checks in more
detail; its readings count here too.

6. **Look at the module.** Write down its version marking (True-D V3, V3.5 …). On a
   True-D V3 from before V3.5, look at the gap between its two boards: nothing metal of
   one may touch the other (step 7 also tests it electrically). Put Kapton tape over the
   back of the module, the side that faces the bay board (not over its 9 pins).
7. **Unpowered: ground, +5 V and pin 8.** Ohms (or continuity) from the **shield** (the
   outer metal body of an antenna socket, or the USB socket's shell) to each module pin:
   the pins that read 1 Ω or less are the module's ground (expect pin 7 and/or pin 8).
   The end of the row nearer to them is pin 9 (+5 V); the other end is pin 1. Then:
   * pin 9 to the shield: above 100 Ω after 5 s, also while you press the module's two
     boards gently together;
   * pin 8 to the shield: 1 Ω or less (ground: fine) or above 1 MΩ (not connected:
     fine).

   *Stop if* no pin reads 1 Ω or less to the shield; pin 9 reads 100 Ω or less to the
   shield; or pin 8 reads between 1 Ω and 1 MΩ to the shield (pin 8 carries a signal
   that the bay's ground would short: lift that pin as for a SteadyView, section 6, and
   repeat this step).
8. **Unpowered: pins 1, 2, 3.** For each: diode test, red lead on the pin, black on the
   shield; then ohms from the pin to the shield and from the pin to pin 9, each with both
   lead polarities; and the diode test on pin 9 (red on pin 9, black on the shield).
   Write everything down.
   *Normal for a True-D V3* (one unit, measured 2026-11-13): ~1.7 V on each of pins 1-3
   and ~1.64 V on pin 9, megaohms to the shield, no series resistors (black on the pin,
   red on the shield: 0.63 V each): all three pins read alike and like pin 9, which is the
   module's 5 V rail through the chip's protection diodes, not an LED.
   *Stop if* one or two of pins 1-3 read 1.5-2.2 V clearly lower (by 0.2 V or more) than
   the others (an LED on that pin: it would load the line far more than 4.7 kΩ can drive),
   or a pin reads below 10 kΩ to the shield or to pin 9 in both polarities (a pull
   resistor that would load the goggle pin in every mode).
9. **True-D only: MOD 1.17 and BAY:RAPIDFIRE, before any powered test.** Out of any
   goggle, on USB only: flash `release/1.17/TrueD-MOD-1.17.hex` with its verify
   (furiousdfw `docs/bench-test.md` step 4), then Settings → BAY = `RAPIDFIRE`. Unplug
   the USB cable. Steps 10-12 then test the firmware that will run in the bay.
   *Stop if* the boot screen does not show "MOD 1.17", or Settings does not show
   `BAY:RAPIDFIRE`.
10. **Bench supply, current-limited.** Set the supply to 5.0 V with the current limit at
    **0.1 A**, output off. Connect − to a ground pin found in step 7 and + to pin 9,
    never to pin 8 or any other pin. Output on, and read the supply's display:
    * the voltage stays below 1.0 V for 2 s with the current at the 0.1 A limit: a short;
    * otherwise (the module may draw more than 0.1 A and hold the supply at its limit at
      a few volts: that is no short) switch off, set the limit to **0.5 A**, switch on:
      expect 4.9-5.1 V. Write down the current.

    (A True-D can instead run from its USB cable alone for steps 11-12, out of any
    goggle, as bench-test.md step 7 does; never USB and the bench supply together.)
    *Stop if* the voltage stays below 1.0 V for 2 s at the 0.1 A limit, or the supply is
    still at its limit with 0.5 A (voltage below 4.75 V).
11. **Powered: do pins 1-3 only listen?** Read only from 2 s after power-on (the
    bootloader's time). For each of pins 1, 2, 3, meter on DC volts between the pin and
    ground: (a) with 10 kΩ from the pin to ground, then (b) with 10 kΩ from the pin to
    pin 9 (the module may stay powered while you move the resistor).

    | reading | means |
    |---|---|
    | (a) below 0.2 V | no pull-up: a plain input (True-D in `BAY:RAPIDFIRE`: expect this) |
    | (a) 0.2-2.0 V | an internal pull-up of 15 kΩ or more (True-D in `BAY:1-8` or `STEP`: 0.8-1.7 V); fine with the resistors |
    | (a) above 2.0 V | an output driving high, or a pull-up below 15 kΩ |
    | (b) within 0.3 V of the supply | not driven low (expected) |
    | (b) below 1.0 V | an output driving low |

    *Stop if* (a) reads above 2.0 V or (b) below 1.0 V on any pin: never wire that
    module; for a True-D it means the wrong firmware or mode.
12. **Bootloader pulse on pin 3.** Supply off. Connect the LED with its 1 kΩ in series
    from pin 3 (the True-D's D13, measured 2026-11-13) to ground (anode toward pin 3).
    Supply on and watch the LED. One flash of
    about 0.1 s: the **stock bootloader** (its pulse on D13; harmless with the resistors,
    and the shifter of path S keeps it from the goggle entirely). No flash: the **no-LED
    bootloader** (furiousdfw `bootloader/`) or none. Supply off, disconnect everything.
    *Stop if* the LED stays lit for more than 1 s (D13 driven for seconds: the 3.9D /
    MOD 1.15 / MOD 1.16 `BAY:AUTO` behaviour, not usable in a wired bay).

### C. Building the mod (goggle off, bay board out of the goggle)

13. **Bay board out; readings before you solder.** Goggle off and unplugged. Take the bay
    board out of the goggle (it plugs into the goggle with its 2×10 connector); touch a
    grounded metal part first, and work on an antistatic or plain wooden surface. Find
    the three connector pins of pkendall64's photos. Meter on ohms; write every value
    down:
    * socket pins 1, 2, 3: to each other and to socket pins 4-9;
    * socket pin 9 to pin 8; socket pin 7 to pin 8 (1 Ω or less: ground; above 1 MΩ:
      not connected);
    * each of the three connector pins: to every pin next to it on the connector (in its
      row and in the other row, diagonals included) and to socket pins 1-9. On a board
      like the open clone all of these read above 1 MΩ.

    *Stop if* any of socket pins 1-3 reads 1 MΩ or less to anything (this board uses
    them, and the mod's assumptions do not hold); pin 9 reads 100 Ω or less to pin 8; or
    one of the three connector pins reads 1 kΩ or less to socket pin 8 or 9 (it is a
    ground or supply pin, not a signal: wrong pin).
14. **Bay mechanics.** With the board out, cover every screw head and exposed solder
    point that faces the module with Kapton tape (section 7). Plan where the wires (and,
    path S, the shifter board) will go: clear of the screws and of where the module
    presses on the board (pkendall64's wiki).
15. **Build.**
    *Path R*: one 4.7 kΩ resistor per line, one lead soldered straight to the connector
    pin's leg, the resistor body within 5 mm of the pin; from its other lead a thin
    wire to socket pin 1, 2 or 3 as in the wiki's photos (the wire labelled Pin 1 to
    socket pin 1, and so on). Insulate the resistors and joints with Kapton; route the
    wires as planned in step 14.
    *Path S*: build the shifter of section 5 on perfboard with a DIP-14 socket, the
    **LM339 not yet inserted**, and without R5 for a 5 V module (True-D). Solder R1
    (4.7 kΩ) at each connector pin as in path R (within 5 mm); a wire from each R1's
    other lead to its comparator's IN+ on the board; a wire from the free end of each
    comparator's R4 to socket pin 1, 2 or 3, keeping the wiki's order (the connector pin
    whose wire goes to socket pin 1 in the photos drives socket pin 1 through its
    comparator; the same for 2 and 3); the board's +5 V to socket pin 9's leg and its
    ground to pin 8's leg. Insulate the board with Kapton and place it as planned.
16. **Check the mod: board still out, no module, ohms.** Every reading of step 13 that
    the table below does not cover must be as it was (within 10 %, or still above 1 MΩ).

    *Path R*:

    | from | to | expect |
    |---|---|---|
    | each connector pin | its own socket pin | 4.45-4.95 kΩ |
    | each connector pin | the two other socket pins of 1-3, socket pins 4-9, its neighbours on the connector | above 1 MΩ (or as in step 13, if it was lower there) |
    | socket pins 1, 2, 3 | each other, and socket pins 4-9 | above 1 MΩ |

    *Path S* (LM339 still out of its socket; values without R5):

    | from | to | expect |
    |---|---|---|
    | each connector pin | its R1's other lead (the comparator's IN+) | 4.45-4.95 kΩ |
    | each connector pin | socket pin 9 | 0.9-1.1 MΩ (R1 + R2) |
    | each connector pin | everything else: the other connector pins, socket pins 1-8 | 0.9 MΩ or more (or as in step 13, if it was lower there) |
    | each of socket pins 1-3 | its comparator's output pad | 0.95-1.05 kΩ (R4) |
    | each of socket pins 1-3 | socket pin 9 | 10.4-11.6 kΩ (R4 + R3); with R5: 5 kΩ or more |
    | socket pins 1, 2, 3 | each other | 20-24 kΩ (two R4 + two R3); with R5: 5 kΩ or more |
    | each of socket pins 1-3 | socket pins 4-8 | 5 kΩ or more |
    | socket pin 9 | socket pin 8 | 5.4-6.0 kΩ (the reference divider) if step 13 read above 1 MΩ there; lower if it did not |

    Path S, then: insert the LM339 (its pin 1 at the socket's notch, board still out) and
    measure socket pin 9 to pin 8 again: it settles over a few seconds (the 100 nF
    charges), typically at a few kΩ.
    *Stop if* any reading is outside its range, or (path S) pin 9 to pin 8 reads below
    1 kΩ with the LM339 in.
17. **Bay board back in.** Goggle off. Board in, screws in; check that no wire is pinched
    and the Kapton is in place. Then, in the socket holes, socket pin 9 to pin 8 (ohms,
    after 5 s). Path S adds its 5.7 kΩ reference divider in parallel to what step 2 read,
    so a lower value than in step 2 is expected there.
    *Stop if* it reads 100 Ω or less; path R: less than half of step 2's value; path S:
    less than half of the smaller of step 2's value and 5.7 kΩ.

### D. Goggle on, bay empty, after the mod

18. **Check Pins again.** Power on: bay empty, Module = None, WiFi off, AnalogRX Power =
    On. Check Pins twice. The voltage must be the one of step 3. Levels: path R: any (the
    wires float); path S: expect "Pins 1 1 1" (R2 pulls each pin up through 1 MΩ; a 0
    means the SoC has a pull-down on that pin: note it).
    *Stop if* the voltage differs from step 3 (1.8 V against 3.3 V).
19. **Idle levels in the socket holes** (Module = None: the goggle drives nothing).
    Meter on DC volts: holes 1, 2, 3 to hole 8, and hole 9 to hole 8.
    *Path R*: each of holes 1-3 between 0 V and 3.6 V (whatever the SoC's input pull
    makes of the unloaded wire).
    *Path S*: each of holes 1-3 at 4.5-5.3 V (the comparator outputs are high; with R5:
    3.0-3.4 V), or at 0.5 V or less where step 18 read a 0.
    *Stop if* hole 9 to 8 is outside 4.75-5.25 V; path R: any of holes 1-3 reads above
    3.6 V (something feeds the wire, such as a bridge to +5 V); path S: any reads between
    0.5 V and 4.5 V (with R5: between 0.5 V and 3.0 V, or above 3.4 V).
20. **The goggle drives, bay empty: order and levels.** Module Bay: Module = **Switch
    (Fat Shark)**, Mode = **Wiring A (standard)** (bit weights PG0 = 1, PG1 = 2, PG2 = 4;
    module-bay.md has the table of all six wirings). Source page: Analog. The Module Bay
    Status line then shows "Sending CH…". Set each channel of the table with the dial
    (turn it to the channel, click to confirm; this needs Input > roller = Switch
    channel, the default), wait 3 s, and measure holes 1, 2, 3 to hole 8:

    | channel | hole 1 (PG0) | hole 2 (PG1) | hole 3 (PG2) |
    |---|---|---|---|
    | CH1 | low | low | low |
    | CH2 | high | low | low |
    | CH3 | low | high | low |
    | CH5 | low | low | high |
    | CH8 | high | high | high |

    Path R: high = 3.0-3.6 V, low = 0.5 V or less. Path S: high = 4.5-5.3 V (with R5:
    3.0-3.4 V), low = 0.5 V or less. Path R with highs of 3.0-3.2 V: within the limits,
    but with a bay supply above 5.0 V (step 2) the True-D's guaranteed margin is gone
    (section 3); it works only by its typical threshold.
    *Stop if* any pattern differs from the table (the wires are crossed: put them in this
    order), or any level is outside its range.
    *Note for a "dumb" module* (RTC6715 pins, section 2), not for a True-D: its low must
    stay at 1.0 V or less against its own pull-ups, which the empty bay cannot show. For
    such a module, between steps 23 and 24: Module = Switch, CH1, a clip lead attached
    with the goggle off on the wire end at socket pin 1, 2 or 3 (path R), one pin per
    boot; *stop if* a low reads above 1.0 V. A True-D in `BAY:RAPIDFIRE` has no pull-ups:
    the empty-bay levels are the levels it sees.
21. **Module None, then WiFi with the mod.** Module = None (Status "Off, bay pins
    untouched"). WiFi on: it must connect within 60 s. Then WiFi off.
    If it does not connect: switch the goggle off and on (WiFi starts at boot, before any
    module control) and wait 60 s. If it connects now, the pins are not handed back to
    WiFi after module control let go of them: a firmware problem; note it, report it,
    from now on reboot before you use WiFi after module control, and go on.
    *Stop if* WiFi does not connect within 60 s after the fresh boot either, although it
    did in step 5: the mod loads the SDIO bus; shorten the stubs at the connector pins to
    under 5 mm, or remove the mod.

### E. The module in the bay

22. **Insert the module.** Goggle off. Check the Kapton on the module's back. True-D:
    MOD 1.17 and `BAY:RAPIDFIRE` (step 9), USB cable unplugged. Insert it. Module Bay is
    still at Module = None.
23. **The module, goggle passive.** Power on. The goggle starts as before, and the
    True-D's screen lights up (the bay is powered: AnalogRX Power = On). Module Bay >
    Check Pins: path R: any levels, "toggling" included (in `BAY:RAPIDFIRE` the True-D
    has no pull-ups, so the lines float); path S: as in step 18.
    Optional, path R, only if you can attach clip leads to both leads of a resistor at
    the connector with the goggle off: goggle off, clips across the pin-1 resistor, meter
    on DC volts; goggle on, read after 5 s; goggle off; the same for the other two
    resistors. Expected: near 0 V.
    *Stop if* the True-D's screen stays dark for more than 5 s after power-on (measure the
    bay supply: module out, goggle on, hole 9 to hole 8), the goggle does not reach its
    menu within 60 s, or (optional reading) more than 0.5 V across a resistor (more than
    0.1 mA flows: something drives or pulls hard).
24. **Module = rapidFIRE.** Module Bay: Module = **rapidFIRE** (leave Mode as it is; the
    True-D ignores it). Source: Analog. The Status line shows "Waiting for the module to
    boot" right after a bay power-on, then "Sending" and the channel. Within 6 s the
    True-D shows the goggle's channel. Change the channel 5 times with the dial, two
    bands included: each time the True-D follows within 2 s. Batch 2 goggles, also: set
    Power > AnalogRX Power = Auto, switch the source to HDZero and back to Analog (the
    bay goes off and on): the Status shows "Waiting for bay power", then "Waiting for the
    module to boot", then "Sending" again, and the True-D boots and gets its channel
    within 6 s; set AnalogRX Power back as you want it.
    (L band: by band and channel the True-D gets the right channel; an ELRS VTX
    frequency on L band ends up 29 MHz off, because the True-D's L1-L8 are 5333-5613 MHz
    and the goggle's table has 5362-5621 MHz.)
    *Stop if* the True-D ever shows a channel the goggle did not send (this must never
    happen: Module = None, write down the sequence, report it), or it misses 3 changes in
    a row (Module = None; then the [hw] items of section 9).
25. **WiFi with module control.** WiFi on: the Status shows "Paused while WiFi is on";
    WiFi connects within 60 s. WiFi off: the Status returns to "Sending" within about
    6 s (the SDIO check runs every 5 s).
    *Stop if* WiFi does not connect within 60 s (it did in step 21: the module loads the
    bus); the WiFi page refuses to start WiFi because the bay pins were not released
    within 3 s (Module = None, report it); or the Status still shows "Paused while WiFi
    is on" or "Paused, pins in use by WiFi" 15 s after WiFi off (reboot with WiFi off,
    report it).
26. **Flying.** WiFi off: module control pauses while WiFi is on, and the module sees the
    SDIO traffic as noise. Insert and remove the module only with the goggle off. Before
    another module goes into the wired bay: Module = None, then steps 6-12 for that
    module, then steps 22-25 with its Module type.

## 9. What the firmware guarantees, and what is unproven

**Goggle firmware** (src/module/, host tests in `utilities/module_bay_test`, 136
checks at 2989f32, all pass):

* Module = None (the default after flashing): no pin is ever touched, through boot,
  power cycles, source changes, WiFi on / off, standby and wake. Exactly stock.
  Check Pins, a click, sets them to GPIO inputs, reads them for 100 ms and never
  drives; afterwards they stay GPIO inputs (function 0), unexported. Whether WiFi then
  gets them back is checked in steps 5 and 21. [analysis, sim]
* Every type: the pins stay inputs until 2.0 s after every bay power-on (and after
  the app start on batch 1 hardware). The time counts from the goggle's power-on
  command to the DM5680 co-processor; the real switch-on follows it after an
  unmeasured delay [hw], inside the 0.55 s margin over the bootloader's 1.45 s.
  [analysis, sim] SteadyView is the one exception by protocol: its mode set
  power-cycles the bay with CLK / DATA driven (70 ms), only with Module =
  SteadyView. Never select it for another module.
* rapidFIRE: ≥ 100 µs between any two CS / CLK edges and DATA changes only while
  CLK is low, whatever the scheduler does: every wait ends only once the clock has
  passed an absolute deadline [analysis]; also under a storm of signals that cut
  every sleep short [sim]. Typical 13.5-14.8 ms per frame on a PC [sim]; on the
  goggle not measured [hw].
* Pins inputs again within one frame after a bay power-off, WiFi on or a Module
  change. [sim] WiFi does not start while module control still holds the pins: the
  WiFi page waits up to 3 s for their release and otherwise refuses. [analysis]

**Unproven, only hardware can settle it** [hw]:

* the Goggle 2 bay routing: whether PG0-PG2 reach its bay connector at all;
* the PG bank voltage, 3.3 V or 1.8 V, on each goggle (Check Pins; the official
  sys_config points to 1.8 V, see the top of this page);
* the official bay board: what it carries on socket pins 1-3 and 7 and on the
  goggle connector, and which connector pin is which PG pin;
* the True-D board's loads on D11-D13 (an LED on D13, pull resistors, protection
  parts, ICSP pads), and that bay pins 1-3 reach D13, D12, D11 in that order;
* whether the WiFi chip is powered while WiFi is off (section 4), and that WiFi
  works with the mod (and with the shifter);
* the shifter of section 5: nothing built; room for it in the bay;
* the goggle's real pin timing, and the DM5680's bay power switching delay.

**Foxeer Wildfire.** Its manual (search-result transcriptions, snippet) lists a
setting **FS SWITCH (ON / OFF)**, so the Fat Shark three-pin code is the likely
interface and **Switch** the likely Module type, absolute or stepped and pin order
unverified; its pins' levels and pull-ups are unknown (d): steps 7-12 first (step 9
does not apply), then in the bay (step 23) Check Pins with FS SWITCH off and on, then
Switch with each wiring, Wiring A to Wiring F (module-bay.md).
Separately, HDZero issue #108 (issue body read) reports a bad picture from a
Wildfire in the HDZero goggle that was fine in an HDO2, cause unknown; it predates
and is unrelated to this mod.
