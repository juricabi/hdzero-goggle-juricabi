# Status of juricabi's firmware, and how to continue (2026-11-20)

juricabi's firmware (version `1.2.2-juricabi.2`) carries this project's additions on top of **ripples' custom
firmware 1.2.2** (LVTY-AI/hdzero-goggle, the release: tag `ripples-v1.2.2`, 3f0461f, "1.2.2-ripples", MIT, by
JustinRipley), which the owner prefers as the base. ripples' history is upstream HDZero's with rewritten hashes (no
merge base with upstream a85841d), so the additions are **ported** from `all` (082b2f5), not merged.
**juricabi.2 runs on the owner's Goggle 1** (the final image, flashed on 2026-11-20; the owner's verdict: all
good). Section "juricabi.2" has what was found there, what was checked and what was not. The Goggle 2 and BoxPro
images are built from the same code and have not run on hardware.

History: until 2026-11-19 this work was called ripples-all, on a branch of that name in juricabi/hdzero-goggle;
since then it is juricabi's firmware, https://github.com/juricabi/hdzero-goggle-juricabi. Phases 1 to 4 and
juricabi.1 were built on ripples' `main` as it was then (4fbb865, 30 commits after the 1.2.2 release, unreleased);
juricabi.2 moved the same commits onto the release. The sections of phases 1 to 4 below record that work as it was
done, on 4fbb865.

## Branches (https://github.com/juricabi/hdzero-goggle-juricabi)

| branch | state |
|---|---|
| `upstream` | ripples' 1.2.2 release (3f0461f), untouched (until juricabi.2: ripples' `main` 4fbb865) |
| `main` | `upstream` + the port: juricabi's firmware. Work goes here. Rewritten for juricabi.2 (rebased onto the release) |
| `juricabi.1` | `main` as it was for juricabi.1 (04c4651, on 4fbb865), kept. Tag `v1.2.2-juricabi.1` (c8d8072) is on it |

`all`, the previous integration branch (upstream HDZero a85841d + audio mixer, live-stream audio, HDMI audio,
module bay; its docs/status.md has the history and the reviews), stays in juricabi/hdzero-goggle.

Local worktree on the developer's machine (WSL): `/root/hdz/goggle-ripples` (branch `juricabi-122`, pushed as
`main`; the local branch `juricabi` there is juricabi.1's).

## juricabi.2

What the owner found with juricabi.1 and the test images on the Goggle 1, the cause of each, and what was done:

* **The yellow "DVR: waiting for recorder to finish"** at every menu press, source and channel switch.
  Cause: ripples' deferred DVR transitions, which came with ripples' `main` after the 1.2.2 release (e41a0c9 and
  the fixes after it) and were in juricabi.1's base; ripples' 1.2.2 shows no such text. **The base is now the
  1.2.2 release**: the 34 commits of juricabi.1 were rebased onto 3f0461f. Conflicts only where ripples' `main`
  had changed the same lines (`elrs.c`, `input_device.c`, `src/record/main.c`, `app_state.c`, `thread.c`,
  `dvr.c`). Of ripples' 30 commits after 1.2.2, only the **security fix** 2821180 ("install updates from the SD
  card only") is taken, cherry-picked. The others wait for ripples' next release.
* **Every DVR stop took about 2 s** in juricabi.1. Cause: with nothing routed into the ADC, DAPM powers the ADC
  down while the record daemon captures, and stopping waited for the capture's timeout. The mixer keeps the mic
  on the ADC at the minimum ADC volume when nothing is recorded (docs/audio.md, "The ADC always has a source").
* **The DVR over the ELRS backpack "works weird", lags, misses** (docs/dvr.md has all of it):
  * Cause in the goggle firmware: ripples' `dvr_cmd()` slept 2 s after every start and stop, in the backpack's
    reader thread, so commands queued; a menu dropped a start; Record Mode Auto restarted after a disarm. Now
    `src/core/dvr_auto.c` follows the backpack's last word from the peripheral thread, and the goggle waits for
    the recorder's status instead of 2 s.
  * Cause **outside the goggle**: about 1 switch change in 4 never reached the goggle's backpack chip, and the
    rest came on a 256.0 ms grid: the ELRS transmitter's telemetry cycle with Telem Ratio Std at 250 Hz. The
    backpack's packet got through only while the transmitter listened. **Telem Ratio 1:16 or 1:32 on the
    radio**: with 1:16, 24 of 24 arrived; 1:32 works as well, also at 1 W, while 1:64 loses commands at 100 mW
    too (the owner's tests with the unchanged backpack 1.5.9). The spacing of the telemetry slots decides it,
    not the transmit power. Found with a debug build of backpack 1.5.9 whose log goes into `HDZGOGGLE.log`;
    ruled out on the way: head tracking, the goggle's WiFi, distance, WiFi power save on the chip.
* **The DVR started and stopped every second while armed** (one boot). Cause: the SD card's filesystem was
  damaged (below), the recorder could not write, every start failed, and the armed DVR retried every second.
  A start that does not bring the recorder to "recording" now counts as failed (logged with the reason), and
  the retries back off: 1 s, 5 s, then every 15 s.
* **The SD card's filesystem was damaged** (chkdsk: the last clip 0 bytes, its data as lost chains). Cause:
  ripples' recorder closed a clip without syncing; the clip's end and directory entry reached the card only
  with the app's next start, so a card pulled within about half a minute of a stop lost the clip (the card was
  swapped about ten times that evening). The recorder syncs after closing a clip, the app after a stop.
