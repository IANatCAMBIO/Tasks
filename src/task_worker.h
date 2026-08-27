/* ===========================================================================
 * task_worker.h — the periodic background pass, once.
 *
 * Three subsystems (the Google Tasks sync, the Notes mirror and the
 * rotating backup) each wanted the same thing: a config-driven timer
 * that runs a pass every N minutes against the database path it was
 * armed with.  Each grew its own copy — an identical flag, GSource id,
 * g_strdup'd path, tick, GDestroyNotify and arm function, differing only
 * in which config keys they read.
 *
 * Three copies is how they drifted.  task_app_switch_database re-armed
 * all three; the File → Open Database path re-armed two, leaving the
 * backup worker pointed at a file that had just been moved — where it
 * would open the missing path and helpfully CREATE an empty database
 * there.  That is a data-safety bug produced purely by duplication, and
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
 * this schedules passes, it does not run them.
 * =========================================================================== */

#ifndef TASK_WORKER_H
#define TASK_WORKER_H

#include "app.h"

/* Whether arming also runs a pass immediately.
 *
 *   NEVER  — arm the timer and nothing else (the backup: an unprompted
 *            copy at every launch is not wanted).
 *   ARMED  — one pass, but only when a timer was actually installed, and
 *            only when `ready` says so (the Google sync: at interval 0
 *            the user asked for manual-only, and there is nothing to
 *            sync while signed out).
 *   ALWAYS — one pass even at interval 0 (the Notes mirror: its view has
 *            to be POPULATED before the user can act on it, so "manual
 *            only" still means "fill it in now").
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
 *                      "no preference", which is right for almost every
 *                      worker.  It exists because one ordering is load-
 *                      bearing and must not depend on which plugin the
 *                      loader happened to open first: the Notes mirror
 *                      runs BEFORE the Google sync, so a new action item
 *                      is mirrored and then pushed on to Google by a
 *                      single press of Sync rather than taking two.
 *   enabled_key      — config master switch; NULL means always enabled.
 *   enabled_default  — its default when the key is unset.
 *   interval_key     — config key holding the period in MINUTES; <= 0
 *                      means "no timer" (manual only).
 *   interval_default — its default when the key is unset.
 *   initial          — see above.
 *   running          — the subsystem's own in-flight flag, so a tick that
 *                      lands on a still-running pass is skipped.  Every
 *                      run() also guards internally; this only avoids the
 *                      pointless call.  May be NULL.
 *   timer            — where to keep the GSource id.  Borrowed, and owned
 *                      by whoever declared it.
 *   run              — start one pass (main thread).
 *   ready            — may a pass run right now?  NULL means always.
 *   on_arm           — run just after the master switch passes and BEFORE
 *                      the timer is installed.  NULL for nothing.  The
 *                      Notes mirror uses it to carry already-mirrored
 *                      items over when the target list setting changed
 *                      while the integration was switched off.
 * ------------------------------------------------------------------------- */
typedef struct {
    const gchar      *id;
    gint              sort;
    const gchar      *enabled_key;
    gboolean          enabled_default;
    const gchar      *interval_key;
    gint              interval_default;
    TaskWorkerInitial initial;
    gboolean         *running;
    guint            *timer;
    void            (*run)(TaskApp *app, const gchar *db_path);
    gboolean        (*ready)(TaskApp *app);
    void            (*on_arm)(TaskApp *app);
    /* The user explicitly asked for a pass (Sync Now) but `ready` said
     * no.  A chance to do something about it rather than appear to do
     * nothing — the Google sync opens its sign-in flow here.  NULL means
     * stay silent, which is right for a worker whose "not now" is not
     * the user's to fix.                                                */
    void            (*on_blocked)(TaskApp *app, const gchar *db_path);
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
 * task_worker_run_all() — run every enabled worker's pass NOW.
 *
 * This is what "Sync Now" means.  It used to be a handler that named the
 * Notes mirror and the Google sync in order, which was wrong twice over:
 * a third integration would have had to be added to it by hand, and the
 * button was gated on Google's setting while also running Notes.
 *
 * Order is registration order, which is deliberate rather than
 * incidental: a cheap local pass registered first has its results in the
 * database before a network pass reads them, so an item picked up from
 * one integration can reach another in a single press.
 *
 * A worker that is switched off, already running, or whose `ready` says
 * no is skipped silently — the button means "bring everything up to
 * date", and a worker with nothing to do has done that.
 * ------------------------------------------------------------------------- */
void task_worker_run_all(TaskApp *app, const gchar *db_path);

/* task_worker_any_enabled() — is there anything for "Sync Now" to do?
 * Drives whether the control is shown at all.                             */
gboolean task_worker_any_enabled(void);

/* ---------------------------------------------------------------------------
 * task_worker_remove_owner() — remove everything plugin `owner`
 * registered here.  Called when a plugin is switched off while the app is
 * running; the app's OWN registrations are unowned and never match.
 * ------------------------------------------------------------------------- */
void task_worker_remove_owner(const gchar *owner);

/* ---------------------------------------------------------------------------
 * task_worker_arm_owner() — arm only the workers plugin `owner`
 * registered.  For bringing one re-enabled plugin back without running
 * every other worker's initial pass as a side effect.
 * ------------------------------------------------------------------------- */
void task_worker_arm_owner(TaskApp *app, const gchar *owner,
                           const gchar *db_path);

#endif /* TASK_WORKER_H */
