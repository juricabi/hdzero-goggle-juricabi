# First flash: the order that risks least (juricabi's firmware; Goggle 1 first)

This page is for flashing juricabi's firmware (ripples' firmware 1.2.2 with juricabi's additions, version
`1.2.2-juricabi.2`, docs/status.md) onto an HDZero goggle for the first time, checking it, and going back to ripples' official
1.2.2 if you want to. It is written for the original HDZero Goggle (Goggle 1); the notes marked **Goggle 2** and
**BoxPro** say where those differ. juricabi.1 and ripples' 1.2.2 have been flashed this way on a Goggle 1, back and
forth; the Goggle 2 and BoxPro notes are not tried. The menu texts and the file handling below are read from ripples' code (`src/ui/page_version.c`, `mkapp/app/script/update_goggle.sh`,
`update_goggle2.sh`, `update_boxpro.sh`, `rc.sh`, `mkapp/mkapp_ota.sh`); where something could not be checked,
the text says so. juricabi's firmware changes none of these files except `rc.sh` (one step, `hdmi_audio_boot`, which
does nothing unless HDMI Out Audio was switched on: docs/hdmi-audio.md), so the flash path is ripples' own.

The images of this release (the repository's Releases page) are:

| target | image |
|---|---|
| Goggle 1 | `HDZERO_GOGGLE-77-206-1.2.2-juricabi.2.bin` |
| Goggle 2 | `HDZERO_GOGGLE2-77-198-1.2.2-juricabi.2.bin` |
| BoxPro | `HDZERO_BOXPRO-77-211-1.2.2-juricabi.2.bin` |

each with a `.sha256` file next to it. (A build that is not at a git tag appends `-<commit>`.)

Do the sections in order. Do not change any setting before section 4 is done.

## 1. Keep these on the PC before you flash

1. **ripples' official 1.2.2 for your goggle**, to go back with, from
   https://github.com/LVTY-AI/hdzero-goggle/releases/tag/ripples-v1.2.2 (sizes and SHA-256 as GitHub lists them
   for that release):

   | target | file | bytes | SHA-256 |
   |---|---|---|---|
   | Goggle 1 | `HDZERO_GOGGLE-77-206-1.2.2-ripples.bin` | 9973760 | `9a9109e1c45a8feb871b84a656812f3f1158be108cf3f40549b1adc87901dc6f` |
   | Goggle 2 | `HDZERO_GOGGLE2-77-198-1.2.2-ripples.bin` | 12800000 | `a6a359838488e6b8d58910cc8f150d8902a0f679c14c05cff55b7ccf20c08bb1` |
   | BoxPro | `HDZERO_BOXPRO-77-211-1.2.2-ripples.bin` | 9676800 | `6f81a8ba074d00339a7817838051844f993c6d94bfb9203dff8c348f7c22fdfd` |

2. **The official HDZero recovery package** of your goggle, from https://www.hd-zero.com/document: the last
   resort (section 9.2). Copies examined for this project's docs:
   * Goggle 1: `HDZEROGOGGLE_Rev20250319.zip`, 25,300,568 bytes, SHA-256
     `17cba93bbd29c20a5c011947c368a65d10b1d6c2fd8ad6715138c5f349475fe7` (it holds
     `HDZERO_GOGGLE-75-200-9.5.0.bin` and `Recovery/` with `HDZG_BOOT.bin`, `HDZG_OS.bin`,
     `HDZGOGGLE_RX.bin`, `HDZGOGGLE_VA.bin` and the user manual);
   * Goggle 2: `HDZEROGOGGLE2_Rev20250715.zip` (its `Recovery/HDZG_OS.bin` was examined, docs/hdmi-audio.md
     section 2; its size and hash were not recorded);
   * BoxPro: `HDZEROBOXPRO_Rev20250715.zip`, 23,215,845 bytes, SHA-256
     `351397af9c7edb10d4f053948f0f81f56bd892b535d9611db22767460892419e` (`Recovery/HDZB_OS.bin`).

   These hashes only tell you whether your download is the file looked at here: HDZero publishes none.
3. **This build's image and its `.sha256` file**, checked (below).

**Checking a SHA-256 on Windows.** In a Command Prompt or PowerShell window, in the folder with the file:

```
certutil -hashfile HDZERO_GOGGLE-77-206-1.2.2-ripples.bin SHA256
```

or in PowerShell:

```
Get-FileHash .\HDZERO_GOGGLE-77-206-1.2.2-ripples.bin -Algorithm SHA256
```

`certutil` prints the hash in small letters, `Get-FileHash` in capitals: compare the 64 characters and ignore the
case. For this build's image, compare with the first 64 characters of its `.sha256` file (the hash, two spaces,
the file name), or let PowerShell compare:

```
(Get-FileHash .\HDZERO_GOGGLE-77-206-1.2.2-juricabi.2.bin -Algorithm SHA256).Hash -eq (Get-Content .\HDZERO_GOGGLE-77-206-1.2.2-juricabi.2.bin.sha256).Split(' ')[0]
```

It must print `True`. If a hash differs, download the file again; never flash a file whose hash does not match.

Also write down (or photograph) the settings you care about. ripples' update script copies `setting.ini` to
`/mnt/UDISK/` before it writes the app partition; whether the base firmware's `hdz_upgrade_app.sh` (not in this
repository) puts it back afterwards, and what a recovery (section 9.2) keeps, was not checked.

## 2. The SD card: only the one image on its root

Use a FAT32 card. Put **only** this build's image for your goggle on the card's root: not its `.sha256`, no other
`HDZERO_…` file, no `develop.sh`, and none of the recovery files (`HDZG_BOOT.bin`, `HDZG_OS.bin`, `HDZB_…`,
`HDZGOGGLE_RX.bin`, `HDZGOGGLE_VA.bin`). Folders (DVR recordings) can stay.

Why, from the code:

* The Firmware page lists **every file on the card's root whose name contains `HDZERO_GOGGLE`** (Goggle 1;
  `HDZERO_GOGGLE2` on the Goggle 2, `HDZERO_BOXPRO` on the BoxPro: a substring match in `page_version.c`). On a
  Goggle 1 the `.sha256` file and every Goggle 2 image appear in the list next to the right one. If any listed name
  contains `.zip`, the page unzips the chosen file instead of copying it.
* ripples' update script checks the file before it erases anything: a file `tar` cannot list (a `.sha256`, a
  damaged download) is refused with "firmware file is corrupt or truncated - not flashing", an archive without the
  app package and the RX and VA images with "firmware archive is missing components - not flashing", and a flash
  chip is erased only when its image is there and not empty. The page then shows "FAILED" and nothing was
  written (read from the script, not tried).
* **But a Goggle 2 image passes those checks on a Goggle 1**: it carries the same inner names
  (`HDZGOGGLE_RX-77.bin`, `HDZGOGGLE_VA-198.bin`: ripples' scripts map the Goggle 2 to the Goggle 1's names), and
  the script checks only the sizes of the flash chips, so it would write the Goggle 2's VA image and app onto the
  Goggle 1. (The other way round the Goggle 2's list does not show Goggle 1 images.)
* At every boot `rc.sh` acts on recovery files on the card's root, before the app starts: `HDZG_BOOT.bin`
  (`HDZB_BOOT.bin` on the BoxPro) is written to the boot loader, `HDZG_OS.bin` is deleted, and
  `HDZGOGGLE_RX.bin` / `HDZGOGGLE_VA.bin` are written to the receiver and VA flash (`write_flashes.sh`). Keep them
  off the card unless you mean that (section 9.2).

With one image on the card there is nothing to choose wrongly.

## 3. Flash from the menu

1. Only the power cable connected (no HDMI, no USB), and a power source that will not run out for 10 minutes.
2. SD card in, power on. Main menu > **Firmware**. The **Current Version** row shows what is installed now,
   `app: ripples-1.2.2 rx0: … rx1: … va: …` for ripples' official 1.2.2 (its version file is `1.2.2-ripples`;
   `page_version.c` shows it as `ripples-1.2.2`): write it down.
3. Select **Update Goggle** and click. A window opens, titled "Goggle Firmware SD Card", with "Target:" and the
   file name (a list to choose from if more than one file matched): check that it is this build's image.
   ("Goggle Firmware not found" means the card has no matching file on its root.)
4. In the window, select **Update Goggle** and click. The row shows "WAIT... DO NOT POWER OFF..." and a progress
   bar: first the receiver (two flash chips) and VA images, then the app partition; the script estimates each
   phase from the sizes in the image. **Do not power off.** It takes a few minutes (the official HDZero page says
   about 3 for its images; this one was not timed).
5. When the script reports "all done", the goggle shows **"Update complete"**: "Goggle update completed
   successfully. Please repower goggle now.", beeps three times and ignores the buttons. Power off, wait 5 s,
   power on.
6. Main menu > Firmware: Current Version now reads `app: juricabi.2 ripples 1.2.2 rx0: … rx1: … va: …` (the
   version file of this build is `1.2.2-juricabi.2`; the row was looked at in the emulator, with that version
   file, on all three targets).
7. Take the image off the card's root again, so that the Firmware page does not offer it later.