* **`dvr_dbg.log` grew without end** (ripples' trace, always on, an fsync per line). It follows Storage,
  Logging now and is kept under 512 KB with one older copy.
* **After Clear DVR Folder the clips stayed in Playback and opened as "Bad file" until a restart** (seen by the
  owner more than once, on older firmware too). Playback reads its list from the folder each time it is opened,
  so the clips had not been removed. The likely cause, not reproduced: a card whose filesystem the kernel found
  damaged (the item above) is read-only until the next start, `rm` removes nothing, and the page said "Done"
  all the same. Clear DVR Folder now says FAILED and logs it when entries are left. Playback also drops the
  thumbnail copies of removed clips from `/tmp` each time it reads the folder: after a clear the clip numbers
  start again, and a new clip could have shown a removed clip's picture.
* **The Audio page was confusing** (ripples' Audio Source and the Record switches were the same setting, Live
  Volume moved A/V In, Record Headphone Mix greyed out what it heard): one row per input with its Headphones
  level and Record switch, nothing linked; Record Level of its own; Record Headphone Mix gone; a juricabi.1
  setting.ini taken over (docs/audio.md).
* **Analog showed no picture** with the owner's Raspberry Pi transmitter: the same on ripples 1.2.2, not this
  firmware. **Channel control on analog** works only with the module bay under control (3-wire mod, Module =
  rapidFIRE, WiFi off), as ripples does on the Goggle 1.

**On the owner's Goggle 1**: test images e2d88c7 and 9a6ee17 (the new Audio page; the DVR following the switch,
exact delays, no fixed 2 s wait; with Telem Ratio 1:16 or 1:32 no command lost), then on 2026-11-20 the final
image (the tag `v1.2.2-juricabi.2`, 1732881), with the owner's verdict "all good". **Not checked one by one on
a goggle** (no log of the final image was read): a failed start and the retry back-off, the start waiting for an
unfinished stop, the sync after a clip (a card pulled right after a stop), `dvr_dbg.log` following Logging,
Clear DVR Folder's check and the thumbnail copies.

Tests adapted to the 1.2.2 base: the live-stream audio suite's `t_pause.sh` stops the live encoding with the goggle
message 1.2.2's app sends (`LIVE_cmdSTOP`) instead of ripples' newer control queue; `t_snapshot.sh` and `ctl_conf`
are gone (1.2.2's recorder runs on no config snapshot); the HDMI C tests compare with 3f0461f; `test_mixer.py`
models the juricabi.2 page.

### Results of juricabi.2 (all on the PC)

| check | result |
|---|---|
| ripples' tests (`utilities/dev/run_ripples_tests.sh`) | battery, framebuffer, hdmi x3, ht, motion x3, screen, sleep, video, both safety tests, `record_avshare_only`: rc 0. `record`: rc 2, `test_dvr_naming` does not build on glibc (the same at ripples' own 1.2.2 tag) |
| `test/dvr` (the automatic DVR's decisions) | passes; 25 of 25 mutants of `dvr_auto.c` caught |
| `utilities/module_bay_test` | 44 checks, all passed; `check_strings.py`: 46 strings x 6 languages fit |
| `utilities/audio_mixer/test_mixer.py` | all passed (3 targets); `mutants.py`: 94 of 94 caught (93 in the full run; the 94th, "[record] not brought in line with all's values", after a check added for it: run alone) |
| probe kit (`utilities/dev/run_probe.sh run`) | rc 0 |
| `utilities/dev/check_version.py` | all passed |
| live-stream audio (`utilities/dev/run_live.sh all`) | all passed (builds `src/record/main.c` with the sync after a clip) |
| HDMI C tests (`it66121_seq_test.sh`, `it66021_edid_test.sh`, `pcmbridge_test.sh`, `hdmi_boot_test.sh`) | not run again on the 1.2.2 base (those drivers are the same in 3f0461f and 4fbb865) |
| emulator | the Audio page of e2d88c7 looked through (key scripts); not run again after it |
| the waits for the recorder in `dvr.c`, the sync after a clip, `dvr_dbg.log` following Logging | no host test; in the final image on the owner's Goggle 1, not checked one by one |
| at the tag (1732881: Clear DVR Folder's check, the thumbnail copies) | ripples' tests as above; the counting function, cut from the source, against a folder with clips, an empty one, one with only a dot file and a missing one; the emulator starts and walks the menu (Goggle 1); the three images build with no compiler warning. The other suites ran at 1965cc6 and do not cover the two files changed. A read-only card cannot be made on the PC |

## Phases

1. **ripples as the base, module bay control**: done.
2. **Live-stream audio and HDMI audio** onto ripples' recorder (`src/record`, which ripples reworked a lot): done.
3. **The Audio page**: ripples' page, its settings keys (`[record] audio`, `audio_source`, `dvr_audio_volume`,
   `live_audio_volume`, `mic_gain`, `linein_gain`) and its test buttons stay; it is extended with the extras of
   `all`'s mixer: done.
4. **Docs, images, an independent review**: the review of `4fbb865..c3e0860` (ripples 1.2.2 to the end of phase 3) and the fixes of its
   findings: done (below).

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
| the ring's reset (`avshare_reset()`, `src/record/avshare.c`) | flushes every slot of both media when live video stops (a new video bitstream after an encoder restart): also on the pause before a source switch and on the resume after it | kept as ripples has it in phase 2 (the stream audio's chain runs on, `all`'s reader resynchronises after the flush); since phase 4 the video side only (finding 6 of the review): `t_pause.sh` pauses and resumes the recorder while a client streams: no audio frame lost (no pts gap over 32 ms, 1.5 AAC frames), in order, video back |
| `disk_checkFile()` (`src/record/disk.c`) | its own fix (`stat()` and `S_ISREG`) | ripples' kept (the same as `all`'s) |
| `src/record/confparser.h` | `rec_dbg_log` | both kept |
| IT66021 (`src/driver/it66021.c`) | EDID DTD #3 1280x720@60, #4 1920x1080@50, block 1 checksum 0xDA; a 300 ms receiver reset | the 48 kHz-only change on ripples' table (the checksum adjusted from 0xDA); `it66021_edid_test.sh` now compares with ripples 1.2.2 (4fbb865): with audio_in off the I2C sequence and the EDID are ripples' |
| IT66121, I2C (`src/driver/it66121.c`, `i2c.c`) | upstream's | the Reg04 guard and `i2c_read_checked()` as on `all`; `it66121_seq_test.sh` against ripples 1.2.2 (4fbb865): identical with audio off |
| the emulator's I2C (`src/emulator/i2c_emu.c` instead of `i2c.c`) | every access a no-op | `i2c_read_checked()` added there; it fails (no bus), so the Reg04 check fails safe |
| HDMI-in audio in the codec (`src/core/hdmi_audio.c`) | codec routing of its own: `audio_sel.sh` (`out_on/off`, `out_dac_on/off`, `out_linein_on/off`) from `src/core/dvr.c`; no audio mixer | `all`'s mixer is not ported (phase 3), so `hdmi_audio_line_out()` uses ripples' routing: on, the DAC into the output mixers and on the line out at the **DVR Playback** volume, the line-in out of the mix; off, what ripples' HDMI in leaves (line out off, the DAC out of the mix); set again after a record daemon restart. **Not recorded** (no output mixer -> ADC path) |
| source switches (`src/core/app_state.c`) | `dvr_defer_transition()` first; analog switches its line out on, HDMI in off | `hdmi_audio_apply()` after ripples' own steps: menu (after `IT66121_init()`), analog (before ripples' line out, so ripples' state wins), HDZero, HDMI in (after its line out is off); a no-op unless HDMI audio is in effect this boot |
| WiFi page (`src/ui/page_wifi.c`) | System: Page, Root PW, SSH, Apply Settings | Stream Audio between SSH and Apply Settings, its note, as on `all`; `record.conf` `[live] audio` written only when it differs |
| build, boot (`CMakeLists.txt`, `rc.sh`) | `SRC_FILES_SOC_REG` (phase 1); ripples' `rc.sh` (console log rotation, `seed_adjtime`) | `hdmi_boot.c` with `soc_reg.c` in the app and the record daemon, the `hdmi_audio_boot` program in `/mnt/app/app/`; the boot step right after `config_uart_vdpo`, as on `all` |
| ripples' tests | `test/video` links `app_state.c` | a stand-in `hdmi_audio_apply()` in `test/video/test_camera_mode.c`, next to its other stand-ins; nothing else changed |
| languages | | "Stream Audio" and its note in the five languages (at most 347 of 384 entries) |
| the host tests | | `it66121_seq_test.sh` / `it66021_edid_test.sh` compare with ripples 1.2.2 (4fbb865) (on `all`: a85841d); `c_mutants.sh` without the module bay's three (they are in `utilities/module_bay_test/mutants.sh` here); `utilities/live_audio_test`'s `record_host` links ripples' `record_control`, `dvr_trace` and `rolling.c` and gets stand-ins for `log_write()` and `disk_availableSize()` |

