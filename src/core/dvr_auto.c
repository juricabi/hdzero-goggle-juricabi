#include "dvr_auto.h"

#include <string.h>

void dvr_auto_reset(dvr_auto_t *s) {
    memset(s, 0, sizeof(*s));
}

// Record Mode Manual with the backpack armed: a start whenever the goggle can record and does not.
static dvr_auto_action_t enforce(dvr_auto_t *s, const dvr_auto_env_t *env) {
    // (Record Mode Auto: the picture starts it, src/core/thread.c, dvr_auto_picture_may_start())
    if (s->arm != DVR_AUTO_ARM_ON || s->held || env->auto_mode)
        return DVR_AUTO_NOTHING;
    if (!env->in_video || env->recording || !env->storage_ok)
        return DVR_AUTO_NOTHING;
    if (env->now_ms < s->next_try_ms)
        return DVR_AUTO_NOTHING;
    // again after DVR_AUTO_RETRY_MS, then more slowly as long as no start holds
    static const unsigned retry_ms[] = {DVR_AUTO_RETRY_MS, DVR_AUTO_RETRY2_MS, DVR_AUTO_RETRY3_MS};
    s->next_try_ms = env->now_ms + retry_ms[s->fails < 2 ? s->fails : 2];
    if (s->fails < 2)
        s->fails++;
    return DVR_AUTO_START;
}

void dvr_auto_backpack(dvr_auto_t *s, bool on, unsigned delay_s, const dvr_auto_env_t *env) {
    // the backpack's new word replaces the button's and any delayed one
    s->held = false;
    s->user_stopped = false;
    s->due = false;
    s->fails = 0;
    if (on) {
        if (delay_s == 0) {
            s->arm = DVR_AUTO_ARM_ON;
            s->next_try_ms = 0;
        } else {
            s->due = true;
            s->due_on = true;
            s->due_ms = env->now_ms + (uint64_t)delay_s * 1000u;
        }
        return;
    }
    // disarmed: nothing starts from now on; a recording stops after the stop delay (at the next pass
    // without one). Always scheduled: a start may be under way that the goggle does not count yet.
    s->arm = DVR_AUTO_ARM_OFF;
    s->due = true;
    s->due_on = false;
    s->due_ms = env->now_ms + (uint64_t)delay_s * 1000u;
}

dvr_auto_action_t dvr_auto_step(dvr_auto_t *s, const dvr_auto_env_t *env, bool picture) {
    if (picture) {
        s->lost_passes = 0;
    } else if (s->lost_passes < DVR_AUTO_LOSS_PASSES && ++s->lost_passes == DVR_AUTO_LOSS_PASSES) {
        s->user_stopped = false; // the picture was gone a while: the next one may start a recording
    }

    // a recording that held DVR_AUTO_HOLD_MS: the start before it was good
    if (env->recording && !s->was_recording)
        s->rec_since_ms = env->now_ms;
    if ((env->recording || s->was_recording) && env->now_ms - s->rec_since_ms >= DVR_AUTO_HOLD_MS)
        s->fails = 0;
    s->was_recording = env->recording;

    if (s->due && env->now_ms >= s->due_ms) {
        if (s->due_on) {
            s->due = false;
            s->arm = DVR_AUTO_ARM_ON;
            s->next_try_ms = 0;
        } else if (env->recording) {
            s->due = false;
            return DVR_AUTO_STOP;
        } else if (env->now_ms >= s->due_ms + DVR_AUTO_STOP_WATCH_MS) {
            s->due = false; // nothing was recording, nor came to: the stop is done with
        }
    }
    return enforce(s, env);
}

void dvr_auto_refused(dvr_auto_t *s, uint64_t now_ms) {
    s->next_try_ms = now_ms + DVR_AUTO_RETRY_BUSY_MS;
    if (s->fails)
        s->fails--; // (not a start that failed: it was not made)
}

void dvr_auto_button(dvr_auto_t *s, bool stopping) {
    // the pilot's own press is the latest word: the arm is not enforced, a delayed command is dropped
    s->held = true;
    s->due = false;
    s->fails = 0;
    s->user_stopped = stopping;
    if (stopping)
        s->lost_passes = 0;
}

bool dvr_auto_picture_may_start(const dvr_auto_t *s) {
    return s->arm != DVR_AUTO_ARM_OFF && !s->user_stopped;
}

const char *dvr_auto_arm_name(dvr_auto_arm_t arm) {
    switch (arm) {
    case DVR_AUTO_ARM_ON:
        return "armed";
    case DVR_AUTO_ARM_OFF:
        return "disarmed";
    default:
        return "none";
    }
}
