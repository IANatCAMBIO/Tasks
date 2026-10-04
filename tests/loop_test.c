/*
 * loop_test.c — the main-loop service (src/task_loop.h): timers fire on the
 * main queue, fire the right number of times, stop when cancelled, release
 * their context exactly once, and a background job can hand a result back.
 *
 * The run loop is spun with check_spin, as NSApplication's would run it.
 */

#include "check.h"
#include "task_loop.h"

#include <pthread.h>
#include <stdbool.h>

/* What one timer's callbacks have seen. */
typedef struct {
    int          fired;
    int          destroyed;
    bool         on_main;           /* the callback ran on the main thread */
    bool         work_on_main;      /* the background work did (it must not) */
    unsigned int cancel_id;         /* a timer to cancel from the callback */
} probe_t;

/*
 * probe_fire — a timer callback: count the call and note the thread.
 *
 * Inputs:
 *   data — the probe_t
 */
static void
probe_fire(void *data)
{
    probe_t *p = data;

    p->fired++;
    p->on_main = pthread_main_np() != 0;
}

/*
 * probe_fire_and_cancel — a timer callback that cancels a timer (its own,
 * when cancel_id names it) from inside the callback.
 *
 * Inputs:
 *   data — the probe_t
 */
static void
probe_fire_and_cancel(void *data)
{
    probe_t *p = data;

    p->fired++;
    task_loop_timer_cancel(p->cancel_id);
}

/*
 * probe_destroy — a timer's destroy hook: count the call.
 *
 * Inputs:
 *   data — the probe_t
 */
static void
probe_destroy(void *data)
{
    ((probe_t *)data)->destroyed++;
}

/*
 * background_work — runs off the main thread and hands the probe back.
 *
 * Inputs:
 *   data — the probe_t
 */
static void
background_work(void *data)
{
    probe_t *p = data;

    p->work_on_main = pthread_main_np() != 0;
    task_loop_main_async(probe_fire, p);
}

/*
 * test_once — a one-shot timer fires once, on the main thread, and its
 * context is released once.
 */
static void
test_once(void)
{
    probe_t      p  = { 0 };
    unsigned int id = task_loop_timer_once(0.05, probe_fire, &p, probe_destroy);

    check(id != 0, "once: returns an id");
    check(p.fired == 0, "once: does not fire synchronously");
    check_spin(0.4);
    check(p.fired == 1, "once: fires exactly once");
    check(p.on_main, "once: fires on the main thread");
    check(p.destroyed == 1, "once: context released after firing");

    task_loop_timer_cancel(id);     /* stale id: must be a harmless no-op */
    check(p.destroyed == 1, "once: cancelling a fired id does nothing");
}

/*
 * test_cancel — a cancelled timer never fires and releases its context.
 */
static void
test_cancel(void)
{
    probe_t      p  = { 0 };
    unsigned int id = task_loop_timer_once(0.05, probe_fire, &p, probe_destroy);

    task_loop_timer_cancel(id);
    check(p.destroyed == 1, "cancel: context released at once");
    check_spin(0.3);
    check(p.fired == 0, "cancel: a cancelled timer never fires");
    check(p.destroyed == 1, "cancel: context released exactly once");
    task_loop_timer_cancel(0);
    check(p.destroyed == 1, "cancel: id 0 is ignored");
}

/*
 * test_every — a repeating timer keeps firing until cancelled.
 */
static void
test_every(void)
{
    probe_t      p  = { 0 };
    unsigned int id = task_loop_timer_every(0.05, probe_fire, &p, probe_destroy);

    check(id != 0, "every: returns an id");
    check_spin(0.5);
    check(p.fired >= 3, "every: fires repeatedly");
    check(p.destroyed == 0, "every: context kept while running");

    int seen = p.fired;
    task_loop_timer_cancel(id);
    check_spin(0.3);
    check(p.fired == seen, "every: stops when cancelled");
    check(p.destroyed == 1, "every: context released on cancel");

    check(task_loop_timer_every(0, probe_fire, &p, NULL) == 0,
          "every: a zero period is refused");
    check(task_loop_timer_once(0.01, NULL, &p, NULL) == 0,
          "once: a NULL callback is refused");
}

/*
 * test_cancel_inside — a repeating timer cancelled from its own callback
 * stops, and its context is released after the callback, exactly once.
 */
static void
test_cancel_inside(void)
{
    probe_t p = { 0 };

    p.cancel_id = task_loop_timer_every(0.05, probe_fire_and_cancel, &p,
                                        probe_destroy);
    check_spin(0.4);
    check(p.fired == 1, "cancel inside: no call after cancelling itself");
    check(p.destroyed == 1, "cancel inside: context released exactly once");
}

/*
 * test_ids — ids are distinct and never reused.
 */
static void
test_ids(void)
{
    probe_t      p = { 0 };
    unsigned int a = task_loop_timer_once(10, probe_fire, &p, NULL);
    unsigned int b = task_loop_timer_once(10, probe_fire, &p, NULL);

    task_loop_timer_cancel(a);
    unsigned int c = task_loop_timer_once(10, probe_fire, &p, NULL);
    check(a != b && b != c && a != c, "ids: never reused");
    task_loop_timer_cancel(b);
    task_loop_timer_cancel(c);
}

/*
 * test_background — background work runs off the main thread and its
 * hand-back runs on it.
 */
static void
test_background(void)
{
    probe_t p = { 0 };

    p.work_on_main = true;          /* until the work says otherwise */
    task_loop_background(background_work, &p);
    for (int i = 0; i < 40 && p.fired == 0; i++) check_spin(0.05);
    check(!p.work_on_main, "background: runs off the main thread");
    check(p.fired == 1 && p.on_main, "background: hands back to the main thread");
}

/*
 * main — run the main-loop tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    test_once();
    test_cancel();
    test_every();
    test_cancel_inside();
    test_ids();
    test_background();
    return check_result();
}
