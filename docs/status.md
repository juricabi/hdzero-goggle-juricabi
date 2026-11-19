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
3. **The Audio page**: ripples' page, its settings keys (`[record] audio`, `audio_source`, `dvr_audio_volume`,
   `live_audio_volume`, `mic_gain`, `linein_gain`) and its test buttons stay; it is extended with the extras of
   `all`'s mixer: done.
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

## Phase 3: the Audio page, and how each overlap with ripples was resolved

ripples' Audio page stays as it was at the top: Record Audio, Audio Source, and Volume Control with DVR Playback,
Live Volume, Mic Gain, Line/AV Gain and the four test buttons. Its keys and test buttons are kept. Below it come the
extras of `all`'s audio mixer (082b2f5) in `all`'s sections: the headphone level of each source (Mic, Line In,
A/V In, HDMI In), the Record switches with Record Headphone Mix, Built-in RX Audio (Goggle 2, BoxPro), and the HDMI
Out / HDMI In Audio rows. These replace phase 2's hand-set `[hdmi]` keys and its `audio_sel.sh` route for HDMI-in
audio. They come with the mixer's worker and watcher and the settings migration. The full description is in
docs/audio.md.

| where | ripples has | resolved |
|---|---|---|
| the page | a fixed grid: Record Audio, Audio Source, "Volume Control:" with four sliders and Run Test, a four-line note | ripples' rows first, in its order and with its keys; `all`'s extras below in `all`'s sections; `all`'s scrolling list (10 rows) with the hint of the selected row below (ripples' note when Run Test is selected, in ripples' font); `MAX_PANELS` 19 |
| codec routing | `dvr.c` + `audio_sel.sh`: a source switch runs `in_*` and the source's gain, then `out_on`, the Live Volume, `out_linein_on`, `out_dac_off`; the recording start selects the source again | `all`'s mixer (one batch per state, a worker, the watcher) sets the whole routing. `dvr_select_audio_source()` / `dvr_enable_line_out()` keep their call sites and apply the mixer; the recording start (ripples' DVR worker) waits for it |
| Audio Source | one source (`in_mic1` / `in_mic2` / `in_linein`); the receiver's audio with A/V In | the preset of `all`'s Record switches (exactly that source directly) and of Built-in RX Audio; Record Headphone Mix stays as it is; a hint when the Record rows differ from it ("Changed in the Recording section below") |
| Live Volume | line-out volume `(v*31+5)/10` and the LINEIN gain by a cubic curve, applied on release; never switches the line out on | the mixer's line-out volume; moving it also sets the A/V In level by ripples' curve (A/V In can be trimmed afterwards); it does not make the headphones live from boot (`phones_at_boot`), as ripples never switched the line out on |
| Mic Gain, Line/AV Gain | the MIC1 preamp (with Mic as the source only); the ADC input gain and the MIC2 preamp (with Line In / A/V In only) | MIC1 = Mic Gain and MIC2 = Line/AV Gain always (each acts on its own input only); ADC input gain = Line/AV Gain whenever more than the mic alone is recorded, else the record daemon's 0 dB, as ripples left it with Mic |
| `all`'s Volume, Playback Volume, Mic Boost, Line In Boost, Record Level | - | not added: ripples' Live Volume, DVR Playback, Mic Gain and Line/AV Gain are the same controls. Their `[mixer]` keys are never written or removed; `headphones`, `playback`, `mic_boost` and `dvr_level` are read for the migration from `all` (Line/AV Gain takes `dvr_level`, the ADC gain both set, not `line_in_boost`) |
| DVR playback | `adec2ao.c`: live audio muted, the DVR Playback volume (line-out table, DAC volume), restored at the end; the file's left DAC channel through libmedia_mpp's route | ripples' code as it is; the mixer held (no batch, no watching) from before `media_init()` to after `media_exit()`, then its state applied. `all`'s playback (both DAC channels at 152) not taken: ripples found the output mixer's DAC path tied to static on the Goggle 1 |
| test buttons | ripples' own routing (`audio_sel.sh`, `dvr_select_audio_source(source)`), its own restore of the source and line out | ripples' test code, with its routing kept in `dvr.c` as `dvr_test_select_audio_source()` / `dvr_test_line_out()`; the mixer held during the test and applied afterwards (instead of ripples' restore) |
| HDMI-in audio | phase 2: `audio_sel.sh` at the DVR Playback volume, not recorded | `all`'s: a live source of the mixer at the HDMI In level, recorded with Record Headphone Mix (`src/core/hdmi_audio.c` as on `all`) |
| HDMI rows | phase 2: `[hdmi]` by hand | `all`'s rows with their Restart / Error marks and fault hints, on every target; off by default; HDMI audio only after the read-only probe, the BoxPro as the goggles (docs/audio.md, docs/hdmi-audio.md) |
| settings | ripples' `[record]` keys | the extras in `[mixer]` with `all`'s keys (version 2), ripples' keys in `[record]`; fresh: ripples' defaults; from ripples 1.2.2: ripples' state, nothing written; from `all`: its `[mixer]` in effect, ripples' values from `all`'s equivalents in memory, nothing written, `all`'s own keys kept; back to ripples: ripples' keys as written; leaving the page without a change writes nothing |
| the built-in receiver's audio | `rtc6715.init(1, audio_source == A/V In)` (`app_state.c`, `ht.c`) | `audio_mixer_rx_audio()` (Built-in RX Audio, preset by the Audio Source) |
| Storage page | - | `audio_mixer_recheck()` after a format or repair (the record daemon's restart resets the codec) |
| translations | ripples' own Audio strings are untranslated in ripples 1.2.2 (shown in English) | left so; `all`'s translations of the extras and one new hint in the five languages; `TRANSLATE_STRING_NUM` 384 -> 512 (ja_jp has 384 entries now) |
| ripples' tests | `test/video` builds `app_state.c`, `test/motion` `ht.c`, `test/record`'s `test_dvr_naming` `dvr.c` | a stand-in for each new symbol they need (`audio_mixer_rx_audio()`, `audio_mixer_set_line_out()`, `audio_mixer_apply_wait()`), next to their other stand-ins. `test/motion` checks that a wake powers the receiver with its audio as ripples' Audio Source says (on with A/V In): its stand-in for Built-in RX Audio is that preset, so the check stays as it is |
| the emulator | no codec, `system_exec()` a no-op | the mixer runs (its amixer call a no-op), the watcher finds no codec and backs off |

## Results of phase 3 (all on the PC)

| what | result |
|---|---|
| images, `build_all.sh` (goggle, goggle2, boxpro), Release with `-Werror` | built from 90627cc (the commit after it changes only docs), no compiler warning; listed below. Each commit of phase 3 builds (the goggle image at 8e192da and ac43c30, no warning). The emulator runs, ripples' tests and the strings ran on the same code before the commits, but for the test hook of the mixer, which only the host test builds |
| emulator builds (goggle, goggle2, boxpro) | ripples' one warning (`page_version.c`) and no new one |
| `utilities/audio_mixer/test_mixer.py` (new on this branch: ripples' `settings.c` + the mixer, three targets) | all passed, Goggle 1, Goggle 2 and BoxPro (about 90 s); at 90627cc |
| `utilities/audio_mixer/mutants.py` | 74 mutants, 74 killed, at 90627cc (6 in parallel, about 45 min). The first full run, at the code before the last test additions, left 3 alive: all's headphones mapped without rounding and all's headphones over ripples' own Live Volume (no setting.ini case told them apart), and hold taken between the worker taking a state and writing it (a race no test reached). Added: two all-firmware cases (headphones 27; ripples' Live Volume from before all's) and a hook of the host test right before a batch (`AUDIO_MIXER_TEST_HOOKS`, not built into the firmware) with the case "hold, taken right before a batch"; each of the three was then killed on its own, and the full run above killed all |
| `utilities/audio_mixer/check_strings.py` | 172 strings (both layouts) x 6 languages: translated (ripples' own nine labels and its note shown in English, as on ripples 1.2.2), in the font, fitting |
| emulator, sandboxed (`emu_run.sh`): start, port vs ripples | Goggle 1, Goggle 2, BoxPro, three starts each (no setting.ini, ripples' shipped one, the one ripples wrote at its first start), against ripples' own emulator build (ripples-base 4fbb865): setting.ini identical after 2 s and at the end, no `[mixer]` written, no early exit or crash; the log shows the mixer taking ripples' values (`from record/audio_source=0 live_audio_volume=10`) |
| emulator: the Audio page, every row (`keys/audio_page.txt`) | all three targets, each from no setting.ini, from ripples' shipped one (its boot scan off, so that the keys reach the menu on the Goggle 1 too) and from one of `all`'s firmware: every row selected in turn and drawn (21 screenshots each; 19 rows on Goggle 2 / BoxPro, 18 on Goggle 1 without Built-in RX Audio; ripples' note under Run Test; HDMI In greyed), leaving the page wrote nothing (0 lines), and the start left `[record]`, `[mixer]` and `[hdmi]` of the given setting.ini as they were |
| emulator: changes on the page (`keys/audio_change.txt`, Goggle 2, BoxPro) | Goggle 2 and BoxPro, from ripples' setting.ini and from `all`'s: each change wrote exactly its keys (Mic: `[mixer] mic`, with `phones_at_boot` and `version=2` the first time; Record Headphone Mix: `rec_mix`; Audio Source: `[record] audio_source` and the Record switches / Built-in RX Audio its preset changes; Live Volume: `live_audio_volume` and `av` by ripples' curve); the DVR test ran ripples' commands and the mixer applied its state afterwards; leaving wrote nothing more |
| ripples' tests (`run_ripples_tests.sh`) | as in the baseline (ripples-base 4fbb865) and phase 2: all pass except `test_dvr_naming`'s build on glibc (in `record` and `record_control`, left to the owner). `test/motion` failed with a first stand-in that ignored the Audio Source; with the preset (above) it passes on all three targets |
| the suites of phases 1 and 2 | `it66121_seq_test.sh` 12, `it66021_edid_test.sh` 5, `pcmbridge_test.sh` 78, `hdmi_boot_test.sh` 352 pass (as in phase 2); `make -C utilities/module_bay_test test` 170 pass (as in phase 2); `utilities/module_bay_test/check_strings.py`: 46 strings x 6 languages fit (the language files grew); `run_probe.sh run`: 65 pass, as in phases 1 and 2. Not rerun, as none of their files changed in phase 3: `c_mutants.sh`, `utilities/module_bay_test/mutants.sh`, `run_probe.sh mutants`, the live-stream suites (`src/record`, `src/rtspLive` untouched) |

Images of phase 3 (built on the developer's machine, not released):

| image | bytes | SHA-256 |
|---|---|---|
| HDZERO_GOGGLE-77-206-1.2.2-ripples-90627cc.bin | 10076160 | 76e203457068790bdfac6371ae9933dc6ab10035391d17e5c2e2da804464ea46 |
| HDZERO_GOGGLE2-77-198-1.2.2-ripples-90627cc.bin | 12912640 | 7956625c9780cabcb044bff52c4b0ca5a6e5c64211ded8e856a972e347a06b5a |
| HDZERO_BOXPRO-77-211-1.2.2-ripples-90627cc.bin | 9768960 | fd755a58e3115d6b8bc53e3e531305e4a98bb83e4af7d3f4c8a52c9650175be9 |

Not testable without a goggle: everything audible (levels, clicks, noise), ripples' playback and tests after the
mixer, the record daemon's reset timing, every HDMI audio path (docs/audio.md, "Testing on a goggle").

## Build, emulate, test

All on Linux (WSL Ubuntu 24.04, root), worktree on the Linux file system. Scripts: [utilities/dev/](../utilities/dev/README.md).

```sh
utilities/dev/build_all.sh /root/hdz/goggle-ripples goggle goggle2 boxpro   # images + .sha256 -> $OUT
utilities/dev/emu_build.sh goggle                                            # SDL emulator build (also goggle2, boxpro)
utilities/dev/emu_run.sh /root/hdz/emu/build_goggle - $PWD/utilities/dev/keys/check_start.txt g1_none   # always sandboxed
utilities/dev/run_ripples_tests.sh                                           # ripples' test/*, as an unprivileged uid
make -C utilities/module_bay_test test                                       # ~2 min; mutants.sh ~20 min
python3 utilities/module_bay_test/check_strings.py
python3 utilities/audio_mixer/test_mixer.py                                  # ~90 s; mutants.py ~45 min, check_strings.py
utilities/dev/run_probe.sh run                                               # probe kit (~10 min); also: mutants (~25 min)
sh utilities/probe/test/it66121_seq_test.sh                                  # and it66021_edid_test.sh, pcmbridge_test.sh, hdmi_boot_test.sh
utilities/probe/test/c_mutants.sh                                            # mutants of the HDMI audio C code (~15 min)
utilities/dev/run_live.sh all                                                # live-stream audio, in private namespaces
RTSPLIVE=arm ARM_RTSPLIVE=<build>/goggle2/rtspLive utilities/dev/run_live.sh t_e2e.sh   # one test on the ARM rtspLive
```

## Open, in this order

1. Phase 4 (docs, images, an independent review).
2. A decision for the owner: ripples' `test_dvr_naming.c` does not build on glibc; fix it here (it is ripples'
   test) or leave it to ripples.
3. On the goggle (only by its owner, never flashed without asking): everything of docs/module-bay.md, "Untested
   on hardware", now on ripples' firmware; the order of module-bay-mod-safety.md (Check Pins with Module None
   first); Stream Audio (docs/live-audio.md section 4); the Audio page (docs/audio.md, "Testing on a goggle":
   ripples' rows, playback and test buttons as before, the extras); HDMI audio only after the read-only probe
   (docs/hdmi-audio.md section 7).
4. Known and left as it is: a goggle with a legacy `/data/confs/record.conf` streams without audio (rtspLive reads
   that file, the WiFi page writes the app's; the same on `all`).
5. `utilities/live_audio_test/t_kill.sh` is flaky on this host (3 of 9 runs, results of phase 2): its stop times
   are seen by polling the log on the wall clock. Measuring them from the recorder's own log times would make it
   exact; that changes a check, so it is left for the review (phase 4).
6. From phase 3, for the owner or the review (docs/audio.md): ripples' own Audio strings (nine labels and the
   test note) are untranslated in ripples 1.2.2 and stay English here; translate them here or leave them to
   ripples. From `all`'s firmware, DVR Playback follows `all`'s `playback` only roughly (`all`'s DAC was 6 dB
   lower). The Live Volume hint says it does not affect recording: true of the line out, but moving Live Volume
   also sets A/V In, which Record Headphone Mix records when it is on.
