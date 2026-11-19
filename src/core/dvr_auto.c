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
    s->next_try_ms = env->now_ms + DVR_AUTO_RETRY_MS;
    return DVR_AUTO_START;
}

dvr_auto_action_t dvr_auto_backpack(dvr_auto_t *s, bool on, unsigned delay_s, const dvr_auto_env_t *env) {
    // the backpack's new word replaces the button's and any delayed one
    s->held = false;
    s->user_stopped = false;
    s->due = false;
    if (on) {
        if (delay_s == 0) {
            s->arm = DVR_AUTO_ARM_ON;
            s->next_try_ms = 0;
            return enforce(s, env);
        }
        s->due = true;
        s->due_on = true;
        s->due_ms = env->now_ms + (uint64_t)delay_s * 1000u;
        return DVR_AUTO_NOTHING;
    }
    // disarmed: nothing starts from now on; a running recording stops, after the stop delay
    s->arm = DVR_AUTO_ARM_OFF;
    if (!env->recording)
        return DVR_AUTO_NOTHING;
    if (delay_s == 0)
        return DVR_AUTO_STOP;
    s->due = true;
    s->due_on = false;
    s->due_ms = env->now_ms + (uint64_t)delay_s * 1000u;
    return DVR_AUTO_NOTHING;
}

dvr_auto_action_t dvr_auto_step(dvr_auto_t *s, const dvr_auto_env_t *env, bool picture) {
    if (picture) {
        s->lost_passes = 0;
    } else if (s->lost_passes < DVR_AUTO_LOSS_PASSES && ++s->lost_passes == DVR_AUTO_LOSS_PASSES) {
        s->user_stopped = false; // the picture was gone a while: the next one may start a recording
    }

    if (s->due && env->now_ms >= s->due_ms) {
        s->due = false;
        if (s->due_on) {
            s->arm = DVR_AUTO_ARM_ON;
            s->next_try_ms = 0;
        } else if (env->recording) {
            return DVR_AUTO_STOP;
        }
    }
    return enforce(s, env);
}

void dvr_auto_refused(dvr_auto_t *s, uint64_t now_ms) {
    s->next_try_ms = now_ms + DVR_AUTO_RETRY_BUSY_MS;
}

void dvr_auto_button(dvr_auto_t *s, bool stopping) {
    // the pilot's own press is the latest word: the arm is not enforced, a delayed command is dropped
    s->held = true;
    s->due = false;
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