If the row shows **"No firmware found."** or **"FAILED"** (red) instead: read section 2 again; with the right
single image, flash once more. If it still fails, put only ripples' official image on the card and flash it the
same way (section 9.1); if the goggle no longer reaches its menu, section 9.2. A "FAILED" from one of the
script's refusals (section 2) has written nothing.

**Goggle 2 / BoxPro**: the same page and texts; the script is `update_goggle2.sh` / `update_boxpro.sh`
(identical to `update_goggle.sh`: it reads the target from `/mnt/app/platform`). The Goggle 2 writes its VA
image twice (a primary and a secondary copy), so its first phase takes longer.

## 4. Check the untouched goggle

Change no setting yet. Check each item against what the goggle did with ripples' 1.2.2:

1. **HDZero video and OSD**: picture, OSD elements, channel.
2. **DVR**: record 1 minute with your usual settings, then play it back (Main menu > Playback): picture and
   sound as before (the input ripples' Audio Source recorded is recorded; DVR Audio is ripples' Record Audio).
   Stop and start a recording a few times: each stop takes about as long as on ripples (`dvr_dbg.log` on the card,
   `record_stop begin` to `record_close begin`, within a second).
3. **Headphones at boot**: headphones on before you power on; as on ripples they are silent until the first
   switch to an analog source. Listen also right after the SD card check at boot (and after a card insertion, an
   SD repair or format): the record daemon's restart resets part of the codec, and this build sets it back within
   about 2 s; a click then is possible (not measured).
4. **Analog**: Goggle 2 / BoxPro built-in receiver, and the bay module (insert it only with the goggle off):
   Source > Analog, the picture and sound as before. With Module Bay > Module = None (the default) the goggle
   leaves the bay pins alone and does not set a bay module's channel: set it on the module, as on ripples.
5. **HDMI out**: the picture on a monitor.
6. **WiFi**: as you use it; then switch it off again.
7. **ELRS backpack**, if you use it: channel commands and DVR on arm. The DVR follows the arm also through a
   menu and a channel switch, and Record Mode Auto no longer restarts after a disarm (docs/dvr.md, "Checks").
   **Set Telem Ratio to 1:16 or 1:32 in the ExpressLRS Lua first** (not Std): with Std at 250 or 500 Hz about one
   command in four never reaches the goggle (docs/dvr.md, "Commands that never arrive").
8. **Standby and wake**: as you usually do.

If something that worked with ripples' 1.2.2 does not work now, go back (section 9.1) and report what you saw.

### What looks different from ripples 1.2.2

* Main menu: a new **Module Bay** entry (Goggle 1 and 2; the BoxPro has no module bay).
* Audio page: one row per input with its Headphones level and Record switch, then Levels, DVR and HDMI Audio
  (experimental); the page scrolls. ripples' Audio Source and Live Volume rows are gone (their keys stay).
  docs/audio.md.
* WiFi Module > System: a new **Stream Audio** row (Off). docs/live-audio.md.
* Firmware > Current Version shows `juricabi.2 ripples 1.2.2` (section 3).
* `setting.ini` is not changed at start-up; the first change on the Audio page adds a `[mixer]` section.

## 5. Then the Audio page

