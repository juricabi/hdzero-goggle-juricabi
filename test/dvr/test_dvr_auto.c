// Host test of the automatic DVR's decisions (src/core/dvr_auto.c): the backpack's recording control,
// Record Mode Auto and the DVR button.
//
// What the backpack sends (ExpressLRS src/lib/Backpack/devBackpack.cpp, backpack 1.5.9): the TX module
// checks the DVR switch (or the arm state) every 20 ms and sends MSP_ELRS_BACKPACK_SET_RECORDING_STATE
// [state, delay lo, delay hi] once at each change, never again; both backpacks pass it on unchanged. So
// every command is the switch's position at that moment: the goggle follows the latest one.
#include <stdio.h>
#include <string.h>

#include "core/dvr_auto.h"

static int failures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                    \
            fprintf(stderr, "\n");                           \
            failures++;                                      \
        }                                                    \
    } while (0)

static const char *name(dvr_auto_action_t a) {
    return a == DVR_AUTO_START ? "start" : a == DVR_AUTO_STOP ? "stop" : "nothing";
}

// the goggle in the video view, a card ready, not recording, Record Mode Manual, at t
static dvr_auto_env_t video(uint64_t t) {
    dvr_auto_env_t e = {.now_ms = t, .in_video = true, .recording = false, .storage_ok = true, .auto_mode = false};
    return e;
}

#define EXPECT(got, want, what) CHECK((got) == (want), "%s: %s, want %s", what, name(got), name(want))

// the backpack's command, then the peripheral thread's next pass (100 ms later)
static dvr_auto_action_t command(dvr_auto_t *s, bool on, unsigned delay_s, dvr_auto_env_t *e) {
    dvr_auto_backpack(s, on, delay_s, e);
    e->now_ms += 100;
    return dvr_auto_step(s, e, true);
}

static void test_no_backpack(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    for (int i = 0; i < 50; i++) {
        e.now_ms = i * 100;
        EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "no backpack command: nothing happens");
    }
    CHECK(dvr_auto_picture_may_start(&s), "no backpack command: Record Mode Auto starts as ripples' did");
}

static void test_command_only_taken_in(void) {
    // the backpack's reader thread must never wait for the DVR: taking a command in decides nothing
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(1000);
    dvr_auto_backpack(&s, true, 0, &e);
    CHECK(s.arm == DVR_AUTO_ARM_ON, "armed: kept");
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "armed: the next pass starts");
}

static void test_manual_armed(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(1000);
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_START, "armed in the video view: start");
    e.recording = true;
    e.now_ms += 100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "recording: nothing more");

    // a menu / a channel switch ended the recording; back in the video view it starts again
    e.recording = false;
    e.in_video = false;
    e.now_ms = 5000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "a menu open: nothing");
    e.in_video = true;
    e.now_ms = 6000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "back in the video view, still armed: start again");

    // refused (the goggle busy): again after the short retry, not at every pass
    dvr_auto_refused(&s, 6000);
    e.now_ms = 6100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "refused: not again at once");
    e.now_ms = 6000 + DVR_AUTO_RETRY_BUSY_MS;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "refused: again after the busy retry");

    // the recorder failed it (no recording, no pending start): again after DVR_AUTO_RETRY_MS
    e.now_ms += 100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "failed start: not at every pass");
    e.now_ms += DVR_AUTO_RETRY_MS;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "failed start: tried again");

    // no card: nothing
    e.storage_ok = false;
    e.now_ms += 5000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "no card: nothing");
}

static void test_armed_in_menu(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    e.in_video = false;
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_NOTHING, "armed while a menu is open: kept");
    e.in_video = true;
    e.now_ms = 3000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "the menu closed: start");
}

