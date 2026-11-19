# Audio: the Audio page and the codec (juricabi's firmware)

All three targets (HDZero Goggle, Goggle 2, BoxPro) use the internal audio codec of the Allwinner
V536 (`sun8iw16`, ALSA card `audiocodec`). The firmware drives it with `amixer`; the record daemon
captures the ALSA `default` device (48 kHz stereo, AAC). The control names and ranges are the ones
of the device's `amixer contents` dump in `mkapp/app/app/record/audio/audiocodec/alsa-util/`, their
meaning comes from the Allwinner driver (`sound/soc/sunxi/sun8iw16-codec.c` of the V536 BSP).

In juricabi's firmware the Audio page is **ripples' page** (firmware 1.2.2: Record Audio, Audio Source,
DVR Playback, Live Volume, Mic Gain, Line/AV Gain, the test buttons; its setting.ini keys) with the
**extras of all's audio mixer** (082b2f5, this project's earlier firmware) below it. docs/status.md,
phase 3, lists every overlap and how it was resolved.

## In short

* ripples' rows work as on ripples 1.2.2. Their keys stay `[record] audio`, `audio_source`,
  `dvr_audio_volume_v2`, `live_audio_volume`, `mic_gain`, `linein_gain`.
* **Headphones**: every source has its own level in the analog mixer that feeds the headphone
  jack (Mic, Line In, A/V In, HDMI In). 0 is off. ripples' Live Volume is the volume of the
  jack, and moving it also sets the A/V In level by ripples' curve, as ripples did.
* **Recording**: one ADC records, and the same capture feeds the DVR, the WiFi stream audio
  (Stream Audio) and HDMI out audio (HDMI Out Audio). It takes
  * each analog source straight from its input at full level ("Record Mic", "Record Line In",
    "Record A/V In"): ripples' Audio Source is the preset that sets exactly one of them, and/or
  * the headphone mix at its levels ("Record Headphone Mix").

  A source that is already in the recorded headphone mix is not taken a second time through its
  direct path; its Record row shows "Mix".
* HDMI-in audio can only be recorded through the headphone mix: there is no direct path from the
  DAC into the ADC.
* A goggle coming from ripples 1.2.2 behaves as before until a row of the extras is changed.
  One coming from all's firmware keeps what its Audio page set. A fresh install gets ripples'
  defaults. Leaving the page without a change writes nothing (section "Settings").
* DVR playback and the test buttons run ripples' own codec code. The mixer is held meanwhile: it
  first takes its own live paths out of the codec's mixers (the headphone sources of the page and
  the recorded mix), so that only what ripples' code routes is heard or recorded, and it sets its
  whole state again afterwards.

## The Audio page

One page, scrolled so that the selected row is always on screen (a scroll bar on the right shows
where the window is), with the help of the selected row below the list (ripples' note when Run
Test is selected).

