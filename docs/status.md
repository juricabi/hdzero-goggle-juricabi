# Status of `ripples-all`, and how to continue (2026-11-19)

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

1. **ripples as the base, module bay control**: done.
2. **Live-stream audio and HDMI audio** onto ripples' recorder (`src/record`, which ripples reworked a lot): done.
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

## Phase 2: what was ported, and how each overlap with ripples was resolved

From `all` (082b2f5), everything off by default: the WiFi live stream's AAC track (`src/rtspLive`, the ring,
`src/record/live_audio.*`, WiFi Module > System > **Stream Audio**, docs/live-audio.md) and HDMI audio (the boot
step `hdmi_audio_boot` in `rc.sh` with its boot record, `src/record/hdmi_boot.*`; the IT66121 audio setup with the
Reg04 guard; the IT66021 EDID with 48 kHz L-PCM only; the record daemon's bridge `src/record/pcmbridge.*`; the
shared `/tmp/.sunxi_dump.lock` of phase 1; `src/core/hdmi_audio.*`; the BoxPro included; docs/hdmi-audio.md),
with their host tests and mutants. ripples' Audio page is not touched (phase 3): `[hdmi] audio_out` / `audio_in`
are set by hand in `setting.ini` until then.

| where | ripples has | resolved |
|---|---|---|
| rtspLive, the ring (`src/rtspLive`, `src/record/avshare.*`) | upstream's code, except the writer's `avshare_reset()` (below) | `all`'s code (applied without a conflict; ripples' `avshare_reset()` kept) |
| the recorder's main loop (`src/record/main.c`) | reworked: DVR commands on a control queue of its own (`record_control`, lib/log), `control_paused`, rolling recordings, `rec_dbg_log` | merged by hand: the AI device's users in one list (`aidev_heldByOthers()`: recording, AI->AO loop, stream audio, HDMI bridge) replace ripples' two pairwise checks in `record_stop()` and `AIO_cmdSTOP` (the list covers both); the stream audio's poll ends `live_run()`, after ripples' pause check (while paused, live video is stopped and the audio chain is neither started nor stopped; it uses no VI); the live video's pts through `liveclock_stamp()`; `HDMIA_cmdSTART` / `STOP` on the goggle message queue (ripples' control queue carries DVR commands only); the exit order of `all` |
| the recorder's conf | the app copies `record.conf` once when it starts and hands the recorder edited copies (`src/core/dvr.c`, `RECORD_CONFIG`: [vi], [venc], [record]); the recorder prefers the app's `record.conf` over `/data/confs/record.conf` | the stream audio reads `[live]`, `[ai]`, `[aenc]` from the file the recorder chose at its start (`liveConfFile`), not from the snapshot: otherwise Stream Audio, which the WiFi page writes later, never reached the recorder. Found while porting; the new `t_snapshot.sh` fails 7 of its 9 checks without it |
| the ring's reset (`avshare_reset()`, `src/record/avshare.c`) | flushes every slot of both media when live video stops (a new video bitstream after an encoder restart): also on the pause before a source switch and on the resume after it | kept as ripples has it: the stream audio's chain runs on, and `all`'s reader resynchronises after the flush (it looks for the writer's position); the new `t_pause.sh` pauses and resumes the recorder while a client streams: no audio gap over one frame, in order, video back |
| `disk_checkFile()` (`src/record/disk.c`) | its own fix (`stat()` and `S_ISREG`) | ripples' kept (the same as `all`'s) |
| `src/record/confparser.h` | `rec_dbg_log` | both kept |
| IT66021 (`src/driver/it66021.c`) | EDID DTD #3 1280x720@60, #4 1920x1080@50, block 1 checksum 0xDA; a 300 ms receiver reset | the 48 kHz-only change on ripples' table (the checksum adjusted from 0xDA); `it66021_edid_test.sh` now compares with ripples-base: with audio_in off the I2C sequence and the EDID are ripples' |
| IT66121, I2C (`src/driver/it66121.c`, `i2c.c`) | upstream's | the Reg04 guard and `i2c_read_checked()` as on `all`; `it66121_seq_test.sh` against ripples-base: identical with audio off |
| the emulator's I2C (`src/emulator/i2c_emu.c` instead of `i2c.c`) | every access a no-op | `i2c_read_checked()` added there; it fails (no bus), so the Reg04 check fails safe |
| HDMI-in audio in the codec (`src/core/hdmi_audio.c`) | codec routing of its own: `audio_sel.sh` (`out_on/off`, `out_dac_on/off`, `out_linein_on/off`) from `src/core/dvr.c`; no audio mixer | `all`'s mixer is not ported (phase 3), so `hdmi_audio_line_out()` uses ripples' routing: on, the DAC into the output mixers and on the line out at the **DVR Playback** volume, the line-in out of the mix; off, what ripples' HDMI in leaves (line out off, the DAC out of the mix); set again after a record daemon restart. **Not recorded** (no output mixer -> ADC path) |
| source switches (`src/core/app_state.c`) | `dvr_defer_transition()` first; analog switches its line out on, HDMI in off | `hdmi_audio_apply()` after ripples' own steps: menu (after `IT66121_init()`), analog (before ripples' line out, so ripples' state wins), HDZero, HDMI in (after its line out is off); a no-op unless HDMI audio is in effect this boot |
| WiFi page (`src/ui/page_wifi.c`) | System: Page, Root PW, SSH, Apply Settings | Stream Audio between SSH and Apply Settings, its note, as on `all`; `record.conf` `[live] audio` written only when it differs |
| build, boot (`CMakeLists.txt`, `rc.sh`) | `SRC_FILES_SOC_REG` (phase 1); ripples' `rc.sh` (console log rotation, `seed_adjtime`) | `hdmi_boot.c` with `soc_reg.c` in the app and the record daemon, the `hdmi_audio_boot` program in `/mnt/app/app/`; the boot step right after `config_uart_vdpo`, as on `all` |
| ripples' tests | `test/video` links `app_state.c` | a stand-in `hdmi_audio_apply()` in `test/video/test_camera_mode.c`, next to its other stand-ins; nothing else changed |
| languages | | "Stream Audio" and its note in the five languages (at most 347 of 384 entries) |
| the host tests | | `it66121_seq_test.sh` / `it66021_edid_test.sh` compare with ripples-base 4fbb865 (on `all`: a85841d); `c_mutants.sh` without the module bay's three (they are in `utilities/module_bay_test/mutants.sh` here); `utilities/live_audio_test`'s `record_host` links ripples' `record_control`, `dvr_trace` and `rolling.c` and gets stand-ins for `log_write()` and `disk_availableSize()` |