static void test_fast_toggles(void) {
    // the owner's test: the switch flipped on and off fast. Commands that came while the goggle was busy
    // (a start takes up to 2 s) are all taken in; the next pass acts on the last one.
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    dvr_auto_backpack(&s, true, 0, &e);
    dvr_auto_backpack(&s, false, 0, &e);
    dvr_auto_backpack(&s, true, 0, &e);
    dvr_auto_backpack(&s, false, 0, &e);
    e.now_ms += 100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "on off on off, not recording: no start");
    dvr_auto_backpack(&s, true, 0, &e);
    dvr_auto_backpack(&s, false, 0, &e);
    dvr_auto_backpack(&s, true, 0, &e);
    e.now_ms += 100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "on off on: start");

    // recording: off then on again before the next pass: the recording goes on, no stop
    e.recording = true;
    dvr_auto_backpack(&s, false, 0, &e);
    dvr_auto_backpack(&s, true, 0, &e);
    e.now_ms += 100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "recording, off on: goes on");
    // recording: on (a repeat after a lost off) changes nothing
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_NOTHING, "recording, on again: goes on");
    // off: the next pass stops
    EXPECT(command(&s, false, 0, &e), DVR_AUTO_STOP, "recording, off: stop");
    e.recording = false;
    // off again (a repeat after a lost on): nothing
    EXPECT(command(&s, false, 0, &e), DVR_AUTO_NOTHING, "stopped, off again: nothing");
}

static void test_disarm_while_start_under_way(void) {
    // a start the goggle does not count as recording yet (dvr_cmd() applies the audio first), then the
    // disarm: the stop waits for the recording to show
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_START, "armed: start");
    EXPECT(command(&s, false, 0, &e), DVR_AUTO_NOTHING, "disarmed, the start not counted yet: nothing yet");
    e.now_ms += 900;
    e.recording = true;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_STOP, "the start shows: stopped");

    // nothing came to record within DVR_AUTO_STOP_WATCH_MS: the stop is done with; a later start by the
    // button is not stopped
    dvr_auto_reset(&s);
    e = video(0);
    dvr_auto_backpack(&s, false, 0, &e);
    e.now_ms = DVR_AUTO_STOP_WATCH_MS;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "disarmed, nothing recording: nothing");
    CHECK(!s.due, "disarmed, nothing recording: the stop done with after the watch");
    e.recording = true;
    e.now_ms += 100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "a recording after that: not stopped");
}

static void test_delays(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(10000);
    dvr_auto_backpack(&s, true, 3, &e);
    e.now_ms = 12900;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "start delay: not before 3 s");
    e.now_ms = 13000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "start delay passed: start");
    dvr_auto_refused(&s, 13000);
    e.now_ms = 13100;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "start delay passed, refused: the delay done with, retry later");

    e.recording = true;
    e.now_ms = 20000;
    dvr_auto_backpack(&s, false, 5, &e);
    CHECK(!dvr_auto_picture_may_start(&s), "disarmed: Record Mode Auto does not start meanwhile");
    e.now_ms = 24900;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "stop delay: not before 5 s");
    e.now_ms = 25000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_STOP, "stop delay passed: stop");
    e.recording = false;
    e.now_ms = 30000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "disarmed: no new start");

    // a stop delay whose recording a menu already ended: nothing to stop
    dvr_auto_reset(&s);
    e = video(0);
    dvr_auto_backpack(&s, true, 0, &e);
    e.recording = true;
    dvr_auto_backpack(&s, false, 4, &e);
    e.recording = false;
    e.now_ms = 4000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "stop delay, recording already ended: nothing");

    // armed with a delay, disarmed before it passed: no start
    dvr_auto_reset(&s);
    e = video(0);
    dvr_auto_backpack(&s, true, 3, &e);
    e.now_ms = 1000;
    dvr_auto_backpack(&s, false, 0, &e);
    e.now_ms = 5000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "armed, disarmed within the start delay: no start");

    // disarmed with a stop delay, armed again within it: the recording goes on
    dvr_auto_reset(&s);
    e = video(0);
    dvr_auto_backpack(&s, true, 0, &e);
    e.recording = true;
    dvr_auto_backpack(&s, false, 5, &e);
    e.now_ms = 2000;
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_NOTHING, "re-armed within the stop delay: recording");
    e.now_ms = 6000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "re-armed within the stop delay: no stop");
}

