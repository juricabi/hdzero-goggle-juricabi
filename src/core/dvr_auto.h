#pragma once

/*
 * Automatic DVR, juricabi: the decisions of the ELRS backpack's recording control (arm / disarm on the
 * radio, MSP_SET_REC_STATE) and how it meets ripples' Record Mode Auto (start when the picture comes,
 * stop when it goes, src/core/thread.c) and the DVR button. Pure logic without firmware dependencies,
 * so that test/dvr/test_dvr_auto.c runs it on the host; src/core/dvr.c holds the state, takes the
 * goggle's state for it and carries out what it decides.
 *
 * The goggle remembers the backpack's last word instead of acting once on each command:
 *  - Armed (Record Mode Manual): it records whenever the video view is shown and a card is ready. A
 *    command that came while a menu was open, while the goggle was busy (the recorder pausing for a
 *    menu, a source or channel switch) or before the card was ready is not lost, and a recording that
 *    such a switch ended starts again. (ripples dropped the command then, and nothing restarted it.)
 *  - Armed (Record Mode Auto): ripples' signal logic starts it, as without the backpack.
 *  - Disarmed: the recording stops (after the radio's stop delay), and Record Mode Auto does not start
 *    a new one while the picture stays (ripples' started one about a second after the disarm, so the
 *    next arm found a recording and the flight went into the landing's file).
 *  - Delays (the radio's DVR start / stop delay) run on the goggle's clock, checked every pass of the
 *    peripheral thread (ripples ran them only when the backpack's serial line was quiet).
 *  - The DVR button wins: after a press the backpack's arm is not enforced until its next command; a
 *    stop by the button in Record Mode Auto holds until the picture has been gone for a while (ripples
 *    started the next recording a second later, so Auto mode could not be stopped by hand).
 *  - No backpack command since the start: nothing changes, ripples' behaviour.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// how often an enforced start is tried again (it was refused: busy, or the recorder failed)
#define DVR_AUTO_RETRY_MS      1000
#define DVR_AUTO_RETRY_BUSY_MS 250
// Record Mode Auto, after a stop by the button: passes (about 100 ms each) without a picture after which
// the picture may start a recording again
#define DVR_AUTO_LOSS_PASSES 20

typedef enum {
    DVR_AUTO_ARM_NONE = 0, // no backpack command since the start
    DVR_AUTO_ARM_ON,
    DVR_AUTO_ARM_OFF,
} dvr_auto_arm_t;

typedef enum {
    DVR_AUTO_NOTHING = 0,
    DVR_AUTO_START,
    DVR_AUTO_STOP,
} dvr_auto_action_t;

typedef struct {
    dvr_auto_arm_t arm;    // the backpack's last word that is in effect
    bool held;             // the button was pressed since: the arm is not enforced
    bool user_stopped;     // the button stopped a recording: Record Mode Auto does not start one
    unsigned lost_passes;  // passes without a picture in a row
    bool due;              // a delayed backpack command waits ...
    bool due_on;           // ... arm (true) or stop (false) ...
    uint64_t due_ms;       // ... until then
    uint64_t next_try_ms;  // an enforced start not before then
} dvr_auto_t;

// What the goggle is doing, taken for every call.
typedef struct {
    uint64_t now_ms;  // monotonic
    bool in_video;    // the video view (not a menu, not asleep)
    bool recording;   // recording, or a start asked for and not refused yet
    bool storage_ok;  // card in and ready, room for a recording
    bool auto_mode;   // Record Mode Auto
} dvr_auto_env_t;

void dvr_auto_reset(dvr_auto_t *s);

// The backpack's recording state (MSP_SET_REC_STATE): on = start (armed), delay_s before it applies.
dvr_auto_action_t dvr_auto_backpack(dvr_auto_t *s, bool on, unsigned delay_s, const dvr_auto_env_t *env);

// Every pass of the peripheral thread (about 100 ms), with the source's picture.
dvr_auto_action_t dvr_auto_step(dvr_auto_t *s, const dvr_auto_env_t *env, bool picture);

// A start that dvr_auto_step() asked for was refused at once (the goggle busy): try again soon.
void dvr_auto_refused(dvr_auto_t *s, uint64_t now_ms);

// The DVR button (or a button assigned "Toggle DVR"): stopping = it stops a recording.
void dvr_auto_button(dvr_auto_t *s, bool stopping);

// Record Mode Auto: may the picture start a recording now?
bool dvr_auto_picture_may_start(const dvr_auto_t *s);

const char *dvr_auto_arm_name(dvr_auto_arm_t arm);

#ifdef __cplusplus
}
#endif
