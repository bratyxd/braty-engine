/* br_tween.h - one-line tweens on any animatable property path (br_animation.h), sequences and
 * parallel groups. Guide: docs/agents/animation.md.
 *
 *   br_tween_to_f(e, br_anim_target_2d(panel), "alpha", 1.0f, 0.4f, BR_EASE_OUT_CUBIC);
 *   float pos[2] = { 640.0f, 360.0f };
 *   br_tween t = br_tween_to(e, br_anim_target_2d(panel), "position", pos, 0.5f, BR_EASE_OUT_BACK);
 *   br_tween_set_delay(e, t, 0.2f);
 *
 * Every tween lives in one dense array advanced by one system ("animation.tweens", LATE_UPDATE
 * -200, so animators can override). The start value of `to` tweens is read when the tween starts
 * (after its delay). A tween whose node is destroyed is dropped silently.
 */
#ifndef BR_TWEEN_H
#define BR_TWEEN_H

#include "braty/br_animation.h"

typedef struct br_tween { uint32_t index; uint32_t generation; } br_tween;

/* Called once when a tween (all repeats) completes. Not called when it is killed. */
typedef void (*br_tween_fn)(br_engine *e, br_tween tween, void *user);

/* Tweens the property path from its value at start to `to` (as many floats as the property has). */
br_tween br_tween_to(br_engine *e, br_anim_target target, const char *property, const float *to, float seconds,
                     br_ease_type ease);
/* Tweens from `from` to the value the property has when the tween starts. */
br_tween br_tween_from(br_engine *e, br_anim_target target, const char *property, const float *from, float seconds,
                       br_ease_type ease);
/* Tweens from `from` to `to`. */
br_tween br_tween_from_to(br_engine *e, br_anim_target target, const char *property, const float *from,
                          const float *to, float seconds, br_ease_type ease);
/* br_tween_to for one-float properties ("alpha", "rotation", "x"). */
br_tween br_tween_to_f(br_engine *e, br_anim_target target, const char *property, float to, float seconds,
                       br_ease_type ease);
/* A tween with no property: calls fn after `seconds` (sequences use it for callbacks). */
br_tween br_tween_delay_call(br_engine *e, float seconds, br_tween_fn fn, void *user);

/* Waits `seconds` before starting (adds to any delay a sequence gave it). */
void br_tween_set_delay(br_engine *e, br_tween t, float seconds);
/* Plays `count` more times (-1 = forever); yoyo plays every other pass backwards. */
void br_tween_set_repeat(br_engine *e, br_tween t, int32_t count, bool yoyo);
/* Changes the easing. */
void br_tween_set_ease(br_engine *e, br_tween t, br_ease_type ease);
/* Changes the easing by data-file name ("out_back"); false (and linear) if unknown. */
bool br_tween_set_ease_name(br_engine *e, br_tween t, const char *name);
/* Called when the tween completes. */
void br_tween_set_on_complete(br_engine *e, br_tween t, br_tween_fn fn, void *user);
/* A number for br_tween_kill_tag (0 = none). */
void br_tween_set_tag(br_engine *e, br_tween t, uint32_t tag);

/* Stops a tween where it is (no on_complete). */
void br_tween_kill(br_engine *e, br_tween t);
/* Stops every tween made on a node (its target argument) or writing it (through a child path such
 * as "label:alpha"). Returns how many. */
uint32_t br_tween_kill_target(br_engine *e, br_anim_target target);
/* Stops every tween with a tag. Returns how many. */
uint32_t br_tween_kill_tag(br_engine *e, uint32_t tag);
/* True while the tween exists (waiting, running or repeating). */
bool br_tween_alive(br_engine *e, br_tween t);
/* Number of live tweens. */
uint32_t br_tween_count(br_engine *e);

/* ---- sequences: a time cursor that schedules tweens one after another (plain value, no heap)
 *
 *   br_tween_sequence seq = br_tween_sequence_begin(7);           tag 7 kills the whole sequence
 *   br_tween_sequence_add(e, &seq, br_tween_to_f(...));           starts at 0
 *   br_tween_sequence_join(e, &seq, br_tween_to_f(...));          parallel with the previous add
 *   br_tween_sequence_wait(&seq, 0.2f);
 *   br_tween_sequence_add(e, &seq, br_tween_to(...));             after both, plus 0.2 s
 *   br_tween_sequence_call(e, &seq, on_done, user);               a callback at the end
 *
 * Set a tween's delay and repeats before adding it (its whole length counts). */
typedef struct br_tween_sequence {
    float    cursor;        /* where the next add starts (seconds from now) */
    float    group_start;   /* where the last add started (join starts there) */
    uint32_t tag;           /* given to every tween of the sequence (0 = none) */
} br_tween_sequence;

/* A new sequence starting now. */
br_tween_sequence br_tween_sequence_begin(uint32_t tag);
/* Schedules t after everything added so far. */
void br_tween_sequence_add(br_engine *e, br_tween_sequence *seq, br_tween t);
/* Schedules t in parallel with the last br_tween_sequence_add. */
void br_tween_sequence_join(br_engine *e, br_tween_sequence *seq, br_tween t);
/* Inserts a pause. */
void br_tween_sequence_wait(br_tween_sequence *seq, float seconds);
/* Calls fn at the current end of the sequence. */
br_tween br_tween_sequence_call(br_engine *e, br_tween_sequence *seq, br_tween_fn fn, void *user);

#endif