## Results of phase 2 (all on the PC)

| what | result |
|---|---|
| images, `build_all.sh` (goggle, goggle2, boxpro), Release with `-Werror` | built from 58458ba (the commits after it change only a host test and docs), no compiler warning (the `liblog.so` note only); listed below. Each commit of phase 2 builds (the goggle image) |
| emulator builds (goggle, goggle2, boxpro) | ripples' one warning (`page_version.c`) and no new one |
| emulator start check, sandboxed (`emu_run.sh`), Goggle 1, Goggle 2 and BoxPro, three starts each (no setting.ini, ripples' shipped one, the one ripples wrote at its first start), against ripples' own emulator build | setting.ini after the start and at the end identical to ripples' in all nine; no `[hdmi]` section and no `stream_audio` key written, no HDMI audio log line (nothing in effect by default); no crash, no early exit. Phase 1's Module Bay check (`keys/check_start.txt`, Goggle 1 and 2, three starts each) repeated: as in phase 1 (one Goggle 2 run's keys went to the page of the startup scan, which was still running; that start again, twice: the page opens, Check Pins answers, nothing written) |
| the WiFi page in the emulator (`keys/wifi_stream_audio.txt`, all three targets) | WiFi Module > System shows Stream Audio between SSH and Apply Settings, a click selects On and marks Apply Settings; leaving without applying writes nothing (setting.ini unchanged, no `stream_audio`); the label fits on the BoxPro's layout too |
| ripples' tests (`run_ripples_tests.sh`) | as in the baseline: all pass except `test_dvr_naming`'s build on glibc (`test/video` with the stand-in above); at 58458ba and at dd3f21f |
| `it66121_seq_test.sh`, `it66021_edid_test.sh`, `pcmbridge_test.sh`, `hdmi_boot_test.sh` | 12, 5, 78, 352 pass (as on `all`), the first two against ripples 1.2.2 (4fbb865); at 58458ba and at dd3f21f |
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
| DVR playback | `adec2ao.c`: live audio muted, the DVR Playback volume (line-out table, DAC volume), restored at the end; the file's left DAC channel through libmedia_mpp's route | ripples' code as it is; the mixer held (no batch, no watching) from before `media_init()` to after `media_exit()`, then its state applied. Since phase 4 the first hold takes the mixer's own live paths out first, and the AIF1 DAC timeslot volumes are the mixer's (findings 1 and 4). `all`'s playback (both DAC channels at 152) not taken: ripples found the output mixer's DAC path tied to static on the Goggle 1 |
| test buttons | ripples' own routing (`audio_sel.sh`, `dvr_select_audio_source(source)`), its own restore of the source and line out | ripples' test code, with its routing kept in `dvr.c` as `dvr_test_select_audio_source()` / `dvr_test_line_out()`; the mixer held during the test and applied afterwards (instead of ripples' restore) |
| HDMI-in audio | phase 2: `audio_sel.sh` at the DVR Playback volume, not recorded | `all`'s: a live source of the mixer at the HDMI In level, recorded with Record Headphone Mix (`src/core/hdmi_audio.c` as on `all`) |
| HDMI rows | phase 2: `[hdmi]` by hand | `all`'s rows with their Restart / Error marks and fault hints, on every target; off by default; HDMI audio only after the read-only probe, the BoxPro as the goggles (docs/audio.md, docs/hdmi-audio.md) |
| settings | ripples' `[record]` keys | the extras in `[mixer]` with `all`'s keys (version 2), ripples' keys in `[record]`; fresh: ripples' defaults; from ripples 1.2.2: ripples' state, nothing written; from `all`: its `[mixer]` in effect, ripples' values from `all`'s equivalents in memory, nothing written, `all`'s own keys kept; back to ripples: ripples' keys as written; leaving the page without a change writes nothing |
| the built-in receiver's audio | `rtc6715.init(1, audio_source == A/V In)` (`app_state.c`, `ht.c`) | `audio_mixer_rx_audio()` (Built-in RX Audio, preset by the Audio Source) |
| Storage page | - | `audio_mixer_recheck()` after a format or repair (the record daemon's restart resets the codec) |
| translations | ripples' own Audio strings are untranslated in ripples 1.2.2 (shown in English) | left so in phase 3; translated in phase 4 (the note line by line); `all`'s translations of the extras and the new hints in the five languages; `TRANSLATE_STRING_NUM` 384 -> 512 (ja_jp has 400 entries since phase 4) |
| ripples' tests | `test/video` builds `app_state.c`, `test/motion` `ht.c`, `test/record`'s `test_dvr_naming` `dvr.c` | a stand-in for each new symbol they need (`audio_mixer_rx_audio()`, `audio_mixer_set_line_out()`, `audio_mixer_apply_wait()`), next to their other stand-ins. `test/motion` checks that a wake powers the receiver with its audio; since phase 4 (finding 8) the test sets Built-in RX Audio against the Audio Source's preset and expects the mixer's value |
| the emulator | no codec, `system_exec()` a no-op | the mixer runs (its amixer call a no-op), the watcher finds no codec and backs off |

## Results of phase 3 (all on the PC)

| what | result |
|---|---|
| images, `build_all.sh` (goggle, goggle2, boxpro), Release with `-Werror` | built from 90627cc (the commit after it changes only docs), no compiler warning; listed below. Each commit of phase 3 builds (the goggle image at 8e192da and ac43c30, no warning). The emulator runs, ripples' tests and the strings ran on the same code before the commits, but for the test hook of the mixer, which only the host test builds |
| emulator builds (goggle, goggle2, boxpro) | ripples' one warning (`page_version.c`) and no new one |
| `utilities/audio_mixer/test_mixer.py` (new on this branch: ripples' `settings.c` + the mixer, three targets) | all passed, Goggle 1, Goggle 2 and BoxPro (about 90 s); at 90627cc |
| `utilities/audio_mixer/mutants.py` | 74 mutants, 74 killed, at 90627cc (6 in parallel, about 45 min). The first full run, at the code before the last test additions, left 3 alive: all's headphones mapped without rounding and all's headphones over ripples' own Live Volume (no setting.ini case told them apart), and hold taken between the worker taking a state and writing it (a race no test reached). Added: two all-firmware cases (headphones 27; ripples' Live Volume from before all's) and a hook of the host test right before a batch (`AUDIO_MIXER_TEST_HOOKS`, not built into the firmware) with the case "hold, taken right before a batch"; each of the three was then killed on its own, and the full run above killed all |
| `utilities/audio_mixer/check_strings.py` | 172 strings (both layouts) x 6 languages: translated (ripples' own nine labels and its note shown in English, as on ripples 1.2.2), in the font, fitting |
| emulator, sandboxed (`emu_run.sh`): start, port vs ripples | Goggle 1, Goggle 2, BoxPro, three starts each (no setting.ini, ripples' shipped one, the one ripples wrote at its first start), against ripples' own emulator build (ripples 1.2.2 (4fbb865)): setting.ini identical after 2 s and at the end, no `[mixer]` written, no early exit or crash; the log shows the mixer taking ripples' values (`from record/audio_source=0 live_audio_volume=10`) |
| emulator: the Audio page, every row (`keys/audio_page.txt`) | all three targets, each from no setting.ini, from ripples' shipped one (its boot scan off, so that the keys reach the menu on the Goggle 1 too) and from one of `all`'s firmware: every row selected in turn and drawn (21 screenshots each; 19 rows on Goggle 2 / BoxPro, 18 on Goggle 1 without Built-in RX Audio; ripples' note under Run Test; HDMI In greyed), leaving the page wrote nothing (0 lines), and the start left `[record]`, `[mixer]` and `[hdmi]` of the given setting.ini as they were |
| emulator: changes on the page (`keys/audio_change.txt`, Goggle 2, BoxPro) | Goggle 2 and BoxPro, from ripples' setting.ini and from `all`'s: each change wrote exactly its keys (Mic: `[mixer] mic`, with `phones_at_boot` and `version=2` the first time; Record Headphone Mix: `rec_mix`; Audio Source: `[record] audio_source` and the Record switches / Built-in RX Audio its preset changes; Live Volume: `live_audio_volume` and `av` by ripples' curve); the DVR test ran ripples' commands and the mixer applied its state afterwards; leaving wrote nothing more |
| ripples' tests (`run_ripples_tests.sh`) | as in the baseline (ripples 1.2.2 (4fbb865)) and phase 2: all pass except `test_dvr_naming`'s build on glibc (in `record` and `record_control`, left to the owner). `test/motion` failed with a first stand-in that ignored the Audio Source; with the preset (above) it passes on all three targets |
| the suites of phases 1 and 2 | `it66121_seq_test.sh` 12, `it66021_edid_test.sh` 5, `pcmbridge_test.sh` 78, `hdmi_boot_test.sh` 352 pass (as in phase 2); `make -C utilities/module_bay_test test` 170 pass (as in phase 2); `utilities/module_bay_test/check_strings.py`: 46 strings x 6 languages fit (the language files grew); `run_probe.sh run`: 65 pass, as in phases 1 and 2. Not rerun, as none of their files changed in phase 3: `c_mutants.sh`, `utilities/module_bay_test/mutants.sh`, `run_probe.sh mutants`, the live-stream suites (`src/record`, `src/rtspLive` untouched) |

Images of phase 3 (built on the developer's machine, not released):

| image | bytes | SHA-256 |
|---|---|---|
| HDZERO_GOGGLE-77-206-1.2.2-ripples-90627cc.bin | 10076160 | 76e203457068790bdfac6371ae9933dc6ab10035391d17e5c2e2da804464ea46 |
| HDZERO_GOGGLE2-77-198-1.2.2-ripples-90627cc.bin | 12912640 | 7956625c9780cabcb044bff52c4b0ca5a6e5c64211ded8e856a972e347a06b5a |
| HDZERO_BOXPRO-77-211-1.2.2-ripples-90627cc.bin | 9768960 | fd755a58e3115d6b8bc53e3e531305e4a98bb83e4af7d3f4c8a52c9650175be9 |

Not testable without a goggle: everything audible (levels, clicks, noise), ripples' playback and tests after the
mixer, the record daemon's reset timing, every HDMI audio path (docs/audio.md, "Testing on a goggle").

## Phase 4: the review of phase 3, and what was done

An independent review went through `4fbb865..c3e0860` (its report and scenarios:
`/root/hdz/review-ripples/` on the developer's machine). Each finding was first confirmed on c3e0860 here, then
fixed with a test that fails on the reviewed behaviour and passes after the fix, and mutants where they fit.

| finding | confirmed on c3e0860 | done |
|---|---|---|
| 1. Mic / Line In heard during DVR playback and ripples' test path (medium) | yes (harness, Goggle 2): Mic 4, Line In 3, line out on, Record Headphone Mix on; after libmedia_mpp's reset and `adec2ao_start()`'s commands the file was heard with the mic and Line In (MIC1 / MIC2 Booststage switches 1, Lineout 1) | the first hold writes a small batch first: the output mixers' mic, Line In, A/V In and DAC switches and the output mixers' switches into the ADC off (`audio_mixer_build_held_commands()`); the release's batch sets them again. `test_mixer.py` `test_hold`: only the DAC heard with the file, nothing recorded through the mix, the state back after the release; mutants: no held batch, the mic / Line In / the recorded mix left in it. docs/audio.md "DVR playback: as on ripples" holds now with the extras set |
| 2. Settings changed in stock ripples overridden on the way back (medium) | yes (harness, three starts on one setting.ini): Line In on juricabi's firmware, then Mic in stock ripples: juricabi's firmware recorded Line In (`REC 1`, `rec_line_in 1`) while setting.ini said `audio_source=0`; A/V In in ripples: Line In and A/V In recorded (`PRESET 0`); Live Volume 3 on juricabi's firmware, 10 in ripples: A/V In stayed at 2 | every `[mixer]` written holds the `[record]` values its values belong to (`record_source`, `record_live`); at the start a `[record]` value changed since re-derives the Audio Source's preset (Record switches, Built-in RX Audio) or the A/V In level, in memory, written with the next change; nothing written at the start. The new test found one more loss, fixed with it: a `[mixer]` field missing from setting.ini followed a changed `[record]` value at the next start (Audio Source Line In, then Record Mic Yes: off again); such a field is now written with the change. `test_mixer.py`: six round-trip cases (each: values in effect, nothing written at the start, `g_setting.record` as the page, the same after the next change and a restart, `[record]` left as ripples wrote it), a restart check after a change in a setting.ini of all's firmware; 8 mutants |
| 3. HDMI Out Audio one click away without the probe (medium, hardware risk) | yes, by reading: the row on every target, the boot step checks only `audio_out` exactly 1 and PH_CFG0 = stock; nothing ties it to the probe | as decided: the read-only probe gives HDMI Out Audio a verdict from what it reads (`snddaudio0`; PH_CFG0 through the latch = 0x35517751 or 0x35513331 in the boot step's format; `/dev/mem` agreeing where readable; PH1-PH3 held by no driver but uart4 (PH1) or daudio0 and by no GPIO user); GO writes `hdmi_audio_go.txt` on the SD card with this board's id (sys_info's `sunxi_chipid` / `sunxi_serial`, cpuinfo's `Serial`, else `unknown`), NO-GO removes it; its only file besides the reports. The boot step refuses `audio_out=1` without the GO for this board ("probe not run", PH_CFG0 neither read nor written); the HDMI Out row is greyed without it (Off still possible) with the hint "Run the HDMI audio probe first (hdmi-audio.md, 7)"; all three targets; strings in six languages. Tests: `hdmi_boot_test.sh` 144 checks per target (27 new: 14 marker cases, 8 board ids, ...), `run_tests.sh` (GO with the marker, 7 NO-GO causes with an earlier GO removed, the id from cpuinfo / unknown, no writable SD card; 3 shells), the emulator (the row without and with the GO, all three targets); mutants: 5 in `c_mutants.sh` (the first is c3e0860's behaviour), 7 in the probe kit's `mutants.sh` |
| 4. DVR Playback volume leaks into HDMI-in audio (low-medium) | yes (harness): after a DVR file at DVR Playback 5 both AIF1 DAC timeslot volumes stayed at 155 (at 0 with level 0) until the next boot | both timeslot volumes are in every batch, at 160 (0 dB): set again on every release; `test_hold` checks 160 / 160 after the release; mutant: not in the batch |
| 5. `audio_mixer_hold()` a boolean shared by two holders (low) | yes (harness): hold, hold, one release: a batch written while the second holder still held | one bit per holder (`AUDIO_MIXER_HOLD_PLAYBACK`, `AUDIO_MIXER_HOLD_TEST`); the last release applies the state in one batch (it no longer wakes the worker before its own request: with a request pending, the release could give two batches under load, seen once in a mutant run of phase 3); `test_hold` "two holders"; mutant: one release ends every hold |
| 6. `avshare_reset()` flushes the audio ring too, unsynchronised with its writer (low) | yes, by reading (both media, from the main thread, while the stream audio's thread writes); ripples' `test_avshare_reset` adapted to the fix fails on c3e0860's `avshare.c` ("reset must leave the audio chunks"); `t_pause.sh` (cross-built rtspLive) passes on both: no frame happened to be queued at the flush in the host test (largest gap 21 ms either way), the race window is small | the video side only (ripples' reason: the new video bitstream); ripples' `test_avshare_reset.c` adapted: the video side as ripples checks it, and the audio side must stay (the audio side of the ring is this project's addition: ripples had no audio writer); `t_pause.sh`'s limit 32 ms (1.5 AAC frames of 21.3 ms) and docs/live-audio.md / this file say the same: "no audio frame lost" |
| 7. The test buttons on the new paths (low) | routing yes (harness): Record Headphone Mix on, Line In in the mix: the Mic test recorded the mic and Line In through the output mixers; the busy case needs a device (arecord's error while the record daemon holds the capture); in the emulator, which records nothing, ripples 1.2.2 (4fbb865)'s page (= c3e0860's test code) says nothing | the held batch takes the output mixers out of the ADC (`test_hold`: the Mic test records the mic alone, directly); a Mic or Line/AV test that records nothing (the capture failed, or only a WAV header) shows "Nothing recorded: audio input busy (Stream Audio, HDMI Out Audio)" in place of the note and plays nothing back (six languages; emulator `audio_test_busy.txt`, all three targets) |
| 8. The test/motion stand-in copies the old expression (low) | yes: the check passed whether `ht.c` asks the mixer or the Audio Source | the stand-in returns a value the test sets against the Audio Source's preset in both cycles; with `ht.c` reverted to the Audio Source the test fails (Goggle 2, BoxPro; the Goggle 1 has no built-in receiver) |
| 9. MSP answers 255 / 0 Hz for an uncontrolled bay module (low) | yes: c3e0860 answered 255 / 0 Hz where ripples 1.2.2 (4fbb865) answered the stored channel (Goggle 1 Module None, Goggle 2 Expansion) | restored: the stored channel when the goggle does not tune the bay module, the bay module's own channel only when it does (all the module bay needs); `utilities/module_bay_test/test_msp_answers.py` (8 checks, in `make test`) compiles the two answers with stand-ins; mutants: 255 / 0 again (killed), the stored channel for a tuned module (killed); docs/module-bay.md corrected (it said "as in ripples: 255 / 0 Hz") |
| the Live Volume hint "Does not affect recording" (open item of phase 3) | wrong with Record Headphone Mix on | "Headphone volume. Also sets the A/V In level", six languages |
| ripples' nine labels and its test note untranslated (open item of phase 3) | - | translated into the five languages (within the fonts' glyphs; the Chinese note names the fill colours in English, the fonts have no glyphs for them); the note line by line, line 3 keyed with '-' for '=' |
| `t_kill.sh` flaky (open item of phase 2) | 3 of 9 runs in phase 2 | the stop times on the recorder's own log clock (the stub logs its T0 on CLOCK_MONOTONIC, `mono` reads that clock before the trigger); same limits; 10 of 10 runs pass (15 / 15 each, 150 checks): live video stopped after 0.015 to 0.507 s (limits 1.0 s, KILL 1.5 s), the audio chain after 3.02 to 3.52 s (limits 4.0 s, KILL 4.5 s) |
| found by the new round-trip test | Audio Source Line In, then Record Mic Yes: Record Mic off again after a restart (the missing `[mixer] rec_mic` followed the new `[record] audio_source`) | a `[mixer]` field missing from setting.ini that a changed `[record]` value would change at the next start is written with the change |

## Results of phase 4 (all on the PC)

| what | result |
|---|---|
| images, `build_all.sh` (goggle, goggle2, boxpro), Release with `-Werror` | built from b02cc20 (the commit after it changes only docs), no compiler warning; listed below. Each code commit of phase 4 builds (the goggle image at d56e9b4, 82771f4, bc34739, c06b00f, 7525689; 3538616 and b02cc20 change tests only), no warning. The suites below ran on the same code before the last re-commit, which changed only two anchors in `mutants.py` and the MSP test's stand-ins (the mutants ran after it) |
| emulator builds (goggle, goggle2, boxpro) | ripples' one warning (`page_version.c`) and no new one |
| emulator, sandboxed (`emu_run.sh`): start, port vs ripples | Goggle 1, Goggle 2, BoxPro, three starts each, against ripples' own emulator build (ripples 1.2.2 (4fbb865)): setting.ini identical after 2 s and at the end, no `[mixer]` written, no early exit or crash |
| emulator: the Audio page, every row (`keys/audio_page.txt`) | all three targets, from no setting.ini, ripples' shipped one (boot scan off) and one of `all`'s: every row selected and drawn (21 screenshots each), the note translated line by line, leaving the page wrote nothing to `[record]`, `[mixer]`, `[hdmi]` (in two runs, Goggle 1 without a setting.ini and BoxPro with `all`'s, ripples' boot scan wrote `[source]`, `[scan]`, `[autoscan]` meanwhile: it found the emulator's mock receiver; not the page) |
| emulator: changes on the page (`keys/audio_change.txt`, Goggle 2, BoxPro) | each change wrote its keys and the `[record]` values they belong to (`record_source`, `record_live`); from `all`'s setting.ini the first change also brought `[record] audio_source` and `live_audio_volume` in line with the page; the DVR test held the mixer (`held (0x2)`) and applied its state afterwards |
| emulator: HDMI Out Audio (`keys/audio_hdmi_out.txt`) | all three targets: without the probe's GO the row is greyed with "Run the HDMI audio probe first (hdmi-audio.md, 7)" and a click writes nothing; with a GO on the emulated SD card (`EMU_EXTSD_FILE`, board `unknown`: the PC has no id) the click writes `[hdmi] audio_out=1` and the row shows Restart |
| emulator: a Mic test that records nothing (`keys/audio_test_busy.txt`) | all three targets (the emulator records nothing): "Nothing recorded: audio input busy (Stream Audio, HDMI Out Audio)" in place of the note, the note back after leaving the page, nothing written; ripples 1.2.2 (4fbb865)'s page (c3e0860's test code) says nothing in the same run |
| `utilities/audio_mixer/test_mixer.py` | all passed, three targets (new: `test_hold`, the round trip, a restart after a change of `all`'s setting.ini) |
| `utilities/audio_mixer/mutants.py` | 88 mutants, 88 killed, at b02cc20 (6 in parallel, about 50 min): the 74 of phase 3 and 14 for the fixes (the held batch, the holders, the timeslot volumes, the round trip) |
| `utilities/audio_mixer/check_strings.py` | 186 strings (both layouts) x 6 languages: translated (nothing left in English), in the font, fitting |
| ripples' tests (`run_ripples_tests.sh`) | as in the baseline (ripples 1.2.2 (4fbb865)): all pass except `test_dvr_naming`'s build on glibc (`record`, `record_control`); `test_avshare_reset` adapted (finding 6), `test/motion` checks the mixer's value (finding 8) |
| `it66121_seq_test.sh`, `it66021_edid_test.sh`, `pcmbridge_test.sh`, `hdmi_boot_test.sh` | 12, 5, 78, 433 pass (`hdmi_boot_test.sh`: 144 per target and the no-target build; 352 before) |
| `utilities/probe/test/c_mutants.sh` | 26 mutants: 25 killed, 1 equivalent (the Reg04 read-modify-write, as before); 5 new for the probe's GO |
| `make -C utilities/module_bay_test test` | 178 pass (170 and the new `test_msp_answers.py`'s 8) |
| `utilities/module_bay_test/mutants.sh` | 18 of 18 killed (16 and the two of the MSP answers) |
| `utilities/module_bay_test/check_strings.py` | 46 strings x 6 languages fit |
| `run_probe.sh run` / `mutants` | 83 pass (65 and the verdict's 18) / 25 of 25 killed (18 and the verdict's 7) |
| `run_live.sh all` (host) | ring buffer 7254 checks native and under qemu-arm, `t_aidev.sh` 8 |
| the rtspLive tests on the cross-built rtspLive under qemu-arm (`RTSPLIVE=arm`, goggle2 build) | describe 12, share 6, e2e 38, avsync 12, audio_fail 6, snapshot 9, pause 7 (largest audio gap 21 ms), `t_arm.sh` 65 (e2e 38, describe 12, kill 15); `t_cpu.sh` only measures (Stream Audio on: 1084 wake-ups/s, 5.6 % of a host core; off: 994, 6.3 %); `t_off_identical.sh` needs a host live555 (not run, as before) |
| `t_kill.sh`, 10 runs | 10 of 10 pass, 15 / 15 each: live video stopped 0.015 to 0.507 s after the trigger (limits 1.0 s, KILL 1.5 s), the audio chain 3.02 to 3.52 s (limits 4.0 s, KILL 4.5 s), on the recorder's log clock |
| before the fixes (c3e0860's behaviour put back on copies) | findings 1, 2, 4, 5, 7: the scenarios above with c3e0860's harness; 8: `test/motion` with `ht.c` asking the Audio Source fails on Goggle 2 and BoxPro; 6: the adapted `test_avshare_reset` fails with c3e0860's `avshare.c`; 3 and 9: the mutants that restore c3e0860's behaviour are killed |

Images of phase 4 (built on the developer's machine, not released):

| image | bytes | SHA-256 |
|---|---|---|
| HDZERO_GOGGLE-77-206-1.2.2-ripples-b02cc20.bin | 10076160 | fb2579018ef1336181f4ed9e08700e2473d02c565396ec38c2c01d932c2d7376 |
| HDZERO_GOGGLE2-77-198-1.2.2-ripples-b02cc20.bin | 12912640 | fa90abbe874731ff795a12b25083215834c08bf938b1d7b3c852125c5cfa261a |
| HDZERO_BOXPRO-77-211-1.2.2-ripples-b02cc20.bin | 9779200 | e8c6b2238b29109a8183b1524eabf0270260359823282d9df72022e7cbee74bb |

Not testable without a goggle: everything audible (the held batch against libmedia_mpp's playback route, the
timeslot volumes, arecord's error while the record daemon holds the capture), the backpack's handling of the
channel answers, whether the probe's GO conditions hold on each target (the device tree and pins: section 7 of
docs/hdmi-audio.md), and whether PH1-PH3 reach the IT66121 and nothing else on each board, which no probe reads.

## Pre-release v1.2.2-juricabi.1

Its release page was taken offline on 2026-11-20, when juricabi.2 became the release (kept as a draft); the tag
`v1.2.2-juricabi.1` and the branch `juricabi.1` stay.

The first public pre-release, marked untested on hardware: the code of phase 4 (b02cc20) with the tone test's
gate (below), the README, docs/first-flash.md, the new name and version.

* **Version**: `1.2.2-juricabi.1` = ripples' 1.2.2 plus juricabi's changes, number 1 (VERSION). The Firmware page
  shows `juricabi.1 ripples 1.2.2` (`format_app_version()` in `src/ui/page_version.c`: 24 characters, the buffer
  holds 31; a build that is not at a tag appends `-<commit>`, cut to what fits; in the emulator the row shows
  either form in full on all three targets). ripples' 1.2.2 still parses as the three numbers that `elrs.c` sends
  to the backpack. Nothing else reads the version or the image names: the Firmware page and the update scripts
  look for `HDZERO_<target>` and `hdzgoggle_app_ota*.tar`, which stay; no setting is tied to the version.
  `utilities/dev/check_version.py` checks the version, its display and the workflows, and with `--images` the
  images.
* **Workflows** (`.github/workflows/build_and_release_*.yml`): they build on every push and pull request, as
  ripples' do, and make a release draft on a tag `v*-juricabi.*` (was `ripples-v*`).
* **The tone test's gate**: `utilities/probe/hdmi_audio_test.sh` runs its HDMI out part (PH1-PH3, the IT66121) only
  with the read-only probe's GO for this board, by the boot step's rule; otherwise it says why and touches
  neither; its HDMI in part runs as before.
* **Images**: built at the annotated tag `v1.2.2-juricabi.1` (so without a commit suffix) with
  `utilities/dev/build_all.sh`, and checked with `utilities/dev/check_version.py --images` (the names, the
  `.sha256`, the app package and the RX / VA images inside): `HDZERO_GOGGLE-77-206-1.2.2-juricabi.1.bin`,
  `HDZERO_GOGGLE2-77-198-1.2.2-juricabi.1.bin`, `HDZERO_BOXPRO-77-211-1.2.2-juricabi.1.bin`. Their SHA-256s are in
  the `.sha256` files on the release, not here: this file is part of the tagged commit, and a build is not byte for
  byte the same twice (the time stamps in the tar and the jffs2), so a rebuild at the tag gives other digests.
* **What the images take from the code since phase 4's images (b02cc20)**: VERSION and `page_version.c` (the
  version and its display); otherwise comment lines only (five in `src`, one in each of five language files). The
  rest are docs, tests and tools.
* **`utilities/dev/build_all.sh`** is now in git (it was on `all`; `.gitignore`'s `build_*` hid it here) and takes
  the name of a build at a tag; it checks each name exactly (the version alone at a tag, else with the commit).
* **The probe kit's `mutants.sh`** fails unless every listed mutant ran: a single quote in the expression of a new
  mutant had ended the quoted list early, so no mutant ran and the script still said ALL MUTANTS KILLED (found
  before anything was pushed; the expression no longer needs the quote).

### Results of the pre-release (all on the PC)

| Check | Result |
|---|---|
| `utilities/dev/check_version.py` | all pass: the version; its display for a release, a development build, a two-digit number and another firmware's version; ripples' 1.2.2 parsed from it; the three workflows |
| emulator, sandboxed (`emu_run.sh`, `keys/firmware_version.txt`, `EMU_APP_VERSION`) | Goggle 1, Goggle 2, BoxPro: Current Version `app: juricabi.1 ripples 1.2.2 rx0: 15 rx1: 15 va: 0`; with `1.2.2-juricabi.1-abc1234` `app: juricabi.1 ripples 1.2.2-abc123 …`; both in full within the row; no early exit, no crash |
| probe kit (`run_probe.sh run`) | 94 pass (dash and busybox sh), the gate's cases among them |
| probe kit mutants (`run_probe.sh mutants`) | 28 of 28 killed, the gate's three by its cases (no GO, another board's GO, a NO-GO); 3721 s |
| ripples' tests (`run_ripples_tests.sh`) | as in the baseline: all pass except `test_dvr_naming`'s build on glibc |
| audio mixer: `test_mixer.py`, `mutants.py`, `check_strings.py` | all pass; `mutants.py` 88 of 88 killed; 186 strings x 6 languages fit |
| module bay: `make test`, `check_strings.py` | 44 checks pass; 46 strings fit |
| `it66121_seq_test.sh`, `it66021_edid_test.sh`, `pcmbridge_test.sh`, `hdmi_boot_test.sh` | not run again: only comments of the first two changed since phase 4 |

## Build, emulate, test

All on Linux (WSL Ubuntu 24.04, root), worktree on the Linux file system. Scripts: [utilities/dev/](../utilities/dev/README.md).

```sh
utilities/dev/build_all.sh /root/hdz/goggle-ripples goggle goggle2 boxpro   # images + .sha256 -> $OUT
utilities/dev/emu_build.sh goggle                                            # SDL emulator build (also goggle2, boxpro)
utilities/dev/emu_run.sh /root/hdz/emu/build_goggle - $PWD/utilities/dev/keys/check_start.txt g1_none   # always sandboxed
utilities/dev/run_ripples_tests.sh                                           # ripples' test/* and test/dvr, as an unprivileged uid
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

1. Done 2026-11-20: the final juricabi.2 image is on the owner's Goggle 1 (the owner's verdict: all good), the
   tag `v1.2.2-juricabi.2` is pushed and the release made (published as a pre-release, made the release the
   same day at the owner's word). Still worth a look there, one by one, with Logging
   On (section "juricabi.2"): a card pulled right after a stop, a start with no card or with a card that cannot
   be written, Clear DVR Folder, and `dvr_dbg.log` staying as it is with Logging Off. The Goggle 2 and BoxPro
   images have not run on hardware.
2. Done 2026-11-20: the goggle's backpack runs the unchanged 1.5.9 again (its page reports 1.5.9 (437a4f), the
   tag's commit); the debug build used to find the lost commands is off it.
3. The owner's ESP32-C3 backpack on an Orqa goggle receives the same radio's commands with Telem Ratio Std; the
   HDZero goggle's built-in ESP32 does not. The owner's explanation: the antenna (a proper one on the C3 board,
   the goggle's ESP32 inside the housing). Not measured. Not tested: 500 Hz, Race, other goggles. A fix on the
   radio's side for everyone (the TX backpack sending a command again when no acknowledgement came; docs/dvr.md
   has where) would be ExpressLRS's. The owner stays with Telem Ratio 1:32 and does not want it built here.
4. A recording stopped within a second of its start takes ripples' recorder up to 3 s to stop (the owner's
   `dvr_dbg.log`); the goggle waits for it. Not looked into.
5. A decision for the owner: ripples' `test_dvr_naming.c` does not build on glibc; fix it here (it is ripples'
   test) or leave it to ripples.
6. On the goggle, still untested: everything of docs/module-bay.md, "Untested on hardware"; the order of
   module-bay-mod-safety.md (Check Pins with Module None first); Stream Audio (docs/live-audio.md section 4);
   HDMI audio only after the read-only probe (docs/hdmi-audio.md section 7).
7. Known and left as it is: a goggle with a legacy `/data/confs/record.conf` streams without audio (rtspLive reads
   that file, the WiFi page writes the app's; the same on `all`).
8. From phase 4, for the owner: (b) a `[mixer]` written by c3e0860 (never released) has no `record_source` /
   `record_live` and is read like one of all's firmware. (c) A board whose id cannot be read is `unknown`: its GO
   counts on any goggle without a readable id (then the GO binds only to the SD card). (d) From all's firmware,
   DVR Playback follows all's `playback` only roughly (all's DAC was 6 dB lower). (e) The busy message comes after
   the capture failed; the test cannot know in advance whether a client streams.