Main menu > **Audio**. Try the rows one at a time: the Headphones level of each input you use (Mic; Line In if
you use that jack; A/V In with an analog source), then its Record switch, each with a short recording played back;
then Record Level and the boosts. docs/audio.md explains every row and lists what to listen for ("Testing on a
goggle"). Leave the two rows under "HDMI Audio (experimental)" Off until section 7.

## 6. Then Stream Audio

Only if you stream over WiFi: WiFi Module > Page: System > **Stream Audio** On > Apply Settings, then stream as
you usually do: the stream carries what the DVR records. docs/live-audio.md, section 4, says what to check. A
Mic or Line/AV test on the Audio page while a client streams says "Nothing recorded: audio input busy": the
stream holds the input.

## 7. Optional: the probe and HDMI audio (only on a GO)

Only if you want HDMI audio. Everything is in `utilities/probe/` ([README](../utilities/probe/README.md)):

1. **The read-only probe first.** Copy `hdmi_audio_probe.sh` to the SD card's root and rename it `develop.sh`
   (the goggle then runs it instead of starting the goggle app; the script starts the app itself). Boot: the
   screen stays dark for up to about a minute, then **2 beeps** and the app starts. Leave the goggle on for
   **2 more minutes**. Power off, keep `hdmi_audio_probe_N.txt`, `hdmi_audio_dmesg_N.txt` (and
   `hdmi_audio_config_N.gz` if it exists). **Delete or rename `develop.sh`.**
2. The report ends part 1 with **"HDMI Out Audio verdict: GO for board …"** or **"NO-GO: …"** and the reasons. On
   GO the probe has written `hdmi_audio_go.txt` on the card: leave it there. Without it HDMI Out Audio cannot be
   switched on (the row is greyed and says to run the probe first, the boot step refuses it) and the tone test
   below skips its HDMI out part. On NO-GO stop here for HDMI out: docs/hdmi-audio.md, section 8, says what the
   reasons mean.
3. **Then, on GO only, the tone test**: `hdmi_audio_test.sh` as `develop.sh` together with
   `tone1k_48k_s16le_2ch.raw` (a TV or monitor with speakers on HDMI out; for part B an HDMI source that plays
   sound). Delete `develop.sh` afterwards.
4. Switch HDMI Out Audio / HDMI In Audio on only if the tone test shows the path works (the decision table of
   docs/hdmi-audio.md, section 8). The switches only store the setting: restart to use them (an orange "Restart"
   until then; "Error" with the reason in the hint if HDMI Out Audio stayed off at the start). Switched off and
   restarted, the goggle is as before: `rc.sh` sets the pins back at every boot.

The GO is about the SoC side only: whether PH1-PH3 reach the HDMI transmitter, and nothing else, on your board is
what no probe can read (docs/hdmi-audio.md). **BoxPro**: the same steps; its wiring was never looked at, so be
the more careful there. Do not run `bay_gpio_pattern.sh` here: it belongs to section 8 of the mod checklist.

## 8. Then the module bay

Goggle 1 and Goggle 2 only, after sections 1-4, and with the checklist of
[module-bay-mod-safety.md](module-bay-mod-safety.md), which is the one to follow. In short, its order: bay empty,
Module = None, WiFi off; **Check Pins** twice; WiFi on and off again to see that it still connects; then the mod,
by the voltage Check Pins shows; then the True-D (MOD 1.17, `BAY:RAPIDFIRE`) in the bay with the goggle off and
Module = rapidFIRE. Keep WiFi off while flying analog; never insert a module with the goggle on.

## 9. Back to ripples' official 1.2.2, and recovery

### 9.1 From the menu (the goggle still reaches its menu)

Put **only** ripples' official image for your goggle (section 1) on the card's root and flash it exactly as in
section 3. The update script is the one on the goggle (ripples' own, unchanged in this build), and the official
image carries its own receiver, VA and app images. Afterwards Current Version reads `app: ripples-1.2.2 …`.

### 9.2 If the goggle app no longer starts: the official HDZero recovery (last resort)

The official "Goggle Emergency Firmware Update Process" (https://docs.hd-zero.com/goggles-firmware-update), with
the files of the recovery zip of your goggle (section 1):

1. On a FAT32 card's root, only `HDZG_OS.bin` (BoxPro: `HDZB_OS.bin`), `HDZGOGGLE_RX.bin` and
   `HDZGOGGLE_VA.bin` from the zip's `Recovery/` folder (follow the official page for the BoxPro's files).
2. Only the power cable connected; card in; power on and **wait 5 minutes**; power off. Do not cut it short.
3. If it worked, the files are gone from the card.

Afterwards the goggle runs the stock HDZero firmware of that package (Goggle 1: app 9.5.0, dated 2025-03-19), not
ripples': flash ripples' 1.2.2 from the menu (9.1) if you want it back. The official page also describes a
PhoenixCard process (Windows only) as the very last step; follow it exactly, including its warning about the
card's hidden partition. Which part of the system performs the emergency update, and whether it keeps
`setting.ini`, was not checked here.

### 9.3 What stays in setting.ini

After going back, `setting.ini` keeps this build's `[mixer]`, `[module]` and `[hdmi]` sections and
`[wifi] stream_audio`: ripples' firmware does not read them, so they do no harm. ripples reads its own `[record]`
keys, which this build writes as its Audio page shows them (Audio Source: the first recorded input; Live Volume:
Headphone Volume; Mic Gain, Line/AV Gain: the boosts; DVR Playback). Coming back to this build later, what ripples changed in `[record]` is taken over (docs/audio.md,
"Settings").

## What could not be checked here

* The flash on the Goggle 2 and BoxPro, and the timings: read from the code.
* Whether the base firmware's `hdz_upgrade_app.sh` restores `setting.ini` from `/mnt/UDISK/`, and what the
  recovery keeps.
* ripples' release page: the sizes and digests above are GitHub's for that release, as given to this project; the
  files themselves were not downloaded for this document.
* The hd-zero.com download page and the official update page's figures: not opened for this document; the
  recovery zips' hashes are those of the copies examined earlier (docs/hdmi-audio.md, module-bay-mod-safety.md).
