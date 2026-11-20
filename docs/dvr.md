# Automatic DVR (juricabi's firmware)

The goggle starts and stops the DVR by itself in two ways, both ripples': **Record Mode Auto**
(Record Option page: a recording starts when the picture comes and stops when it goes) and the
**ELRS backpack's recording control** (on the radio: ExpressLRS Lua, Backpack, DVR Rec on arm, with a
start and a stop delay). juricabi.2 makes the two work together (`src/core/dvr_auto.c`).

## What the radio sends

From ExpressLRS's code (`src/lib/Backpack/devBackpack.cpp`) and the backpack's (1.5.9: `Tx_main.cpp`,
`Vrx_main.cpp`, `hdzero.cpp`):

* The TX module checks the DVR switch chosen in the Lua (or the arm state) every 20 ms and, **only when
  it changes**, sends one `MSP_ELRS_BACKPACK_SET_RECORDING_STATE`: `[state, delay low, delay high]`,
  state 1 = record, 0 = stop, the delay in seconds (0, 5, 15, 30, 45, 60 or 120: the start delay with a 1,
  the stop delay with a 0).
* It is **never repeated**, and nothing confirms it. The radio's backpack sends it over ESP-NOW, the
  goggle's backpack passes it on over the serial line as it came.
* So every command is **the switch's position at that moment**, and a command that is lost leaves the
  goggle in the old state until the next change. Nothing can ask the radio for the state: a goggle that
  starts after the arm hears nothing until the switch moves again.

## What the goggle does

The goggle follows the backpack's last word, armed or disarmed, instead of acting once on each command.
The backpack's reader only takes a command in; the peripheral thread, about every 100 ms, compares the
last word with the DVR and starts or stops it. So a switch flipped fast several times ends in its last
position, however busy the goggle was meanwhile.

* **Armed, Record Mode Manual**: the goggle records whenever the video view is shown and a card is
  ready. A recording that a menu, a channel switch or a source switch ended starts again, as a new
  file, when the video view is back. An arm that comes while a menu is open is kept for then.
* **Armed, Record Mode Auto**: the picture starts the recording, as without the backpack.
* **Disarmed**: the recording stops after the radio's stop delay, and Record Mode Auto does not start
  a new one while the picture stays. The next arm starts a new file.
* **The delays** (DVR start delay, stop delay on the radio) run on the goggle's clock. Disarming during
  the start delay cancels the start; arming again during the stop delay keeps the recording going.
* **The DVR button wins**: after a press, the arm is not enforced until the backpack's next command.
  A stop by the button in Record Mode Auto holds until the picture has been gone for 2 s (the next
  flight then records again).
* **A start that does not hold** (the recorder refuses or fails: a card it cannot write, no record folder,
  a full card; `HDZGOGGLE.log` says which, `DVR: the recorder did not start: status N (...)`) is tried again
  after 1 s, then 5 s, then every 15 s, until a recording holds for 3 s, the backpack's next command or a
  button press. (On the owner's goggle a card with a damaged filesystem made every start fail; the armed
  DVR then started and stopped every second.)
* **No backpack command since the goggle started**: nothing changes, as on ripples.

## What it fixes (ripples 1.2.2)

* A backpack command was ignored while a menu was open.
* A recording that a menu, channel or source switch ended was not started again while still armed.
* After a disarm, Record Mode Auto started a new recording about a second later (the picture was still
  there). The next arm then found a recording running, so the flight went on in the landing's file.
* The start and stop delays ran only while the backpack's serial line had been quiet for a moment.
* The backpack's serial reader polled without a pause after its first quiet 100 ms, keeping a CPU core
  busy as long as the backpack was on (the timeout of `select()` was set only once).
