# Audio: the Audio page and the codec (juricabi's firmware)

All three targets (HDZero Goggle, Goggle 2, BoxPro) use the internal audio codec of the Allwinner
V536 (`sun8iw16`, ALSA card `audiocodec`). The firmware drives it with `amixer`; the record daemon
captures the ALSA `default` device (48 kHz stereo, AAC). The control names and ranges are the ones
of the device's `amixer contents` dump in `mkapp/app/app/record/audio/audiocodec/alsa-util/`, their
meaning comes from the Allwinner driver (`sound/soc/sunxi/sun8iw16-codec.c` of the V536 BSP).

Since juricabi.2 the Audio page has **one row per input**: how loud you hear it and whether it is
recorded, next to each other. Nothing on the page changes another row. (juricabi.1 had ripples'
Audio Source, Live Volume and Mic / Line/AV Gain rows above the extras of all's mixer; Audio Source
and the Record switches were the same setting twice, Live Volume moved the A/V In level, and Record
Headphone Mix greyed out the switches of what it heard. The owner found that confusing.)

## In short

* **Headphones** (per input): the input's level in the analog mixer that feeds the headphone jack
  (the line out): Off, or -4.5..+6 dB. Only what you hear.
* **Record** (per input, but HDMI In): Yes puts the input straight from its jack into the
  recording, heard or not. The recording is one ADC: its capture is the DVR, the WiFi stream audio
  (Stream Audio) and HDMI out audio (HDMI Out Audio) alike.
* **Headphone Volume**: the volume of the jack. **Record Level**: the level of everything recorded
  (the ADC input gain). **Mic Boost** / **Line In Boost**: the mic's and the Line In jack's input
  amplifiers, for hearing and recording alike. A/V In has none.
* The codec has no record level per input (section "Per-input record levels"): two recorded
  inputs share Record Level, and the boosts balance them for hearing and recording together.
* HDMI-in audio is heard only: there is no path from the DAC into the ADC but through the
  output mixers, which are never recorded.
* With nothing recorded, the mic stays connected to the ADC at the minimum ADC volume (-119 dB):
  the recording is silent, and the ADC keeps a source (section "The ADC always has a source").
* ripples' keys stay for ripples' values (`[record] live_audio_volume`, `mic_gain`, `linein_gain`,
  `audio`, `dvr_audio_volume_v2`, and `audio_source`, which follows the first recorded input), so
  going back to ripples' firmware reads them as the page shows them. A goggle coming from ripples
  1.2.2 records and hears what it did there until a row is changed (section "Settings").
* DVR playback and the test buttons run ripples' own codec code. The mixer is held meanwhile: it
  first takes its own live paths out of the codec's mixers, so that only what ripples' code routes is
  heard or recorded, and it sets its whole state again afterwards.

## The Audio page

One page, scrolled so that the selected cell is always on screen (a scroll bar on the right shows
where the window is), with the help of the selected cell below the list (ripples' note when Run
Test is selected). The dial walks an input row cell by cell: its Headphones level, then its Record
switch.

```
Inputs              Headphones               Record
 Mic                ====|-----   -1.5 dB      Yes
 Line In            |---------   Off          No
 A/V In             ======|---   +3.0 dB      Yes
 HDMI In            |---------   Off          -
Levels
 Headphone Volume   ==========   10
 Record Level       ===|------   0.0 dB
 Mic Boost          =====|----   4
 Line In Boost      ===|------   3
DVR
 DVR Audio          [Yes]  No
 DVR Playback       ========|-   8
 Run Test           DVR  Live  Mic  Line/AV
HDMI Audio (experimental)
 HDMI Out Audio     On  [Off]
 HDMI In Audio      On  [Off]
< Back
```

