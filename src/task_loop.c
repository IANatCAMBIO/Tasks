#include "task_loop.h"

#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * One live timer.  Entries sit in a singly linked list owned by the main
 * thread; a timer leaves the list when it is cancelled or (one-shot) when
 * it has fired.
 */
typedef struct loop_timer {
    unsigned int       id;
    dispatch_source_t  source;
    task_loop_fn       fn;
    task_loop_fn       destroy;
    void              *user_data;
    bool               repeats;
    bool               firing;      /* fn is on the stack right now       */
    bool               cancelled;   /* cancelled while firing: release
                                     * when fn returns                    */
    struct loop_timer *next;
} loop_timer_t;

static loop_timer_t *timers;        /* live timers, newest first          */
static unsigned int  last_id;       /* ids count up and are never reused  */

/*
 * timer_find — the live timer with this id.
 *
 * Inputs:
 *   id — timer id
 *
 * Output:
 *   the entry, or NULL when no live timer has that id.
 */
static loop_timer_t *
timer_find(unsigned int id)
{
    for (loop_timer_t *t = timers; t != NULL; t = t->next) {
        if (t->id == id) return t;
    }
    return NULL;
}

/*
 * timer_release — unlink a timer, stop its source and hand user_data to
 * its destroy hook.  The entry must not be firing.
 *
 * Inputs:
 *   t — a live timer
 */
static void
timer_release(loop_timer_t *t)
{
    for (loop_timer_t **link = &timers; *link != NULL; link = &(*link)->next) {
        if (*link == t) {
            *link = t->next;
            break;
        }
    }
    dispatch_source_cancel(t->source);
    dispatch_release(t->source);
    if (t->destroy != NULL) t->destroy(t->user_data);
    free(t);
}

/*
 * timer_fired — the dispatch source's event handler (main queue).
 *
 * The context is the timer's id, not its entry: an event already queued
 * when the timer was cancelled then finds nothing and does nothing,
 * instead of touching a freed entry.
 *
 * Inputs:
 *   context — the timer id, carried as a pointer-sized integer
 */
static void
timer_fired(void *context)
{
    loop_timer_t *t = timer_find((unsigned int)(uintptr_t)context);
    if (t == NULL) return;

    t->firing = true;
    t->fn(t->user_data);
    t->firing = false;

    if (t->cancelled || !t->repeats) timer_release(t);
}

/*
 * timer_start — create and start a timer.
 *
 * Inputs:
 *   seconds   — delay (and period, when repeating); not negative
 *   repeats   — false for one shot
 *   fn        — callback, not NULL
 *   user_data — its context
 *   destroy   — user_data's destroy hook, or NULL
 *
 * Output:
 *   the new timer's id, or 0 on failure.
 */
static unsigned int
timer_start(double seconds, bool repeats, task_loop_fn fn, void *user_data,
            task_loop_fn destroy)
{
    loop_timer_t *t = calloc(1, sizeof *t);
    if (t == NULL) return 0;

    t->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                       dispatch_get_main_queue());
    if (t->source == NULL) {
        free(t);
        return 0;
    }
    t->id        = ++last_id;
    t->fn        = fn;
    t->destroy   = destroy;
    t->user_data = user_data;
    t->repeats   = repeats;
    t->next      = timers;
    timers       = t;

    /* A little slack lets the system batch wakeups: a tenth of the
     * interval, but never more than a second, so a deadline minutes away
     * is still met to the second (as a GLib seconds-timer was). */
    uint64_t nsec   = (uint64_t)(seconds * (double)NSEC_PER_SEC);
    uint64_t leeway = nsec / 10 < NSEC_PER_SEC ? nsec / 10 : NSEC_PER_SEC;
    dispatch_source_set_timer(t->source,
                              dispatch_time(DISPATCH_TIME_NOW, (int64_t)nsec),
                              repeats ? nsec : DISPATCH_TIME_FOREVER, leeway);
    dispatch_set_context(t->source, (void *)(uintptr_t)t->id);
    dispatch_source_set_event_handler_f(t->source, timer_fired);
    dispatch_resume(t->source);
    return t->id;
}

/*
 * task_loop_timer_once — one-shot timer.
 * Inputs/output are documented in task_loop.h.
 */
unsigned int
task_loop_timer_once(double seconds, task_loop_fn fn, void *user_data,
                     task_loop_fn destroy)
{
    if (fn == NULL) return 0;
    return timer_start(seconds < 0 ? 0 : seconds, false, fn, user_data,
                       destroy);
}

/*
 * task_loop_timer_every — repeating timer.
 * Inputs/output are documented in task_loop.h.
 */
unsigned int
task_loop_timer_every(double seconds, task_loop_fn fn, void *user_data,
                      task_loop_fn destroy)
{
    if (fn == NULL || !(seconds > 0)) return 0;
    return timer_start(seconds, true, fn, user_data, destroy);
}

/*
 * task_loop_timer_cancel — stop a timer by id.
 * Inputs are documented in task_loop.h.
 */
void
task_loop_timer_cancel(unsigned int id)
{
    loop_timer_t *t = id != 0 ? timer_find(id) : NULL;

    if (t == NULL) return;
    if (t->firing) {
        t->cancelled = true;         /* timer_fired releases it */
        return;
    }
    timer_release(t);
}

/*
 * task_loop_main_async — queue fn on the main queue.
 * Inputs are documented in task_loop.h.
 */
void
task_loop_main_async(task_loop_fn fn, void *user_data)
{
    dispatch_async_f(dispatch_get_main_queue(), user_data, fn);
}

/*
 * task_loop_background — queue fn on a background queue.
 * Inputs are documented in task_loop.h.
 */
void
task_loop_background(task_loop_fn fn, void *user_data)
{
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                     user_data, fn);
}