* A stop by the button in Record Mode Auto was undone a second later.
* **Every start and stop held the goggle for 2 s** (`sleep(2)` "wait for record process" in
  `dvr_cmd()`, under its lock). The backpack's commands were handled in its reader thread, so commands
  that came meanwhile, and a DVR button press, waited behind it: a switch flipped fast left the DVR
  seconds behind (the owner's log, 21:52 to 21:55). The goggle now waits for the recorder's status
  instead, so the two stay in step: after a start until it says "recording" (at most 2 s; an error ends
  the wait and the start counts as failed), after a stop until it no longer does (well under a second in
  the owner's `dvr_dbg.log`, but up to 3 s for a recording stopped within a second of its start: at most
  4 s). A start waits for a stop that the recorder has not finished. The reader thread never waits.

## Commands that never arrive: the radio at 1000 mW with no quad connected

**Found by the owner on 2026-11-20:** with no quad connected and the ELRS transmitter at its maximum,
1000 mW, the DVR command that the radio's backpack sends over ESP-NOW gets lost after a few arms. The
transmitter's own 2.4 GHz signal overwhelms the backpack's ESP-NOW packet, so the goggle's backpack chip never
receives it. The owner's ESP32-C3 backpack on another goggle loses commands the same way, so neither this
firmware nor the HDZero goggle causes it. The Telem Ratio is not the cause either: even with telemetry Off the
backpack works. The HDZero goggle's backpack chip sits inside the housing, so it may lose them a little sooner
(not measured); the cause is the transmitter.

**With the quad connected there is no problem at all.** With no quad, keep the transmitter at a normal power,
such as 100 mW: that avoids it too. So if DVR commands get lost while you test on the bench, check this first:
is a quad connected, and is the transmitter at 1000 mW? Nothing in the goggle's firmware can make up for a
command that never reaches the goggle.

Where a real fix would belong: the radio's backpack chip. ExpressLRS Backpack 1.5.9 (`src/Tx_main.cpp`,
`sendMSPViaEspnow()`) hands each command to ESP-NOW once, ignores the result and registers no send
callback, and ExpressLRS sends each switch change once. Nobody notices a command the goggle did not
acknowledge. Resending the last recording state until it is acknowledged would deliver it as soon as a
packet gets through. Not written, not tested.

### Earlier tests: the Telem Ratio (advice withdrawn)

Until 2026-11-20 this page, the README and the release notes advised ExpressLRS Telem Ratio 1:16 or 1:32
instead of Std. That advice is withdrawn: the Telem Ratio is not the cause. The tests behind it are kept as
they were made. They fit the cause above: the backpack's packet got through only in the transmitter's
telemetry slots, while the transmitter was silent. One result does not fit and was not checked again: with Std
at 100 mW commands were lost too, while in the owner's later use at 100 mW none is lost.

| at 250 Hz (a packet every 4 ms) | a pause every | DVR commands |
|---|---|---|
| Std = 1:64 | 256 ms | about 1 in 4 lost, at 100 mW as at 1 W |
| 1:32 | 128 ms | none lost, also at 1 W |
| 1:16 | 64 ms | none lost |


What was found on the owner's Goggle 1 (backpack 1.5.9, radio at 250 Hz, Telem Ratio Std):

* About 1 in 4 commands was missing (a start after a start, a stop after a stop), whatever the goggle
  firmware did. The goggle's side logged no CRC failure and no overflow; a debug build of the backpack
  (its log goes to `HDZGOGGLE.log` as `[ESP]` lines) showed that the goggle's backpack chip never received
  the missing ones.
* Every command that did arrive came on a **256.0 ms grid**, within a few ms (41 of 41 in one run). Not
  changed by: head tracking off, the goggle's WiFi off, the radio 10 cm away, WiFi power save off on the
  chip (it stayed on channel 1 throughout).