| Section | Row | Values | setting.ini | Default (fresh) |
|---|---|---|---|---|
| (ripples) | Record Audio | Yes / No | `[record] audio` | Yes |
| | Audio Source (greyed and skipped while Record Audio is No, as on ripples) | Mic / Line In / A/V In | `[record] audio_source` | Mic |
| Volume Control (ripples) | DVR Playback | 0..8 | `[record] dvr_audio_volume_v2` | 8 |
| | Live Volume (hint: "Headphone volume. Also sets the A/V In level") | 0..10 | `[record] live_audio_volume` | 10 |
| | Mic Gain | 0..7 | `[record] mic_gain` | 4 |
| | Line/AV Gain | 0..7 | `[record] linein_gain` | 3 |
| | Run Test | DVR, Live, Mic, Line/AV | - | - |
| Headphones | Mic | Off, -4.5..+6.0 dB | `[mixer] mic` 0..8 | Off |
| | Line In | Off, -4.5..+6.0 dB | `[mixer] line_in` 0..8 | Off |
| | A/V In | Off, -4.5..+6.0 dB | `[mixer] av` 0..8 | by Live Volume (10: +6.0 dB) |
| | HDMI In (greyed while HDMI In Audio is not in effect) | Off, -4.5..+6.0 dB | `[mixer] hdmi` 0..8 | 0.0 dB |
| Recording (DVR, WiFi Stream, HDMI Out) | Record Headphone Mix | Yes / No | `[mixer] rec_mix` | No |
| | Record Mic | Yes / No, "Mix" | `[mixer] rec_mic` | by Audio Source |
| | Record Line In | Yes / No, "Mix" | `[mixer] rec_line_in` | by Audio Source |
| | Record A/V In | Yes / No, "Mix" | `[mixer] rec_av` | by Audio Source |
| Inputs (Goggle 2, BoxPro) | Built-in RX Audio | On / Off | `[mixer] rx_audio` | On only with Audio Source A/V In |
| HDMI Audio (experimental; from the next start) | HDMI Out Audio (On only with the HDMI audio probe's GO for this board, docs/hdmi-audio.md section 7) | On / Off | `[hdmi] audio_out` | Off |
| | HDMI In Audio | On / Off | `[hdmi] audio_in` | Off |
| | < Back | | | |

What each of ripples' rows does on the codec:

* **Audio Source** is the preset of the Recording section: Mic records the mic directly and not
  Line In and A/V In (ripples: `audio_sel.sh in_mic1`), and so on. It also switches Built-in RX
  Audio on with A/V In only and off otherwise, as ripples did with `rtc6715.init(1, audio_source ==
  A/V In)`. Record Headphone Mix stays as it is. When the Record rows below record anything else,
  the row's hint says "Changed in the Recording section below"; the buttons show the last choice.
* **Live Volume** is the line-out volume, `(v * 31 + 5) / 10` (0 switches the line out off).
  Moving it also sets the A/V In level by ripples' curve, register `(v^3 * 7 + 500) / 1000` and at
  least 1 above 0, as ripples' `dvr_set_live_audio_volume()` did. It does not make the headphones
  live from boot, because ripples' slider never switched the line out on. A/V In can be trimmed
  on its own afterwards.
* **Mic Gain** is the MIC1 preamp. ripples set it only with Mic as the source; here it is always
  set, and it acts only on the mic.
* **Line/AV Gain** is the MIC2 preamp (the Line In jack) and the ADC input gain, the level of the
  whole recording. ripples set both only with Line In or A/V In as the source, and the ADC input
  gain stayed at the record daemon's 0 dB with Mic. Here the ADC input gain is Line/AV Gain
  whenever more than the mic alone is recorded (Line In, A/V In or the headphone mix), else 0 dB.
* **DVR Playback** is ripples' playback volume (line-out table and DAC volume), used by ripples'
  playback code (`player/adec2ao.c`) and the DVR test as before.
* **Record Audio** is unchanged: whether the DVR records audio at all.
* **Run Test**: ripples' four tests. They run ripples' own codec code: the mic or line into the ADC,
  the DAC to the line out. The mixer is held during a test (the page's own live paths out first:
  a Mic test records the mic alone, also with Record Headphone Mix on), and afterwards it puts the
  page's routing back. ripples instead restored its source and its line out itself. A Mic or
  Line/AV test that records nothing (the audio input held by Stream Audio or HDMI Out Audio, whose
  capture the record daemon has open) says so in place of the note: "Nothing recorded: audio input
  busy (Stream Audio, HDMI Out Audio)"; ripples played back nothing without a word.

Sliders: click to grab, turn, click to release; the value applies while it moves (ripples applied
on release). DVR Playback is stored on release, as on ripples, and used by the next playback.
Switches apply on click. setting.ini is written on a switch click, a slider release, leaving the
page, and 1 s after the last dial step of a slider still grabbed. Only what changed is written.

## Signal paths

```
 codec input           preamp            output mixer gain     L/R OUTPUT MIXER          line out
 MIC1  built-in mic ──[MIC1 boost 0..7]─┬─[MIC1_G      -4.5..+6 dB]─sw─┐
 MIC2  "Line In" jack [MIC2 boost 0..7]─┼─[MIC2 BST    -4.5..+6 dB]─sw─┤
 LINEIN A/V (L/R) ──────────────────────┼─[LINEIN gain -4.5..+6 dB]─sw─┼──> L+R sum ─[lineout volume 0..31]─sw─> jack
 DAC   playback / HDMI-in audio ────────┼──────────────────────────sw─┘      (LINEOUTL/R Mux = OMIX_LR_SUM)
                                        │                                │
           Record Mic / Line In / A/V In│  sw: MIC1, MIC2, LINEIN        │ sw: "l_output/r_output mixer"
                                        v                                v     (Record Headphone Mix)
                          L/R ADC INPUT MIXER ─[ADC input gain -4.5..+6 dB]─ ADC ─[ADC volume, digital]─ AIF1
                                                                                  ─> record daemon: DVR, stream, HDMI out
```

* MIC1 boost = Mic Gain; MIC2 boost = Line/AV Gain; the output mixer gains = the Headphones levels
  (LINEIN also by Live Volume); `lineout volume` = Live Volume; `ADC input gain control` =
  Line/AV Gain (see above).
* The ADC input mixers are plain DAPM mixers: one bit per input in `L_ADCMIX_SRC` /
  `R_ADCMIX_SRC` (MIC1 bit 6, MIC2 bit 5, LINEIN bit 2, output mixers bits 1/0), summed, no mux,
  no event that clears another input. So the output mixer and the direct inputs can be on
  together. Nothing in the driver changes these switches behind user space.
* Per source: only the preamps and the three output mixer gains. The preamps act on both paths;
  the output mixer gains only on the way into the output mixer. The direct paths into the ADC are
  plain on/off switches; LINEIN has no gain of its own in front of the ADC. That is why a direct
  source is recorded "at full level" and why per-source record levels exist only through the mix.
* Record Headphone Mix takes the output mixers into the ADC: the left ADC takes the left output
  mixer, the right ADC the right one. The recording is stereo and taken before the L+R sum and
  before `lineout volume`, while the headphones get left + right of both mixers (mono).
* The DAC joins through `Left/Right Output Mixer DACL/DACR Switch`. The line out is mono: both
  LINEOUT muxes take the sum of the left and right output mixers.

### When a source is in the mix and recorded directly

A source counts as "in the recorded mix" when Record Headphone Mix is on and its level is above 0.
Then its direct ADC switch stays off. When a level goes from 0 to above 0 (or back), the batch
switches the mix path on before the direct path off (or the other way round). The source is never
missing from the recording in between; at most it is doubled for the few milliseconds of the
batch. `test_mixer.py` checks this per transition.

## What the firmware does

`src/core/audio_mixer_routing.c` turns the state into one `amixer -q -s` batch, with every routing
control set explicitly. `src/core/audio_mixer.c` writes the batch to `/tmp/audio_mixer.cmd` and
runs `amixer -q -s < /tmp/audio_mixer.cmd`. Applying is asynchronous: callers mark the state as
changed, and a worker thread writes the latest state. A burst of dial steps therefore ends in one
or two amixer runs, and the input thread never waits for amixer. Two callers wait for the codec:
the recording start (ripples' DVR worker, `dvr_update_record_conf()`, so a recording never starts
with the previous routing) and HDMI-in audio off (before the bridge stops playing into the DAC).
ripples' `dvr_select_audio_source()` and `dvr_enable_line_out()` keep their call sites (every
source switch, the recording start) and now apply the mixer.

