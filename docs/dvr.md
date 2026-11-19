# Automatic DVR (juricabi's firmware)

The goggle starts and stops the DVR by itself in two ways, both ripples': **Record Mode Auto**
(Record Option page: a recording starts when the picture comes and stops when it goes) and the
**ELRS backpack's recording control** (on the radio: ExpressLRS Lua, Backpack, DVR Rec on arm, with a
start and a stop delay). juricabi.2 makes the two work together (`src/core/dvr_auto.c`).

## What the goggle does

The goggle remembers the backpack's last word, armed or disarmed, instead of acting once on each
command.

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

## The log

With Storage, Logging On, `HDZGOGGLE.log` on the card has a line for every decision:

```
DVR auto: backpack start, delay 0 s (video view 1, recording 0, card 1, Record Mode Manual)
DVR auto: start (backpack)
DVR auto: button stop
DVR auto: start (armed)
DVR auto: backpack stop, delay 5 s (...)
DVR auto: stop (the backpack's stop delay passed)
DVR auto: Record Mode Auto does not start a recording (the backpack is disarmed)
```

`dvr_dbg.log` (always written) has the recorder's own steps, and since juricabi.2 a line for a slow part
of the recorder's loop (`slow live_run: ... ms`).

## Checks

`test/dvr/test_dvr_auto.c` checks the decisions on the host (`make -C test/dvr run`; 14 of 14 mutants of
`dvr_auto.c` caught). On a goggle:

1. Record Mode Manual, arm: recording. Open the menu, close it: a new recording. Disarm: it stops after
   the stop delay.
2. Record Mode Auto with a picture: recording. Disarm: it stops, and does not start again. Arm: a new
   recording.
3. Arm with a 3 s start delay, disarm after 1 s: no recording.
4. Armed, press the DVR button: it stops and stays stopped until the next arm.