* 256 ms is the ELRS transmitter's telemetry cycle: with Std the TX listens instead of sending once every
  64 packets at 250 Hz (64 x 4 ms) and once every 128 at 500 Hz (128 x 2 ms) (ExpressLRS
  `src/src/common.cpp`, `TLMinterval`; `UpdateTlmRatioEffective()` in `tx_main.cpp`). The backpack's
  ESP-NOW packet, sent from the same module on the same 2.4 GHz band, reached the goggle's chip only in
  that silent slot. The radio's backpack tries a packet for roughly 190 ms (inferred from the loss rate):
  a change made in the first ~65 ms after a slot is given up before the next one, 65 / 256 = 25 %. Changes
  in quick succession lose every second packet (seen as "start, start, start", 256 ms apart).
* With Telem Ratio 1:16 the slot comes every 64 ms: 24 of 24 changes arrived, the 256 ms grid was gone
  and a 64 ms grid took its place (the owner's log of 2026-11-20). The cost is about 6 % of the control
  packets (250 -> 234 a second).
* On the final image, with the unchanged backpack 1.5.9 on the goggle (the 24-of-24 run above was made on
  the debug build, which also had WiFi power save off): Telem Ratio 1:64 at 100 mW still lost at least 8
  of 54 changes (the goggle's log). With 1:16 and with 1:32 (a
  slot every 128 ms, still inside the ~190 ms; 250 -> 242 packets a second) the owner saw none lost, with
  1:32 also at 1 W (the owner's report, no log read).

Not tested in these runs: 500 Hz, Race.

A command that is lost all the same leaves the DVR in the old state until the next switch change (in the
log: a start after a start, or a stop after a stop).

## The log

With Storage, Logging On, `HDZGOGGLE.log` on the card has a line for every decision:

```
DVR auto: backpack start, delay 0 s (video view 1, recording 0, card 1, Record Mode Manual)
DVR auto: start (armed)
DVR auto: button stop
DVR auto: backpack stop, delay 5 s (...)
DVR auto: stop (the backpack is disarmed)
DVR auto: Record Mode Auto does not start a recording (the backpack is disarmed)
```

A start after a start, or a stop after a stop, in the `backpack` lines is a command that never arrived
(section above). `dvr_dbg.log` has the recorder's own steps, how long the recorder took to start or stop
(`start: the recorder's status 1 after ... ms`), and since juricabi.2 a line for a slow part of the
recorder's loop (`slow live_run: ... ms`). Since juricabi.2 it is written **only with Storage, Logging On**
as well (ripples wrote it always, a line with an fsync at every DVR start and stop, without end), and it is
kept under 512 KB with one older copy (`dvr_dbg.log.1`). Switch Logging off again after looking for a
fault: both logs are extra writes on the card.

## A clip is on the card when the recording stops

ripples' recorder closed a clip and left its end and its directory entry in the kernel's write cache; the
app's next start synced them. A card pulled, or the goggle switched off, within about half a minute of a
stop lost that last clip: 0 bytes on the card, its data as lost chains (the owner's card, 2026-11-20,
after about ten card swaps in an evening; the recorder then could not write on that card at all, and
every start failed). Since juricabi.2 the recorder syncs after closing a clip and the app after a stop.

Still true, as on every firmware of this goggle: power cut or the card pulled **while recording** loses
the end of that clip and can leave the filesystem damaged. Stop the recording (disarm) first. A damaged
card: `chkdsk X: /f` on a PC, or Storage, Format on the goggle.

## Checks

`test/dvr/test_dvr_auto.c` checks the decisions on the host (`make -C test/dvr run`; 25 of 25 mutants of
`dvr_auto.c` caught). The waits for the recorder (`dvr.c`) have no host test. On a goggle:

1. Record Mode Manual, arm: recording. Open the menu, close it: a new recording. Disarm: it stops after
   the stop delay.
2. Record Mode Auto with a picture: recording. Disarm: it stops, and does not start again. Arm: a new
   recording.
3. Arm with a 3 s start delay, disarm after 1 s: no recording.
4. Armed, press the DVR button: it stops and stays stopped until the next arm.
5. Flip the switch on and off fast, five times, and leave it: within about a second of the last flip the
   DVR is in the switch's position (unless the last command was one that never arrived: the log shows it).