The batch is ordered so that a change never disconnects everything in between. A line out that
goes off is switched off first. Then come the gains, every switch that ends up on, and the line
out (switched on last when it is on). Only then come the switches that end up off, and the output
mixer gains of the sources that are off, back to 0 dB.

The mixer is applied at boot, on every change on the Audio page, at the recording start and on
every source switch, after DVR playback and after a test, and when HDMI-in audio starts and stops.

### DVR playback and the test buttons: ripples' code, the mixer held

DVR playback is ripples' own code. `player/adec2ao.c` mutes the live audio
(`dvr_mute_live_audio()`) and sets the DVR Playback volume, so playback runs as on ripples, which
plays the file's left channel through libmedia_mpp's route. `ui_player.c` holds the mixer from
before `media_init()` (whose `AW_MPI_SYS_Init` resets part of the mixer) until after
`media_exit()`. The first hold writes one small batch: the output mixers' switches of the mic,
Line In, A/V In and the DAC, and the output mixers' switches into the ADC, all off
(`audio_mixer_build_held_commands()`). ripples' playback mutes only LINEIN (`dvr_mute_live_audio()`)
and libmedia_mpp's reset touches none of the others but the left DAC switch, which it sets again for
the file: without the held batch a mic or Line In level set on the page was heard with the file (the
review of phase 3). Then, while held, the mixer writes no batch and does not watch the codec.
Released, it applies its state and waits for the codec; that batch also sets the AIF1 DAC timeslot
volumes back to 0 dB, which ripples' DVR Playback volume sets for a file or a test (HDMI-in audio
plays through timeslot 0). The four test buttons do the same around ripples' test code. Each holder
(the player, the tests) has its own bit: a test that ends while a file plays does not end the
player's hold. They use ripples' raw routing of one source (`dvr_test_select_audio_source()`) and ripples'
line out (`dvr_test_line_out()`), kept in `dvr.c` for them. all's mixer drove playback itself, with
both DAC channels. ripples found the output mixer's DAC path tied to static on the Goggle 1, so its
playback path is kept.