## Results of phase 2 (all on the PC)

| what | result |
|---|---|
| images, `build_all.sh` (goggle, goggle2, boxpro), Release with `-Werror` | built from 58458ba (the commits after it change only a host test and docs), no compiler warning (the `liblog.so` note only); listed below. Each commit of phase 2 builds (the goggle image) |
| emulator builds (goggle, goggle2, boxpro) | ripples' one warning (`page_version.c`) and no new one |
| emulator start check, sandboxed (`emu_run.sh`), Goggle 1, Goggle 2 and BoxPro, three starts each (no setting.ini, ripples' shipped one, the one ripples wrote at its first start), against ripples' own emulator build | setting.ini after the start and at the end identical to ripples' in all nine; no `[hdmi]` section and no `stream_audio` key written, no HDMI audio log line (nothing in effect by default); no crash, no early exit. Phase 1's Module Bay check (`keys/check_start.txt`, Goggle 1 and 2, three starts each) repeated: as in phase 1 (one Goggle 2 run's keys went to the page of the startup scan, which was still running; that start again, twice: the page opens, Check Pins answers, nothing written) |
| the WiFi page in the emulator (`keys/wifi_stream_audio.txt`, all three targets) | WiFi Module > System shows Stream Audio between SSH and Apply Settings, a click selects On and marks Apply Settings; leaving without applying writes nothing (setting.ini unchanged, no `stream_audio`); the label fits on the BoxPro's layout too |
| ripples' tests (`run_ripples_tests.sh`) | as in the baseline: all pass except `test_dvr_naming`'s build on glibc (`test/video` with the stand-in above); at 58458ba and at dd3f21f |
| `it66121_seq_test.sh`, `it66021_edid_test.sh`, `pcmbridge_test.sh`, `hdmi_boot_test.sh` | 12, 5, 78, 352 pass (as on `all`), the first two against ripples-base; at 58458ba and at dd3f21f |
| `utilities/probe/test/c_mutants.sh` | 21 mutants: 20 killed, 1 equivalent (the Reg04 read-modify-write, as on `all`); `all`'s 24 less the module bay's three, which are in `utilities/module_bay_test/mutants.sh` here |
| `run_live.sh all` (host) | ring buffer 7254 checks native and under qemu-arm, `t_aidev.sh` 8 (`record_host` on ripples' recorder); also at dd3f21f. The rtspLive tests need a host live555 (`LIVE555`) for the native rtspLive, not on this machine (as on `all`): they run on the ARM build, next row |
| the rtspLive tests on the cross-built rtspLive under qemu-arm (`RTSPLIVE=arm`, goggle2 build) | describe 12, share 6, e2e 38, avsync 12, audio_fail 6, the new `t_snapshot.sh` 9 and `t_pause.sh` 7, each at 58458ba and again at dd3f21f (`t_pause.sh` at dd3f21f only); `t_arm.sh` 65 at 58458ba (e2e 38, describe 12, kill 15), at dd3f21f twice e2e 38 and describe 12, kill as below; `t_cpu.sh` only measures (Stream Audio on: 1132 / 1158 wake-ups/s, 3.9 / 4.0 % of a host core; off: 1027 / 1037, 5.3 / 5.1 %); `t_off_identical.sh` needs the native rtspLive (not run, as on `all`). `t_kill.sh` is flaky on this host: of its 9 runs at 58458ba / dd3f21f (alone and inside `t_arm.sh`) 6 passed 15/15, 3 failed one check each: the test measured the audio chain's stop at 4.22, 4.75 and 4.76 s (limit 4.0 s; two while other suites ran, one on an idle machine). In the two whose logs remain, live video stopped as fast as in the passing runs (0.11 s) and the recorder's log puts the chain's stop 3.006 s after it, the same times as in the passing runs (the stop delay as designed): the excess is in when the test's polling sees the log line, not in the recorder. The check is left as it is (Open, 5.) |
| `make -C utilities/module_bay_test test` | 170 pass (15, 17, 91, 3, 44), as in phase 1 (it links `settings.c`, which gained `stream_audio`) |
| `utilities/module_bay_test/mutants.sh` | 16 of 16 killed, as in phase 1 |
| `utilities/module_bay_test/check_strings.py` | 46 strings (the Module Bay page's 42 and the WiFi row's 2 on the goggles' layout, the WiFi row's 2 on the BoxPro's) x 6 languages: translated, in the font, fitting (widest: the Japanese row label on the BoxPro, 125 of 132 px) |
| `run_probe.sh run` / `mutants` | 65 pass / 18 of 18 killed, as in phase 1 (the kit is unchanged; its new C tests are listed above) |

Images of phase 2 (built on the developer's machine, not released):

| image | bytes | SHA-256 |
|---|---|---|
| HDZERO_GOGGLE-77-206-1.2.2-ripples-58458ba.bin | 10055680 | 6353d333a4453059d2877340d7329d9c6360a73b3a7ccc1aa4ff514b7dfa9f26 |
| HDZERO_GOGGLE2-77-198-1.2.2-ripples-58458ba.bin | 12892160 | ac6d2e0b30138304e33fe5878238f4f2f7789d9b3a7b40e69c7b70ae91785324 |
| HDZERO_BOXPRO-77-211-1.2.2-ripples-58458ba.bin | 9748480 | 892c27b69595e656efad0e738d36d970dbe9919367a9360e862d1f4fbc2dc389 |

Not testable without a goggle: everything at a hardware boundary (codec, I2S, IT66121 / IT66021, sunxi_dump, MPP);
docs/live-audio.md section 4 and docs/hdmi-audio.md section 7 say what to try on the device, HDMI audio only after
the read-only probe.

## Build, emulate, test

All on Linux (WSL Ubuntu 24.04, root), worktree on the Linux file system. Scripts: [utilities/dev/](../utilities/dev/README.md).

```sh
utilities/dev/build_all.sh /root/hdz/goggle-ripples goggle goggle2 boxpro   # images + .sha256 -> $OUT
utilities/dev/emu_build.sh goggle                                            # SDL emulator build (also goggle2, boxpro)
utilities/dev/emu_run.sh /root/hdz/emu/build_goggle - $PWD/utilities/dev/keys/check_start.txt g1_none   # always sandboxed
utilities/dev/run_ripples_tests.sh                                           # ripples' test/*, as an unprivileged uid
make -C utilities/module_bay_test test                                       # ~2 min; mutants.sh ~20 min
python3 utilities/module_bay_test/check_strings.py
utilities/dev/run_probe.sh run                                               # probe kit (~10 min); also: mutants (~25 min)
sh utilities/probe/test/it66121_seq_test.sh                                  # and it66021_edid_test.sh, pcmbridge_test.sh, hdmi_boot_test.sh
utilities/probe/test/c_mutants.sh                                            # mutants of the HDMI audio C code (~15 min)
utilities/dev/run_live.sh all                                                # live-stream audio, in private namespaces
RTSPLIVE=arm ARM_RTSPLIVE=<build>/goggle2/rtspLive utilities/dev/run_live.sh t_e2e.sh   # one test on the ARM rtspLive
```

## Open, in this order

1. Phase 3 (the Audio page: ripples' page with its keys and test buttons, extended with `all`'s mixer extras and
   the HDMI Audio rows, which then replace the hand-set `[hdmi]` keys and ripples' codec routing for HDMI-in
   audio), phase 4 (docs, images, review).
2. A decision for the owner: ripples' `test_dvr_naming.c` does not build on glibc; fix it here (it is ripples'
   test) or leave it to ripples.
3. On the goggle (only by its owner, never flashed without asking): everything of docs/module-bay.md, "Untested
   on hardware", now on ripples' firmware; the order of module-bay-mod-safety.md (Check Pins with Module None
   first); Stream Audio (docs/live-audio.md section 4); HDMI audio only after the read-only probe
   (docs/hdmi-audio.md section 7).
4. Known and left as it is: a goggle with a legacy `/data/confs/record.conf` streams without audio (rtspLive reads
   that file, the WiFi page writes the app's; the same on `all`).
5. `utilities/live_audio_test/t_kill.sh` is flaky on this host (3 of 9 runs, results of phase 2): its stop times
   are seen by polling the log on the wall clock. Measuring them from the recorder's own log times would make it
   exact; that changes a check, so it is left for the review (phase 4).
