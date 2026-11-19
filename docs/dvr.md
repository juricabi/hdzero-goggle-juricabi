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
  seconds behind (the owner's log, 21:52 to 21:55). The goggle now waits only until the recorder's status
  says it has started or stopped, at most those 2 s (the owner's `dvr_dbg.log`: well under a second), and
  the reader thread never waits.

## Commands that never arrive

In the owner's test (juricabi.2-e2d88c7, Goggle 1, backpack 1.5.9) 7 of about 29 commands were missing:
a start after a start, a stop after a stop. The goggle's side logged no CRC failure and no overflow (it
logs both), and it has one reader for the backpack's line: everything the goggle's backpack chip sent
arrived. The same radio drives the owner's Orqa without misses. So those commands were lost on the way
into the HDZero goggle's built-in backpack chip, over the air or inside it, where the goggle firmware
cannot see. When it happens, the next switch change puts the DVR right.

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
(section above). `dvr_dbg.log` (always written) has the recorder's own steps, how long the recorder took
to start or stop (`start: the recorder's status 1 after ... ms`), and since juricabi.2 a line for a slow
part of the recorder's loop (`slow live_run: ... ms`).

## Checks

`test/dvr/test_dvr_auto.c` checks the decisions on the host (`make -C test/dvr run`; 18 of 18 mutants of
`dvr_auto.c` caught). On a goggle:

1. Record Mode Manual, arm: recording. Open the menu, close it: a new recording. Disarm: it stops after
   the stop delay.
2. Record Mode Auto with a picture: recording. Disarm: it stops, and does not start again. Arm: a new
   recording.
3. Arm with a 3 s start delay, disarm after 1 s: no recording.
4. Armed, press the DVR button: it stops and stays stopped until the next arm.
5. Flip the switch on and off fast, five times, and leave it: within about a second of the last flip the
   DVR is in the switch's position (unless the last command was one that never arrived: the log shows it).