### Headphones at boot (as on ripples)

ripples set the headphone routing only when it switched to an analog source
(`dvr_enable_line_out(true)`). Booting straight into HDZero (or HDMI in) left the record daemon's
mixer in place, whose line out has only the idle DAC, so the headphones were silent until the
first switch to an analog source. The mixer does the same while `[mixer] phones_at_boot` = 0
(every setting.ini without the extras changed). The first change of a source's headphone level on
the page (Mic, Line In, A/V In, HDMI In) sets `phones_at_boot` = 1 and turns the line out on at
once: from then on the headphones are live from boot, in every source except HDMI in without
HDMI-in audio. Live Volume does not count, because ripples' slider never switched the line out on.

### Analog receiver audio

The built-in analog receiver (RTC6715 on Goggle 2 and BoxPro) has an audio output of its own,
switched by the receiver itself. The "Built-in RX Audio" row (`[mixer] rx_audio`) is used by every
`rtc6715.init(1, ...)` for the live picture (source change and head-tracker wake-up). ripples
switched it with the Audio Source (on with A/V In); the Audio Source preset still does, and the row
can change it afterwards. The Analog RSSI page keeps the receiver's audio off, as before.

### libmedia_mpp resets part of the mixer

`AW_MPI_SYS_Init()` calls `audioHw_Construct()` -> `alsaOpenMixer()`. That sets `Lineout`,
`DACL Mixer AIF1DA0L`, `Left Output Mixer DACL`, `AIF1 AD0L Mixer ADCL` and
`LADC input Mixer MIC1 boost` on, `lineout volume` 27, `DAC volume` 160, `LINEOUTL/R Mux`
OMIX_LR_SUM, `MIC1 boost AMP gain control` 4, `ADC input gain control` 3 and `ADC volume` 160. It
runs when the record daemon starts (rc.sh; chkfixsd.sh / formatsd.sh: the automatic SD check after
boot, the SD repair and format of the Storage page) and in the app for every DVR file played. The
mixer's worker watches the codec: while it has nothing to apply, it reads the codec back every
250 ms, straight from `/dev/snd/controlC0` with ALSA control ioctls (`audio_mixer_codec.c`, no
process). Each pass reads the 12 controls of the reset; every 8th pass (2 s) reads every control
of the last batch. It applies the current state again as soon as one differs (log: `codec changed
by someone else`). After the Storage page's SD repair or format (`audio_mixer_recheck()`) it looks
at once and then every 50 ms for 6 s. A codec that does not keep a value, one that cannot be read,
or an amixer that fails is logged and retried with a backoff (2 s, doubling, at most 60 s); the
mixer never gives up. It does not watch while it is held (playback, a test). The emulator has no
codec, so it does not watch.

