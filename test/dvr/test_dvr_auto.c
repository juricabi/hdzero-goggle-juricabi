// Host test of the automatic DVR's decisions (src/core/dvr_auto.c): the backpack's recording control,
// Record Mode Auto and the DVR button.
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

static void test_manual_armed(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(1000);
    EXPECT(dvr_auto_backpack(&s, true, 0, &e), DVR_AUTO_START, "armed in the video view: start at once");
    e.recording = true;
    e.now_ms = 1100;
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
    EXPECT(dvr_auto_backpack(&s, true, 0, &e), DVR_AUTO_NOTHING, "armed while a menu is open: kept");
    e.in_video = true;
    e.now_ms = 3000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "the menu closed: start");
}

static void test_delays(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(10000);
    EXPECT(dvr_auto_backpack(&s, true, 3, &e), DVR_AUTO_NOTHING, "start delay 3 s: not yet");
    e.now_ms = 12900;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "start delay: not before 3 s");
    e.now_ms = 13000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_START, "start delay passed: start");

    e.recording = true;
    e.now_ms = 20000;
    EXPECT(dvr_auto_backpack(&s, false, 5, &e), DVR_AUTO_NOTHING, "stop delay 5 s: not yet");
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

    // disarmed with a delay while not recording: nothing waits (ripples discarded that timer too)
    dvr_auto_reset(&s);
    e = video(0);
    EXPECT(dvr_auto_backpack(&s, false, 4, &e), DVR_AUTO_NOTHING, "disarmed, not recording: nothing");
    CHECK(!s.due, "disarmed, not recording: no timer");

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
    EXPECT(dvr_auto_backpack(&s, true, 0, &e), DVR_AUTO_NOTHING, "re-armed within the stop delay: recording");
    e.now_ms = 6000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "re-armed within the stop delay: no stop");
}

static void test_disarm_now(void) {
    dvr_auto_t s;
    dvr_auto_reset(&s);
    dvr_auto_env_t e = video(0);
    dvr_auto_backpack(&s, true, 0, &e);
    e.recording = true;
    EXPECT(dvr_auto_backpack(&s, false, 0, &e), DVR_AUTO_STOP, "disarmed while recording: stop at once");
    e.recording = false;
    e.now_ms = 10000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "disarmed: no start");
    // the next arm records again (the case ripples lost: the Auto mode restart in between)
    e.now_ms = 20000;
    EXPECT(dvr_auto_backpack(&s, true, 0, &e), DVR_AUTO_START, "next arm: start");
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
    EXPECT(dvr_auto_backpack(&s, true, 0, &e), DVR_AUTO_START, "the backpack's next arm: start again");

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
    EXPECT(dvr_auto_backpack(&s, true, 0, &e), DVR_AUTO_NOTHING, "Auto mode, armed: the picture starts it");
    CHECK(dvr_auto_picture_may_start(&s), "Auto mode, armed: the picture may start");
    e.now_ms = 5000;
    EXPECT(dvr_auto_step(&s, &e, true), DVR_AUTO_NOTHING, "Auto mode: never an enforced start");
    e.recording = true;
    EXPECT(dvr_auto_backpack(&s, false, 0, &e), DVR_AUTO_STOP, "Auto mode, disarmed: stop");
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
    test_manual_armed();
    test_armed_in_menu();
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
