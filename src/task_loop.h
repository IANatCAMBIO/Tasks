/*
 * task_loop — the core's main-loop services: timers, "run this on the main
 * thread next", and "run this on a background thread".
 *
 * The core used to get these from the GLib main loop (g_timeout_add,
 * g_idle_add, GThread).  Nothing pumps a GLib main loop under
 * NSApplication, so a GLib timer armed in this build would simply never
 * fire.  This module provides the same three services on Grand Central
 * Dispatch: timers and task_loop_main_async run on the MAIN QUEUE, which
 * both NSApplication's run loop and CFRunLoopRun drain.
 *
 * Timers are named by an id, as GLib sources were: 0 is "no timer", ids are
 * never reused, and cancelling an id that has already fired or been
 * cancelled is a harmless no-op.  That is what lets a caller keep an id in
 * a variable without tracking whether the timer is still alive.
 *
 * Timers run on the monotonic clock, which does not advance while the
 * machine sleeps: a timer spanning a sleep fires late by the length of the
 * sleep.  The recurrence pass already bounds that (RECUR_MAX_SLEEP_SEC).
 *
 * THREADING: the timer functions are MAIN THREAD ONLY.  task_loop_main_async
 * and task_loop_background may be called from any thread.
 *
 * Plain C on libdispatch; no GLib, no Foundation.  Objective-C code is free
 * to use dispatch_after and blocks directly for its own UI timing — this
 * module exists for the C core and for C callbacks that need a destroy
 * hook.
 */

#ifndef TASK_LOOP_H
#define TASK_LOOP_H

/* A callback taking the caller's context pointer. */
typedef void (*task_loop_fn)(void *user_data);

/*
 * task_loop_timer_once — call `fn` once on the main queue after `seconds`.
 *
 * Inputs:
 *   seconds   — delay; values below 0 are treated as 0
 *   fn        — the callback, not NULL
 *   user_data — passed to fn and to destroy
 *   destroy   — called exactly once when the timer is finished with
 *               user_data: after fn returns, or when the timer is cancelled
 *               before firing; may be NULL
 *
 * Output:
 *   the timer id (never 0), or 0 when fn is NULL or the timer cannot be
 *   created — in which case destroy is NOT called.
 */
unsigned int task_loop_timer_once(double seconds, task_loop_fn fn,
                                  void *user_data, task_loop_fn destroy);

/*
 * task_loop_timer_every — call `fn` on the main queue every `seconds`,
 * first after one full period, until cancelled.
 *
 * Inputs:
 *   seconds   — the period; must be greater than 0
 *   fn        — the callback, not NULL
 *   user_data — passed to fn and to destroy
 *   destroy   — called exactly once, when the timer is cancelled; may be
 *               NULL
 *
 * Output:
 *   the timer id (never 0), or 0 on invalid input or failure — in which
 *   case destroy is NOT called.
 */
unsigned int task_loop_timer_every(double seconds, task_loop_fn fn,
                                   void *user_data, task_loop_fn destroy);

/*
 * task_loop_timer_cancel — stop a timer.  After this returns fn is never
 * called again.  Cancelling from inside the timer's own callback is safe:
 * destroy then runs when the callback returns, not underneath it.
 *
 * Inputs:
 *   id — a timer id; 0 and ids no longer live are ignored
 */
void task_loop_timer_cancel(unsigned int id);

/*
 * task_loop_main_async — call `fn` on the main queue, after the current
 * main-queue work has finished.  This is how a background thread hands a
 * result back, and how main-thread code defers work "until after this
 * event".
 *
 * Inputs:
 *   fn        — the callback, not NULL
 *   user_data — passed to fn, which owns it from here
 */
void task_loop_main_async(task_loop_fn fn, void *user_data);

/*
 * task_loop_background — call `fn` on a background thread (a utility-
 * priority global queue).  fn must not touch anything owned by the main
 * thread: no TaskApp, no shared SQLite connection, no UI.  It reports back
 * with task_loop_main_async.
 *
 * Inputs:
 *   fn        — the work, not NULL
 *   user_data — passed to fn, which owns it from here
 */
void task_loop_background(task_loop_fn fn, void *user_data);

#endif /* TASK_LOOP_H */