The mixer is the only part of the firmware that writes these controls, apart from ripples' playback
and test code while it is held. The HDMI-in audio code tells the mixer (`audio_mixer_set_live_dac()`)
instead of running amixer.

### HDMI-in audio (HDMI In Audio = On)

The record daemon's bridge plays HDMI-in audio into the DAC while HDMI in is shown
(docs/hdmi-audio.md). `hdmi_audio_line_out()` in `src/core/hdmi_audio.c` calls
`audio_mixer_set_live_dac(true)` when the bridge route starts and `false` (waiting for the codec)
before it stops. Then DACL -> left and DACR -> right output mixer at the HDMI In level (1..8 =
-4.5..+6 dB: `DAC volume` 152 + 2 x (level - 4), 146..160; default 0 dB). The other live sources
stay, and the line out stays on although HDMI in is the source (Live Volume permitting). Record
Headphone Mix records it; the direct Record switches cannot. With HDMI In Audio off (the default)
nothing calls it: HDMI in mutes the line out as on ripples, and the HDMI In row is greyed out. In
phase 2 this route used ripples' `audio_sel.sh` at the DVR Playback volume, not recorded. It is
now the mixer's, as on all.

### Per-source record levels: what the codec allows (evaluated, not implemented)

With this driver, per-source record levels exist only through the headphone mix (Record Headphone
Mix: the recording follows the headphone levels), plus the direct inputs at one shared ADC input
gain. The codec cannot sum or average the two ADC sides through the driver (the AIF1OUT0 SUM / AVE
mux items have no routes), and there is no digital cross-mix. A split-stereo recording (one group
left, the other right, own digital levels) is possible but not implemented. A mono mix with
independent levels needs a kernel change or a software mix in the record daemon. The codec model
reproduces this, and the test checks it.

## Settings

The extras are stored in `[mixer]`, with the keys and meaning of all's firmware (version 2). ripples'
rows keep ripples' keys in `[record]`. The mixer takes ripples' values from `settings_load()`
(`g_setting.record`) and hands back what it filled in.

* **Fresh setting.ini**: ripples' defaults. Record Audio Yes, Audio Source Mic, DVR Playback 8,
  Live Volume 10, Mic Gain 4, Line/AV Gain 3. The extras as ripples set the codec: Record Mic
  only, no headphone mix, A/V In +6.0 dB (Live Volume 10), Mic and Line In off, receiver audio
  off, the headphones silent at boot until the first analog source. Nothing is written.
* **From ripples 1.2.2** (no `[mixer]`): ripples' values as ripples read them. The extras follow
  from them: Record <Audio Source> only, A/V In by Live Volume's curve, receiver audio with A/V
  In. The codec gets what ripples set (`test_mixer.py` "migration" compares the two for every
  Audio Source and several Live Volumes and gains). Nothing is written.
* **From all's firmware** (`[mixer]` version 2, none of ripples' keys): its `[mixer]` values
  stay in effect (levels, Record switches, headphone mix, receiver audio, HDMI In level,
  headphones at boot). ripples' values come from all's equivalents where ripples' keys are
  missing: Live Volume from all's `headphones` (`(h * 10 + 15) / 31`), Mic Gain from
  `mic_boost`, Line/AV Gain from all's Record Level `dvr_level`, DVR Playback from all's
  `playback` (the lowest level whose line-out volume is at least as loud; all's DAC was 6 dB
  lower, so only roughly the same). The Audio Source shows the one source recorded directly, if
  there is one. Nothing is written. all's own keys (`headphones`, `playback`, `mic_boost`,
  `line_in_boost`, `dvr_level`) are never written or removed, so all's firmware reads them as
  it left them.
