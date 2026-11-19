# Status of `ripples-all`, and how to continue (2026-11-16)

`ripples-all` carries this project's additions on top of **ripples' custom firmware** (LVTY-AI/hdzero-goggle,
`main` 4fbb865, "1.2.2-ripples", MIT, by JustinRipley), which the owner prefers as the base. ripples' history is
upstream HDZero's with rewritten hashes (no merge base with upstream a85841d), so the additions are **ported**
from `all` (082b2f5), not merged. **Nothing of it has run on a real goggle.**

## Branches (fork https://github.com/juricabi/hdzero-goggle)

| branch | state |
|---|---|
| `ripples-base` | ripples' `main` 4fbb865, untouched |
| `ripples-all` | `ripples-base` + the port. Work goes here. |
| `all` | the previous integration branch: upstream a85841d + audio mixer, live-stream audio, HDMI audio, module bay (its docs/status.md has the history and the reviews) |

Local worktree on the developer's machine (WSL): `/root/hdz/goggle-ripples` (`ripples-all`).

## Phases

1. **ripples as the base, module bay control** (this page): done.
2. Live-stream audio and HDMI audio onto ripples' recorder (`src/record`, which ripples reworked a lot).
3. The Audio page: ripples' page, its settings keys (`[record] audio`, `audio_source`, `dvr_audio_volume`,
   `live_audio_volume`, `mic_gain`, `linein_gain`) and its test buttons stay; it is extended with the extras of
   `all`'s mixer.
4. Docs, images, an independent review.

## Baseline: ripples 4fbb865 as it is

* **Images**: `utilities/dev/build_all.sh` (the same CMake build as ripples' `setup.sh` / `build_*`, the same
  Bootlin `armv7-eabihf--musl--stable-2018.02-2` toolchain), Release (ripples builds with `-Werror`): all three
  built, no compiler warning, only the linker's old note about `liblog.so` of the prebuilt `libadecoder.so`.

  | image | bytes | SHA-256 |
  |---|---|---|
  | HDZERO_GOGGLE-77-206-1.2.2-ripples-4fbb865.bin | 9973760 | 6179aa8f4345913f6bdf0b04182b77c03195a068bac81a072d41d06df5fa027b |
  | HDZERO_GOGGLE2-77-198-1.2.2-ripples-4fbb865.bin | 12810240 | 0c3d7b8eccd8761cc30475909609370f7c419ee10379dda99504e7f3e4a73194 |
  | HDZERO_BOXPRO-77-211-1.2.2-ripples-4fbb865.bin | 9687040 | d2ead98982b4dea9583f65c8b1e9b578e9c9627ced0f8d24474fe3c88b76bbe9 |
* **Emulator** (goggle, goggle2): builds; one compiler warning each, ripples' own (`page_version.c:404`,
  `-Wformat-truncation`).
* **ripples' tests** (`test/*`, run by `utilities/dev/run_ripples_tests.sh`: each area's `make run`, the three
  targets where a Makefile takes `TARGET`, the two Python safety tests): **all pass, except**
  `make -C test/record run` and `run_record_control`, which also build `test_dvr_naming.c`: it does not compile
  on Ubuntu 24.04 (glibc 2.39), its `#define statfs(path, fs) ...` meets the declaration in `<sys/statfs.h>`
  (ripples' Makefiles have macOS branches; presumably it builds there). The two record tests that build on their
  own (`test_avshare_reset`, `test_record_control`) pass. Left as ripples has it.

## Phase 1: what was ported, and how each overlap with ripples was resolved

From `all` as it is: `src/module/*` (worker, Switch, rapidFIRE, SteadyView, SteadyView X, soft SPI),
`src/ui/page_modulebay.*`, `src/record/soc_reg.*` (the lock of `/sys/class/sunxi_dump`), `mkapp/app/ko/soft_uart.ko`,
`utilities/module_bay_test`, the probe kit's shell part with its suite (`utilities/probe`), `utilities/dev`,
docs/module-bay.md and docs/module-bay-mod-safety.md. Defaults and safety rules as on `all`: Module = None
(the bay pins never touched), the 2.0 s input hold after every bay power-on, rapidFIRE levels ≥ 100 µs, the WiFi
/ SDIO interlock, Check Pins inputs first, "Paused, pin state unknown", the SteadyView CS turnaround.

| where | ripples has | resolved |
|---|---|---|
| bay power | `Analog_Module_Power(bool)` called on events (Source page, Power > AnalogRX Power, Scan Now, wake-up; the sleep entry switches the bay off directly); no power thread; `DM5680_ExternalAnalog_Power(x)` with x = 0 on, 1 off | `DM5680_ExternalAnalog_Power()` (`src/driver/dm5680.c`) reports every command to the module bay (`module_power_changed()`, which ignores a repeat of the bay's state) under a lock that also serialises SteadyView's power cycle (`DM5680_ExternalAnalog_PowerCycle()`, via `external_analog_power_cycle()` in `hardware-goggle*.c`; BoxPro: none). `Analog_Module_Power()` sends its first command at the app start even when the bay is already in the wanted state (ripples sent nothing then), so the module bay learns the power state; otherwise unchanged |
| Goggle 2 analog source | Source > Analog Module Built-in / Expansion, and Auto Detect ("Dual", `auto_protocol_detect`), which always uses the built-in receiver | the bay module is the source with Expansion and not Dual (`module_bay_is_source()`); leaving Dual or switching to Expansion sends the bay module its mode and channel again |
| analog channel: dial, ELRS / MSP, OSD, status bar, Source page | the Expansion module refused (ripples cannot tune it): no channel shown, ELRS / dial ignored, nothing sent over MSP; Goggle 1 analog: no channel at all | ripples' rule kept for an **uncontrolled** bay module (Module None, WiFi on); a bay module under module control is tuned through `src/module/module.h` (`analog_channel_*`), on the Goggle 1 too, with its own list (CH1-CH8 for a Switch module); ripples' Dual / auto-detect paths unchanged (built-in receiver) |
| WiFi | page_wifi.c applies the settings | `module_wifi_changed()` before WiFi starts (pins released within 3 s, else WiFi stays off), as on `all` |
| main menu | 20 entries on the Goggle 2 (`PAGE_PACK_MAX_NUM 20`), scrolling sidebar | Module Bay (goggles) right before Go Sleep!, `PAGE_PACK_MAX_NUM 21`; ripples' scrolling kept (`all`'s chevrons not ported) |
| `aww` / `awr` | `system_exec()`; a no-op in the emulator | `system_exec()` hands `aww` / `awr` to `system_exec_sunxi_dump()` (the shared lock, as on `all`); the analog RSSI `awr` too; the emulator still runs nothing |
| settings | ripples' `settings.c` | `[module] type / setting / channel` as on `all`, read only (nothing written at start-up) |
| languages | 5 files, about 300 entries, table 384 | the 42 module bay entries of `all` added (at most 345 of 384), every string checked |
| UART | 4 ports | port 4 `/dev/ttySOFT0` (SteadyView X), also in the emulator's table |

Not ported now (later phases): the audio mixer, live-stream audio, HDMI audio and their tests, `all`'s main-menu
chevrons. The probe kit's HDMI scripts come along (the kit is one unit with one suite; they do not depend on the
firmware); its C tests and docs/hdmi-audio.md come with HDMI audio.