| Row | Values | setting.ini | Default (fresh) |
|---|---|---|---|
| Mic: Headphones / Record | Off, -4.5..+6.0 dB / Yes, No | `[mixer] mic` 0..8 / `rec_mic` | Off / Yes |
| Line In: Headphones / Record | Off, -4.5..+6.0 dB / Yes, No | `[mixer] line_in` / `rec_line_in` | Off / No |
| A/V In: Headphones / Record | Off, -4.5..+6.0 dB / Yes, No | `[mixer] av` / `rec_av` | +6.0 dB / No |
| HDMI In: Headphones (greyed while HDMI In Audio is not in effect) | Off, -4.5..+6.0 dB | `[mixer] hdmi` | 0.0 dB |
| Headphone Volume | Off, 1..10 | `[record] live_audio_volume` | 10 |
| Record Level | -4.5..+6.0 dB | `[mixer] rec_level` 0..7 | 0.0 dB |
| Mic Boost | 0..7 | `[record] mic_gain` | 4 |
| Line In Boost | 0..7 | `[record] linein_gain` | 3 |
| DVR Audio | Yes / No | `[record] audio` | Yes |
| DVR Playback | 0..8 | `[record] dvr_audio_volume_v2` | 8 |
| Run Test | DVR, Live, Mic, Line/AV | - | - |
| HDMI Out Audio (On only with the HDMI audio probe's GO for this board, docs/hdmi-audio.md section 7; from the next start) | On / Off | `[hdmi] audio_out` | Off |
| HDMI In Audio (from the next start) | On / Off | `[hdmi] audio_in` | Off |

What each row does on the codec:

* **Headphones** of an input: its output mixer gain (`MIC1_G`, `MIC2 BST`, `LINEINL/R` to the output
  mixers), Off disconnects it. HDMI In: the DAC volume while HDMI-in audio plays (`DAC volume` 152 +
  2 x (level - 4)).
* **Record** of an input: its switch into the ADC input mixer (`MIC1 boost`, `MIC2 boost`, `LINEINL/R`).
* **Headphone Volume**: `lineout volume` `(v * 31 + 5) / 10`, ripples' scale; Off switches the line out
  off. It does not make the headphones live from boot (ripples' slider never switched the line out on).
* **Record Level**: `ADC input gain control`, 0..7 = -4.5..+6 dB.
* **Mic Boost** / **Line In Boost**: `MIC1 boost AMP gain control` / `MIC2 boost AMP gain control`.
* **DVR Audio**: ripples' Record Audio: whether the DVR files have a sound track. The WiFi stream and
  HDMI out keep their sound either way.
* **DVR Playback**: ripples' playback volume (line-out table and DAC volume), used by ripples' playback
  code (`player/adec2ao.c`) and the DVR test as before.
* **Run Test**: ripples' four tests. They run ripples' own codec code: the mic or line into the ADC,
  the DAC to the line out. The mixer is held during a test (the page's own live paths out first: a
  Mic test records the mic alone), and afterwards it puts the page's routing back. A Mic or Line/AV
  test that records nothing (the audio input held by Stream Audio or HDMI Out Audio, whose capture the
  record daemon has open) says so in place of the note: "Nothing recorded: audio input busy (Stream
  Audio, HDMI Out Audio)".

Bookkeeping that has no row: ripples' `[record] audio_source` follows the Record switches (the current
one while it is recorded, otherwise the first recorded of Mic, Line In, A/V In), so ripples' firmware
records one of them after a way back. The built-in receiver's audio (Goggle 2, BoxPro: `[mixer]
rx_audio`, used by every `rtc6715.init(1, ...)` for the live picture) follows A/V In: on while A/V In is
heard or recorded.

Sliders: click to grab, turn, click to release; the value applies while it moves. DVR Playback is
stored on release, as on ripples, and used by the next playback. Switches apply on click. setting.ini
is written on a switch click, a slider release, leaving the page, and 1 s after the last dial step
of a slider still grabbed. Only what changed is written.

## Signal paths

```
 codec input           preamp            output mixer gain     L/R OUTPUT MIXER          line out
 MIC1  built-in mic ──[MIC1 boost 0..7]─┬─[MIC1_G      -4.5..+6 dB]─sw─┐
 MIC2  "Line In" jack [MIC2 boost 0..7]─┼─[MIC2 BST    -4.5..+6 dB]─sw─┤
 LINEIN A/V (L/R) ──────────────────────┼─[LINEIN gain -4.5..+6 dB]─sw─┼──> L+R sum ─[lineout volume 0..31]─sw─> jack
 DAC   playback / HDMI-in audio ────────┼──────────────────────────sw─┘      (LINEOUTL/R Mux = OMIX_LR_SUM)
                                        │
            Record Mic / Line In / A/V In  sw: MIC1, MIC2, LINEIN  (the output mixers into the ADC: always off)
                                        v
                          L/R ADC INPUT MIXER ─[ADC input gain -4.5..+6 dB]─ ADC ─[ADC volume, digital]─ AIF1
                                                                                  ─> record daemon: DVR, stream, HDMI out