* **Back to ripples 1.2.2**: ripples reads its own keys as juricabi's firmware wrote them and ignores
  `[mixer]` and `[hdmi]`.
* **A `[mixer]` of all's unreleased version 1, or one cut short by a power loss** before its
  version was written: ripples' state, the section removed (one write).
* **juricabi, then stock ripples, then juricabi again**: every `[mixer]` this firmware
  writes also holds the `[record]` values its values belong to, `record_source` (Audio Source) and
  `record_live` (Live Volume). When stock ripples changed `[record] audio_source` since, the
  Record switches and Built-in RX Audio follow the new Audio Source as its row would set them;
  when it changed `live_audio_volume`, A/V In follows the new Live Volume by ripples' curve. In
  memory, nothing written at the start; the next change on the page writes them. What ripples did
  not change stays as the page left it (the page's own Record switches, a trimmed A/V In). A
  `[mixer]` without these keys (all's firmware) is read as before.
* **Leaving the page without a change** writes nothing. A change writes the changed keys
  (`[mixer]` ones with `version=2` last, ripples' in `[record]`), the `[record]` values the
  `[mixer]` values belong to, and a `[mixer]` value that a changed `[record]` value would otherwise
  change at the next start (Audio Source Line In, then Record Mic Yes again: written, or the Line
  In preset would turn it off).

## Targets

* Goggle 2: A/V = LINEIN = AV-in jack, module bay and the internal receiver.
* Goggle (1): no internal receiver, A/V = AV-in jack / module bay; no Built-in RX Audio row.
* BoxPro: same codec and the same three inputs, internal receiver like Goggle 2. Same page,
  smaller rows.
* The HDMI In row and the HDMI Audio section exist on every target. **HDMI audio stays Off on all
  three until the read-only probe has run on the device**, on the BoxPro as on the goggles: its
  paths are unproven (docs/hdmi-audio.md, sections 1 and 7).

## Differences from ripples 1.2.2

For a goggle that is updated and never has an extra changed, `utilities/audio_mixer/test_mixer.py`
checks that recording, headphones (sources and gains), line out power and the analog receiver's
audio switch are what ripples produced. It does so for every Audio Source and several Live Volumes
and gains, after a source switch, with HDMI in, at the recording start and when booting straight
into HDZero. What differs:

* The record daemon's mixer reset (boot, SD check, SD format): ripples kept it until the next
  source switch or recording start; now it is undone within about 2 s. Booting straight into
  HDZero, the line out is switched off instead of left on with nothing audible connected (silent
  either way).
* Live Volume 0: ripples left the line out switch on at volume 0; now it is off (silent either way).
* The preamps and the ADC input gain are always set (Mic Gain, Line/AV Gain; the ADC input gain
  0 dB with the mic alone). ripples set them only for the selected source, so after a source switch
  they could keep an earlier source's value. Nothing changes for the source that is recorded.
* The mic and Line In output mixer switches are always set (off unless a level is set). ripples
  left them as the board booted.
* The LINEOUT muxes stay at OMIX_LR_SUM while the line out is off. ripples' `out_off` set them to
  their first item, which has no effect while the line out is off.
* Sliders apply while they move; ripples applied them on release.
* The AIF1 DAC timeslot volumes are 0 dB outside DVR playback and the tests; ripples left them at
  the last DVR Playback volume (no difference to hear: ripples played nothing else through them).
* Changes are click-free where the batch can make them so; the old scripts switched one control
  per amixer process.

## Audio page layout

The page has its own columns (`conf/ui.h` `UI_AUDIO_*`, per target): label, two button columns
(arrow + button; the sliders span both), value, without column gaps, and rows as tall as on the
other pages (60 px goggles, 40 px BoxPro). ripples' Audio Source row is its label and three
buttons, each as wide as its text, across the label, button and value columns. Some translations
of "Line In" are as wide as two button columns. ripples' test buttons share the button and value
columns. The list scrolls in a window of 10 rows. Below it is the hint for the selected row: one
line, or ripples' four-line note of the test buttons in ripples' font.

`utilities/audio_mixer/check_strings.py` measures every string of the page in every language with
the firmware's LVGL fonts (built for the host) against its column, on the goggles and the BoxPro,
and checks that the font has every character. The CJK parts of the fonts hold only the characters
of the translations they were made from (`utilities/font/generate_font_lib.py`), so new Chinese
and Japanese strings must use those characters (where a word has none, the Chinese note names the
test fill's colours in English). Every string is translated: all's translations of all's strings,
and since phase 4 ripples' own labels and its test note too (untranslated in ripples 1.2.2). The
note is a key per line; line 3 is keyed with '-' for its '=' (a key of a language file cannot hold
'='; `note_line()`).

## Testing on a goggle

The host tests cover the routing logic; these need the hardware. Before updating, note the
Audio Source and Live Volume, and if possible keep a short DVR clip and listen to the headphones
in each source, to compare with afterwards.

1. Update, boot into HDZero, do not open the Audio page: the headphones are silent as on ripples.
   `/mnt/app/setting.ini` has no `[mixer]`; `/tmp/audio_mixer.cmd` exists. Log: `audio mixer: from
   record/audio_source=N live_audio_volume=M`.
2. Record a clip in HDZero and, on Goggle 2 / BoxPro, with the built-in analog receiver: the same
   source as before is recorded at the same level.
3. Analog receiver (Goggle 2, BoxPro), Audio Source Mic or Line In: no receiver audio. Audio Source
   A/V In: receiver audio as before.
4. Headphones after a switch to analog: A/V (AV-in jack or module bay) as loud as before. Live
   Volume up and down: as before.
5. HDMI in: headphones silent; a clip records the selected source.
6. The four test buttons: as on ripples. Afterwards the headphones and the recording are as
   before the test (`amixer contents` matches `/tmp/audio_mixer.cmd`). With Mic and Record
   Headphone Mix on: the Mic test's clip has the mic alone. With Stream Audio streaming: the Mic
   test says "Nothing recorded: audio input busy" (or records: then note it).
7. DVR playback: as on ripples (left channel, DVR Playback volume), also with Mic or Line In set
   above Off on the page: neither is heard with the file. After it the live sources come back.
   With HDMI In Audio: HDMI-in audio as loud after a DVR file as before it.
8. Extras: set Mic to 0 dB: the mic is heard at once, and after a reboot into HDZero too. Record
   Headphone Mix = Yes: Record Mic shows "Mix"; a clip has the mic once, at its headphone level.
   Audio Source to Line In: Record Line In only, the hint unchanged; Record Mic Yes as well: the
   Audio Source hint says "Changed in the Recording section below", and a clip has both.
9. Record daemon reset: start a recording after an SD repair or format on the Storage page. Within
   about 2 s the log shows `codec changed by someone else`, and `amixer contents` matches
   `/tmp/audio_mixer.cmd` again. After a DVR file (the mixer held) it matches again as soon as the
   player is left.

## Checking without hardware

`utilities/audio_mixer/test_mixer.py` (python3 and a host C compiler, about 90 s) builds ripples'
real `src/core/settings.c` and the real `src/core/audio_mixer.c` and `audio_mixer_routing.c` for
the host three times (`-DHDZGOGGLE`, `-DHDZGOGGLE2`, `-DHDZBOXPRO`). It uses a harness
(`harness/harness.c`: real minIni and setting.ini, `settings_load()` then the mixer as `main.c`,
the worker thread, a simulated codec that takes the batches) and checks the results against
`codec_model.py`, a model of the codec's signal graph made of the driver's DAPM routes. It asserts:

* routing: a matrix of states (direct sources, the mix, both, every source, Live Volume 0, HDMI
  in, out-of-range values, HDMI-in audio), including ripples' controls. Every batch line is a
  control of the device's dump with a value in range, set once, and never a digital DAC/ADC
  loopback.
* loading: clamping, missing fields, a version 1 section, the "in the mix" marker, the Audio
  Source and Live Volume presets, and the first change.
* settings both ways: stock ripples (several Audio Sources, Live Volumes and gains), a fresh
  setting.ini, five setting.ini files of all's firmware (one with ripples' Live Volume from
  before it, which is kept), and back to stock ripples. It checks
  the values in effect and in `g_setting.record`. It checks that nothing is written by the
  mixer at start-up or by a save without a change, and that all's own keys are still there
  after a change.
* hold: ripples' DVR playback (libmedia_mpp's reset and `adec2ao_start()`'s commands) with Mic and
  Line In heard and Record Headphone Mix on: only the file is heard, nothing recorded through the
  output mixers, the timeslot volumes back afterwards; the Mic test records the mic alone; a test
  that ends while a file plays does not end the player's hold.
* the round trip juricabi -> stock ripples -> juricabi: Audio Source and Live Volume changed
  by ripples win, nothing written at the start, the next change writes them, what ripples did not
  change stays.
* migration equivalence against ripples' firmware: libmedia_mpp's reset, then ripples' `dvr.c`
  sequences with the real `mkapp/app/script/audio_sel.sh`, after a source switch, with HDMI in,
  at the recording start, booting straight into HDZero, and with the record daemon resetting the
  codec after the app has routed it.
* headphones at boot, transitions (a change never drops the line out or a recorded source in
  between), the worker (never blocks, coalesces, writes setting.ini only on save, clamps), hold
  (nothing written while held, also when hold() comes after the worker took a state but before it
  wrote it, through a hook of the test that is not built into the firmware, `AUDIO_MIXER_TEST_HOOKS`;
  the state applied on release, watched again), the watcher, and a record daemon restart while
  the line out is off.

`--driver <BSP>/sound/soc/sunxi/sun8iw16-codec.c` also checks that every modelled route is one of
the driver's. `utilities/audio_mixer/mutants.py` (about 50 min, 6 in parallel) applies 88 small
mutations to a copy of the sources and fails unless the test catches every one: all's that still
apply (their reviews' among them), ripples' controls, the presets, the settings both ways, hold,
and the fixes of the review of phase 3 (the held batch, the holders, the timeslot volumes, the
round trip). `check_strings.py` checks the page's strings (above).

`utilities/audio_mixer/stub/amixer` logs what it is given; put it first in `PATH` (with
`AMIXER_LOG`) to see the batches the emulator sends. On a goggle, `/tmp/audio_mixer.cmd` holds the
last batch and `amixer contents` shows the result.

In the emulator (`utilities/dev/emu_run.sh`, sandboxed), `utilities/dev/keys/audio_page.txt`
selects every row of the page in turn with a screenshot and leaves it without a change, and
`audio_change.txt` (Goggle 2, BoxPro) changes Mic, Record Headphone Mix, Audio Source and Live
Volume one step each and runs the DVR test, with a copy of setting.ini after each step. Both start
in the main menu: ripples' shipped setting.ini boots into a scan, which finds the emulator's mock
receiver on the Goggle 1, so its copy for these runs has `[autoscan] status=2`.

Proven only on the host or in the emulator: the routing logic against a model of the driver, the
order of every batch, the settings, and the page (layout, scrolling, strings, widths). What needs
a real goggle: everything audible (levels, clicks, noise, clipping of mixed sources), the record
daemon's reset timing, ripples' playback and tests after the mixer, and every HDMI audio path.