## Results of phase 1 (all on the PC)

| what | result |
|---|---|
| images, `build_all.sh` (goggle, goggle2, boxpro), Release with `-Werror` | built from 34fa379 (the commit after it adds only this table's image list), no compiler warning (the `liblog.so` note only); listed below |
| emulator builds (goggle, goggle2) | ripples' one warning (`page_version.c`) and no new one |
| emulator start check, sandboxed (`emu_run.sh`, `keys/check_start.txt`), Goggle 1 and 2, three starts each (no setting.ini, ripples' shipped setting.ini, the setting.ini ripples wrote at its first start), against ripples' own emulator build | setting.ini after the start identical to ripples' in all six; the Module Bay page opens (Module None, Mode greyed, "Status: Off, bay pins untouched"), Check Pins answers "Not read: no GPIO access" (no GPIO in the sandbox); opening the page and leaving it writes nothing, no `[module]` section; no crash |
| ripples' tests (`run_ripples_tests.sh`) | as in the baseline: all pass except `test_dvr_naming`'s build on glibc |
| `make -C utilities/module_bay_test test` | 170 pass (15, 17, 91, 3, 44): `all`'s 166, adapted to ripples' `settings.c` (two stand-ins), plus 4: Dual uses the built-in receiver (3), a repeated bay power command sets nothing up again (1) |
| `utilities/module_bay_test/mutants.sh` | 16 of 16 killed: `all`'s 11, `all`'s three module.c mutants of `c_mutants.sh` (the lock, two of three reads, the status), and two for ripples (a repeated power report sets the module up again; Dual counted as the bay being the source) |
| `utilities/module_bay_test/check_strings.py` | 42 strings x 6 languages: translated, in the font, fitting |
| `run_probe.sh run` / `mutants` | 65 pass / 18 of 18 killed (the same kit and suite as on `all`) |

Images of phase 1 (built on the developer's machine, not released):

| image | bytes | SHA-256 |
|---|---|---|
| HDZERO_GOGGLE-77-206-1.2.2-ripples-34fa379.bin | 10004480 | dedda956641c3903ceb162ebfc9a070ef6d9f3b68aec1415070f591682dba5f5 |
| HDZERO_GOGGLE2-77-198-1.2.2-ripples-34fa379.bin | 12840960 | e3013bd8b310e37dd2a94578b8b677d6ffd47cb4ef826e9de6c75b227515904b |
| HDZERO_BOXPRO-77-211-1.2.2-ripples-34fa379.bin | 9697280 | 77fdb0456af20d57c84e1836be0189311a521d2b179a73e067d20bfc4b88551a |

Everything at a hardware boundary (sysfs GPIO, sunxi_dump, the DM5680) is a model written by the same engineers;
nothing here replaces the goggle.

## Build, emulate, test

All on Linux (WSL Ubuntu 24.04, root), worktree on the Linux file system. Scripts: [utilities/dev/](../utilities/dev/README.md).

```sh
utilities/dev/build_all.sh /root/hdz/goggle-ripples goggle goggle2 boxpro   # images + .sha256 -> $OUT
utilities/dev/emu_build.sh goggle                                            # SDL emulator build (also goggle2)
utilities/dev/emu_run.sh /root/hdz/emu/build_goggle - $PWD/utilities/dev/keys/check_start.txt g1_none   # always sandboxed
utilities/dev/run_ripples_tests.sh                                           # ripples' test/*, as an unprivileged uid
make -C utilities/module_bay_test test                                       # ~2 min; mutants.sh ~20 min
python3 utilities/module_bay_test/check_strings.py
utilities/dev/run_probe.sh run                                               # probe kit (~10 min); also: mutants (~25 min)
```

## Open, in this order

1. Phase 2 (live-stream audio, HDMI audio), phase 3 (the Audio page), phase 4 (docs, images, review).
2. A decision for the owner: ripples' `test_dvr_naming.c` does not build on glibc; fix it here (it is ripples'
   test) or leave it to ripples.
3. On the goggle (only by its owner, never flashed without asking): everything of docs/module-bay.md, "Untested
   on hardware", now on ripples' firmware; the order of module-bay-mod-safety.md (Check Pins with Module None
   first).
