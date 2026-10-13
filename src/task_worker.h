/* ===========================================================================
 * task_worker.h — the periodic background pass, once.
 *
 * Several subsystems (the rotating backup, the recurrence pass, and the
 * syncs that used to live here) each wanted the same thing: a
 * config-driven timer that runs a pass every N minutes against the
 * database path it was armed with.  Each grew its own copy — an
 * identical flag, timer id, copied path, tick, destroy hook and arm
 * function, differing only in which config keys they read.
 *
 * Three copies is how they drifted.  The Settings "move the database"
 * flow (since deleted — gotcha 14) re-armed all three; the File → Open
 * Database path re-armed two, leaving the backup worker pointed at a
 * file that had just been moved — where it would open the missing path
 * and helpfully CREATE an empty database there.  That is a data-safety
 * bug produced purely by duplication, and
 * the fix is to have one scheduler that every worker is registered with,
 * so "re-arm everything" is one call that cannot miss a member.
 *
 * WHY THE PATH IS COPIED PER ARM: a timer outlives the string it was
 * given, and the database can move underneath it (see the sync-folder
 * warning in db.h).  Each armed worker owns its own copy, and re-arming
 * is what replaces it.
 *
 * Main thread only.  A worker's `run` is called on the main thread and
 * is expected to spawn its own thread with its own SQLite connection —
 * this schedules passes, it does not run them.  The timers are task_loop
 * timers (task_loop.h), so they fire under the Cocoa run loop.
 * =========================================================================== */

#ifndef TASK_WORKER_H
#define TASK_WORKER_H

#include "task_app.h"

/* Whether arming also runs a pass immediately.
 *
 *   NEVER  — arm the timer and nothing else (the backup: an unprompted
 *            copy at every launch is not wanted).
 *   ARMED  — one pass, but only when a timer was actually installed, and
 *            only when `ready` says so (a sync at interval 0 means the
 *            user asked for manual-only).
 *   ALWAYS — one pass even at interval 0 (the recurrence pass: repeats
 *            that came round while the app was closed are applied at
 *            launch whatever the timer says).
 */
typedef enum {
    TASK_WORKER_INITIAL_NEVER = 0,
    TASK_WORKER_INITIAL_ARMED,
    TASK_WORKER_INITIAL_ALWAYS,
} TaskWorkerInitial;

/* ---------------------------------------------------------------------------
 * What one worker is.  A static definition per subsystem; the scheduler
 * copies nothing but the pointer, so it must outlive the app (a string
 * literal and a file-static struct, in practice).
 *
 *   id               — short name, for diagnostics only.
 *   sort             — run ORDER: lower goes first, and equal values keep
 *                      registration order.  0 is the default and means
 *                      "no preference".  It exists because one ordering
 *                      is load-bearing: the recurrence pass runs before
 *                      anything that would push its results elsewhere.
 *   enabled_key      — config master switch (its default is the config
 *                      registry's); NULL means always enabled.
 *   interval_key     — config key holding the period in MINUTES (default
 *                      from the registry); <= 0 means "no timer" (manual
 *                      only).  NULL means the worker has no period.
 *   initial          — see above.
 *   running          — the subsystem's own in-flight flag, so a tick that
 *                      lands on a still-running pass is skipped.  Every
 *                      run() also guards internally; this only avoids the
 *                      pointless call.  May be NULL.
 *   timer            — where to keep the task_loop timer id (0 = none).
 *                      Borrowed, and owned by whoever declared it.
 *   run              — start one pass (main thread).
 *   ready            — may a pass run right now?  NULL means always.
 * ------------------------------------------------------------------------- */
typedef struct {
    const gchar      *id;
    gint              sort;
    const gchar      *enabled_key;
    const gchar      *interval_key;
    TaskWorkerInitial initial;
    gboolean         *running;
    guint            *timer;
    void            (*run)(TaskApp *app, const gchar *db_path);
    gboolean        (*ready)(TaskApp *app);
} TaskWorkerDef;

/* ---------------------------------------------------------------------------
 * task_worker_register() — add `def` to the scheduler.  Call once per
 * worker at startup, before any thread exists; the registry is
 * process-wide and unlocked, like the other startup registries.
 * ------------------------------------------------------------------------- */
void task_worker_register(const TaskWorkerDef *def);

/* ---------------------------------------------------------------------------
 * task_worker_arm() — (re)arm ONE worker against `db_path`.
 *
 * Disarms first, so it is also the re-arm entry point and is safe to
 * call repeatedly — which is what a Settings change does.
 * ------------------------------------------------------------------------- */
void task_worker_arm(TaskApp *app, const TaskWorkerDef *def,
                     const gchar *db_path);

/* ---------------------------------------------------------------------------
 * task_worker_arm_all() — (re)arm EVERY registered worker against
 * `db_path`.
 *
 * This is the call that must be used wherever the database changes
 * identity — startup, a directory switch, File → Open Database.  Naming
 * individual workers there is what let one go missing.
 * ------------------------------------------------------------------------- */
void task_worker_arm_all(TaskApp *app, const gchar *db_path);

/* ---------------------------------------------------------------------------
 * task_worker_disarm_all() — cancel every registered worker's timer and
 * free what it held.  Called once at quit, before the TaskApp is freed, so
 * no tick can fire into a closed database.  Passes already running on
 * their own thread are not interrupted; they hold their own connection.
 * ------------------------------------------------------------------------- */
void task_worker_disarm_all(void);

#endif /* TASK_WORKER_H */