```

* MIC1 boost = Mic Boost; MIC2 boost = Line In Boost; the output mixer gains = the Headphones levels;
  `lineout volume` = Headphone Volume; `ADC input gain control` = Record Level; `ADC volume` 160 (0 dB),
  0 (-119 dB) while nothing is recorded.
* The ADC input mixers are plain DAPM mixers: one bit per input in `L_ADCMIX_SRC` / `R_ADCMIX_SRC`
  (MIC1 bit 6, MIC2 bit 5, LINEIN bit 2, output mixers bits 1/0), summed, no mux, no event that clears
  another input. Nothing in the driver changes these switches behind user space.
* The DAC joins the output mixers through `Left/Right Output Mixer DACL/DACR Switch`. The line out is
  mono: both LINEOUT muxes take the sum of the left and right output mixers.

### The ADC always has a source

With no input switched into the ADC input mixers, the codec's DAPM powers the ADC down while the
record daemon captures. In juricabi.1 that happened when nothing was recorded (every Record switch
No, or Record Headphone Mix with nothing heard): the capture stalled, and stopping a recording waited
for its timeout. The owner's `dvr_dbg.log` of juricabi.1 shows about 2 s from `record_stop begin` to
`record_close begin` on every stop, under a second on ripples 1.2.2. Now, with nothing to record, the
mic stays switched into the ADC and `ADC volume` is 0 (-119.25 dB, the dump's `dBscale-min`): a silent
recording with a powered ADC. A change of the Record switches never leaves the ADC without a source in
between (the batch order, section "What the firmware does"; `test_mixer.py` transitions).

### Per-input record levels: what the codec allows

Per input there are only the preamps (Mic Boost, Line In Boost; A/V In has none) and the output
mixer gains, which only reach the recording through the output mixers. The direct paths into the
ADC are plain on/off switches, summed in front of the one ADC input gain. So the inputs recorded
together share Record Level. juricabi.1's Record Headphone Mix recorded the output mixers, which gave
per-input record levels at the price of recording exactly what was heard; juricabi.2 does not offer
it. The codec cannot sum or average the two ADC sides through the driver (the AIF1OUT0 SUM / AVE mux
items have no routes), and there is no digital cross-mix. A split-stereo recording (one group left,
the other right, own digital levels) is possible but not implemented. A mono mix with independent
levels needs a kernel change or a software mix in the record daemon. The codec model reproduces
this, and the test checks it.

## What the firmware does

`src/core/audio_mixer_routing.c` turns the state into one `amixer -q -s` batch, with every routing
control set explicitly. `src/core/audio_mixer.c` writes the batch to `/tmp/audio_mixer.cmd` and runs
`amixer -q -s < /tmp/audio_mixer.cmd`. Applying is asynchronous: callers mark the state as changed,
and a worker thread writes the latest state. A burst of dial steps therefore ends in one or two amixer
runs, and the input thread never waits for amixer. Two callers wait for the codec: the recording
start (`dvr_update_record_conf()`, so a recording never starts with the previous routing) and HDMI-in
audio off (before the bridge stops playing into the DAC). ripples' `dvr_select_audio_source()` and
`dvr_enable_line_out()` keep their call sites (every source switch) and now apply the mixer.

The batch is ordered so that a change never disconnects everything in between. A line out that goes
off is switched off first. Then come the gains, every switch that ends up on, and the line out
(switched on last when it is on). Only then come the switches that end up off, and the output mixer
gains of the sources that are off, back to 0 dB.

The mixer is applied at boot, on every change on the Audio page, at the recording start and on every
source switch, after DVR playback and after a test, and when HDMI-in audio starts and stops.

### DVR playback and the test buttons: ripples' code, the mixer held

DVR playback is ripples' own code. `player/adec2ao.c` mutes the live audio (`dvr_mute_live_audio()`)
and sets the DVR Playback volume, so playback runs as on ripples, which plays the file's left channel
through libmedia_mpp's route. `ui_player.c` holds the mixer from before `media_init()` (whose
`AW_MPI_SYS_Init` resets part of the mixer) until after `media_exit()`. The first hold writes one
small batch: the output mixers' switches of the mic, Line In, A/V In and the DAC, and the output
mixers' switches into the ADC, all off (`audio_mixer_build_held_commands()`): without it a mic or Line
In level set on the page was heard with the file (the review of phase 3). While held, the mixer
writes no batch and does not watch the codec. Released, it applies its state and waits for the codec;
that batch also sets the AIF1 DAC timeslot volumes back to 0 dB, which ripples' DVR Playback volume sets
for a file or a test (HDMI-in audio plays through timeslot 0). The four test buttons do the same around
ripples' test code. Each holder (the player, the tests) has its own bit: a test that ends while a file
plays does not end the player's hold. They use ripples' raw routing of one source
(`dvr_test_select_audio_source()`) and ripples' line out (`dvr_test_line_out()`), kept in `dvr.c` for
them.

### Headphones at boot (as on ripples)

ripples set the headphone routing only when it switched to an analog source (`dvr_enable_line_out(true)`).
Booting straight into HDZero (or HDMI in) left the record daemon's mixer in place, whose line out has
only the idle DAC, so the headphones were silent until the first switch to an analog source. The mixer
does the same while `[mixer] phones_at_boot` = 0 (every setting.ini whose headphone levels were never
changed). The first change of an input's Headphones level (Mic, Line In, A/V In, HDMI In) sets
`phones_at_boot` = 1 and turns the line out on at once: from then on the headphones are live from boot,
in every source except HDMI in without HDMI-in audio. Headphone Volume does not count, because ripples'
slider never switched the line out on.

### libmedia_mpp resets part of the mixer

`AW_MPI_SYS_Init()` calls `audioHw_Construct()` -> `alsaOpenMixer()`. That sets `Lineout`, `DACL Mixer
AIF1DA0L`, `Left Output Mixer DACL`, `AIF1 AD0L Mixer ADCL` and `LADC input Mixer MIC1 boost` on,
`lineout volume` 27, `DAC volume` 160, `LINEOUTL/R Mux` OMIX_LR_SUM, `MIC1 boost AMP gain control` 4,
`ADC input gain control` 3 and `ADC volume` 160. It runs when the record daemon starts (rc.sh;
chkfixsd.sh / formatsd.sh: the automatic SD check after boot, the SD repair and format of the Storage
page) and in the app for every DVR file played. The mixer's worker watches the codec: while it has
nothing to apply, it reads the codec back every 250 ms, straight from `/dev/snd/controlC0` with ALSA
control ioctls (`audio_mixer_codec.c`, no process). Each pass reads the 12 controls of the reset; every
8th pass (2 s) reads every control of the last batch. It applies the current state again as soon as
one differs (log: `codec changed by someone else`). After the Storage page's SD repair or format
(`audio_mixer_recheck()`) it looks at once and then every 50 ms for 6 s. A codec that does not keep a
value, one that cannot be read, or an amixer that fails is logged and retried with a backoff (2 s,
doubling, at most 60 s); the mixer never gives up. It does not watch while it is held (playback, a
test). The emulator has no codec, so it does not watch.

The mixer is the only part of the firmware that writes these controls, apart from ripples' playback and
test code while it is held. The HDMI-in audio code tells the mixer (`audio_mixer_set_live_dac()`) instead
of running amixer.

### HDMI-in audio (HDMI In Audio = On)

The record daemon's bridge plays HDMI-in audio into the DAC while HDMI in is shown
(docs/hdmi-audio.md). `hdmi_audio_line_out()` in `src/core/hdmi_audio.c` calls
`audio_mixer_set_live_dac(true)` when the bridge route starts and `false` (waiting for the codec)
before it stops. Then DACL -> left and DACR -> right output mixer at the HDMI In level (1..8 =
-4.5..+6 dB: `DAC volume` 152 + 2 x (level - 4), 146..160; default 0 dB). The other live sources stay,
and the line out stays on although HDMI in is the source (Headphone Volume permitting). It is heard
only. With HDMI In Audio off (the default) nothing calls it: HDMI in mutes the line out as on ripples,
and the HDMI In row is greyed out.

## Settings

The page's own values are in `[mixer]` (version 2, the keys of all's firmware plus `rec_level`),
ripples' in `[record]`. The mixer takes ripples' values from `settings_load()` (`g_setting.record`) and
hands back what it filled in.

* **Fresh setting.ini**: ripples' defaults. DVR Audio Yes, DVR Playback 8, Headphone Volume 10, Mic
  Boost 4, Line In Boost 3. As ripples set the codec: Record Mic only, Record Level 0 dB, A/V In heard
  at +6.0 dB (ripples' curve of its Live Volume 10), Mic and Line In not heard, receiver audio off, the
  headphones silent at boot until the first analog source. Nothing is written.
* **From ripples 1.2.2** (no `[mixer]`): ripples' values as ripples read them. Then: the Audio Source's
  input recorded, Record Level as ripples had the ADC (Line/AV Gain with Line In or A/V In, 0 dB with
  the mic), A/V In heard by Live Volume's curve, receiver audio with A/V In. The codec gets what ripples
  set (`test_mixer.py` "migration" compares the two for every Audio Source and several Live Volumes and
  gains). Nothing is written.
* **From juricabi.1** (`[mixer]` version 2): its values. Record Level, which juricabi.1 did not have, is
  what the ADC had there: Line/AV Gain when more than the mic was recorded, else 0 dB. Record Headphone
  Mix (`rec_mix=1`) recorded what was heard: each input heard then (its level above Off) is now recorded
  directly. `rec_mix` is removed, and the Record switches it changed are written, with the next change
  on the page. Nothing is written at the start.
* **From all's firmware** (`[mixer]` version 2, none of ripples' keys): its `[mixer]` values stay in
  effect (as from juricabi.1, its Record Headphone Mix too). ripples' values come from all's equivalents
  where ripples' keys are missing: Headphone Volume from all's `headphones` (`(h * 10 + 15) / 31`), Mic
  Boost from `mic_boost`, Line In Boost from `line_in_boost`, Record Level from all's record level
  `dvr_level`, DVR Playback from all's `playback` (the lowest level whose line-out volume is at least as
  loud; all's DAC was 6 dB lower, so only roughly the same). Nothing is written. all's own keys
  (`headphones`, `playback`, `mic_boost`, `line_in_boost`, `dvr_level`) are never written or removed.
* **Back to ripples 1.2.2**: ripples reads its own keys as this firmware wrote them (`audio_source`: the
  first recorded input; `live_audio_volume`, `mic_gain`, `linein_gain`, `audio`) and ignores `[mixer]`
  and `[hdmi]`.
* **A `[mixer]` of all's unreleased version 1, or one cut short by a power loss** before its version was
  written: ripples' state, the section removed (one write).
* **juricabi, then stock ripples, then juricabi again**: every `[mixer]` this firmware writes also holds
  the `[record]` values its values belong to, `record_source` (ripples' Audio Source) and `record_live`
  (Live Volume). When stock ripples changed `[record] audio_source` since, the Record switches, Record
  Level and the receiver's audio follow it as ripples recorded; when it changed `live_audio_volume`, A/V
  In follows ripples' curve of it. In memory, nothing written at the start; the next change on the page
  writes them. What ripples did not change stays as the page left it.
* **Leaving the page without a change** writes nothing. A change writes the changed keys (`[mixer]`
  ones with `version=2` last, ripples' in `[record]`), the `[record]` values the `[mixer]` values belong
  to, and a `[mixer]` value that ripples' `[record]` values would otherwise change at the next start.

## Targets

* Goggle 2: A/V = LINEIN = AV-in jack, module bay and the internal receiver.
* Goggle (1): no internal receiver, A/V = AV-in jack / module bay.
* BoxPro: same codec and the same three inputs, internal receiver like Goggle 2. Same page, smaller
  rows.
* The HDMI In row and the HDMI Audio section exist on every target. **HDMI audio stays Off on all three
  until the read-only probe has run on the device**, on the BoxPro as on the goggles: its paths are
  unproven (docs/hdmi-audio.md, sections 1 and 7).

## Differences from ripples 1.2.2

For a goggle that is updated and never has a row changed, `utilities/audio_mixer/test_mixer.py` checks
that recording, headphones (sources and gains), line out power and the analog receiver's audio switch
are what ripples produced. It does so for every Audio Source and several Live Volumes and gains, after
a source switch, with HDMI in, at the recording start and when booting straight into HDZero. What
differs:

* The page itself (above). ripples' Audio Source and Live Volume rows are gone; their keys stay.
* The record daemon's mixer reset (boot, SD check, SD format): ripples kept it until the next source
  switch or recording start; now it is undone within about 2 s. Booting straight into HDZero, the line
  out is switched off instead of left on with nothing audible connected (silent either way).
* Headphone Volume 0: ripples left the line out switch on at volume 0; now it is off (silent either way).
* The preamps and the ADC input gain are always set. ripples set them only for the selected source, so
  after a source switch they could keep an earlier source's value. Nothing changes for the source that
  is recorded.
* The mic and Line In output mixer switches are always set (off unless a level is set). ripples left
  them as the board booted.
* The LINEOUT muxes stay at OMIX_LR_SUM while the line out is off. ripples' `out_off` set them to their
  first item, which has no effect while the line out is off.
* Sliders apply while they move; ripples applied them on release.
* The AIF1 DAC timeslot volumes are 0 dB outside DVR playback and the tests; ripples left them at the
  last DVR Playback volume (no difference to hear).
* Changes are click-free where the batch can make them so; the old scripts switched one control per
  amixer process.

## Audio page layout

The page has its own columns (`conf/ui.h` `UI_AUDIO_*`, per target): indent, label, slider, value,
record, without column gaps, and rows as tall as on the other pages (60 px goggles, 40 px BoxPro). The
first heading carries the titles of the Headphones and Record columns. A switch row has its two buttons
in the slider column; ripples' test buttons span the slider, value and record columns. The list scrolls
in a window of 10 rows. Below it is the hint for the selected cell: one line, or ripples' four-line note
of the test buttons in ripples' font.

`utilities/audio_mixer/check_strings.py` measures every string of the page in every language with the
firmware's LVGL fonts (built for the host) against its column, on the goggles and the BoxPro, and checks
that the font has every character. The CJK parts of the fonts hold only the characters of the
translations they were made from (`utilities/font/generate_font_lib.py`), so new Chinese and Japanese
strings use those characters. Every string is translated. ripples' note is a key per line; line 3 is
keyed with '-' for its '=' (a key of a language file cannot hold '='; `note_line()`).

## Testing on a goggle

The host tests cover the routing logic; these need the hardware.

1. Update from ripples 1.2.2, boot into HDZero, do not open the Audio page: the headphones are silent as
   on ripples, and a clip records ripples' Audio Source as before. Log: `audio mixer: from
   record/audio_source=N live_audio_volume=M`.
2. Audio page: Mic Record Yes, A/V In Record Yes: a clip has both. Mic Headphones Off: the mic is not
   heard, still recorded.
3. Every Record switch No: a clip is silent; stopping it (DVR button, menu) takes no longer than with a
   source recorded (`dvr_dbg.log`: `record_stop begin` to `record_close begin` within a second).
4. Record Level up and down: the recording louder and quieter, the headphones unchanged.
5. Analog receiver (Goggle 2, BoxPro): A/V In Off and Record No: no receiver audio; either on: receiver
   audio.
6. The four test buttons: as on ripples. Afterwards the headphones and the recording are as before the
   test (`amixer contents` matches `/tmp/audio_mixer.cmd`). With Stream Audio streaming: the Mic test says
   "Nothing recorded: audio input busy" (or records: then note it).
7. DVR playback: as on ripples (left channel, DVR Playback volume), also with Mic or Line In heard on the
   page: neither is heard with the file. After it the live sources come back.
8. Record daemon reset: start a recording after an SD repair or format on the Storage page. Within about
   2 s the log shows `codec changed by someone else`, and `amixer contents` matches `/tmp/audio_mixer.cmd`.

## Checking without hardware

`utilities/audio_mixer/test_mixer.py` (python3 and a host C compiler, about 90 s) builds ripples' real
`src/core/settings.c` and the real `src/core/audio_mixer.c` and `audio_mixer_routing.c` for the host
three times (`-DHDZGOGGLE`, `-DHDZGOGGLE2`, `-DHDZBOXPRO`). It uses a harness (`harness/harness.c`: real
minIni and setting.ini, `settings_load()` then the mixer as `main.c`, the worker thread, a simulated codec
that takes the batches) and checks the results against `codec_model.py`, a model of the codec's signal
graph made of the driver's DAPM routes. It asserts:

* routing: a matrix of states (each input heard and recorded on its own, several recorded, nothing
  recorded: the mic at the minimum ADC volume, Headphone Volume 0, HDMI in, out-of-range values, HDMI-in
  audio heard and never recorded). Every batch line is a control of the device's dump with a value in
  range, set once, and never a digital DAC/ADC loopback.
* loading: clamping, missing fields, a version 1 section, Headphone Volume leaving A/V In, ripples' Audio
  Source and the receiver's audio following the Record switches and A/V In, the first change.
* settings both ways: stock ripples (several Audio Sources, Live Volumes and gains), a fresh setting.ini,
  five setting.ini files of all's firmware, juricabi.1 (with and without Record Headphone Mix), back to
  stock ripples. It checks the values in effect and in `g_setting.record`, that nothing is written at
  start-up or by a save without a change, and that all's own keys are still there after a change.
* hold: ripples' DVR playback with Mic and Line In heard: only the file is heard, nothing recorded
  through the output mixers, the timeslot volumes back afterwards; the Mic test records the mic alone; a
  test that ends while a file plays does not end the player's hold.
* the round trip juricabi -> stock ripples -> juricabi.
* migration equivalence against ripples' firmware: libmedia_mpp's reset, then ripples' `dvr.c` sequences
  with the real `mkapp/app/script/audio_sel.sh`, after a source switch, with HDMI in, at the recording
  start, booting straight into HDZero, and with the record daemon resetting the codec after the app has
  routed it.
* headphones at boot, transitions (a change never drops the line out, the ADC's source or a recorded
  input in between), the worker (never blocks, coalesces, writes setting.ini only on save, clamps), hold,
  the watcher, and a record daemon restart while the line out is off.

`--driver <BSP>/sound/soc/sunxi/sun8iw16-codec.c` also checks that every modelled route is one of the
driver's. `utilities/audio_mixer/mutants.py` applies small mutations to a copy of the sources and fails
unless the test catches every one (docs/status.md has the count of the last run). `check_strings.py`
checks the page's strings (above).

`utilities/audio_mixer/stub/amixer` logs what it is given; put it first in `PATH` (with `AMIXER_LOG`) to
see the batches the emulator sends. On a goggle, `/tmp/audio_mixer.cmd` holds the last batch and `amixer
contents` shows the result.

Proven only on the host or in the emulator: the routing logic against a model of the driver, the order of
every batch, the settings, and the page (layout, scrolling, strings, widths). What needs a real goggle:
everything audible (levels, clicks, noise), the ADC's power with nothing recorded, the record daemon's
reset timing, ripples' playback and tests after the mixer, and every HDMI audio path.