static void test_disarm_now(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    dvr_auto_backpack(&s, true, 0, &e);
    e.recording = true;
    EXPECT(command(&s, false, 0, &e), DVR_AUTO_STOP, "disarmed while recording: stop at the next pass");
    e.recording = false;
    e.now_ms = 10000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "disarmed: no start");
    // the next arm records again (the case ripples lost: the Auto mode restart in between)
    e.now_ms = 20000;
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_START, "next arm: start");
}

static void test_button(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    dvr_auto_backpack(&s, true, 0, &e);
    e.recording = true;
    dvr_auto_button(&s, true); // the pilot stops it while armed
    e.recording = false;
    e.now_ms = 5000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "button stop while armed: not started again");
    e.now_ms = 9000;
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_START, "the backpack's next arm: start again");

    // a button start while disarmed: recording; the next disarm stops it
    dvr_auto_reset(&s);
    e = video(0);
    dvr_auto_backpack(&s, false, 0, &e);
    e.now_ms = 5000;
    dvr_auto_step(&s, &e, true);
    dvr_auto_button(&s, false);
    e.recording = true;
    e.now_ms = 6000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "button start while disarmed: kept");
    EXPECT(command(&s, false, 0, &e), DVR_AUTO_STOP, "the backpack's next disarm: stop");

    // a button press drops a delayed command
    dvr_auto_reset(&s);
    e = video(0);
    dvr_auto_backpack(&s, true, 0, &e);
    e.recording = true;
    dvr_auto_backpack(&s, false, 5, &e);
    dvr_auto_button(&s, false);
    e.now_ms = 6000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "a button press after the disarm: its stop delay dropped");
}

static void test_auto_mode(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    e.auto_mode = true;
    EXPECT(command(&s, true, 0, &e), DVR_AUTO_NOTHING, "Auto mode, armed: the picture starts it");
    CHECK(dvr_auto_picture_may_start(&s), "Auto mode, armed: the picture may start");
    e.now_ms = 5000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "Auto mode: never an enforced start");
    e.recording = true;
    EXPECT(command(&s, false, 0, &e), DVR_AUTO_STOP, "Auto mode, disarmed: stop");
    CHECK(!dvr_auto_picture_may_start(&s), "Auto mode, disarmed: the picture does not start a new one");

    // a stop by the button holds until the picture was gone DVR_AUTO_LOSS_PASSES passes in a row
    dvr_auto_reset(&s);
    e = video(0);
    e.auto_mode = true;
    dvr_auto_button(&s, true);
    CHECK(!dvr_auto_picture_may_start(&s), "Auto mode, button stop: no restart");
    for (int i = 0; i < DVR_AUTO_LOSS_PASSES - 1; i++)
        dvr_auto_step(&s, &e, false);
    dvr_auto_step(&s, &e, true); // the picture back before the end of the loss
    for (int i = 0; i < DVR_AUTO_LOSS_PASSES - 1; i++)
        dvr_auto_step(&s, &e, false);
    CHECK(!dvr_auto_picture_may_start(&s), "Auto mode, button stop: a short loss does not end it");
    dvr_auto_step(&s, &e, false);
    CHECK(dvr_auto_picture_may_start(&s), "Auto mode, button stop: the picture gone a while, the next one starts");

    // a button start in Auto mode: the picture may start again after a loss (as ripples)
    dvr_auto_button(&s, false);
    CHECK(dvr_auto_picture_may_start(&s), "Auto mode, button start: not held");
}

int main(void) {
    test_no_backpack();
    test_command_only_taken_in();
    test_manual_armed();
    test_armed_in_menu();
    test_fast_toggles();
    test_disarm_while_start_under_way();
    test_delays();
    test_disarm_now();
    test_button();
    test_auto_mode();
    if (failures) {
        fprintf(stderr, "test_dvr_auto: %d failed\n", failures);
        return 1;
    }
    puts("test_dvr_auto: all checks passed");
    return 0;
}
