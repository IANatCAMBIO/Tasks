/* ===========================================================================
 * backup.h — backups of the shared database, through the shared module
 *
 * The copy, its verification, the retention and the schedule all belong to
 * the shared module (shared_db.h: shared_db_backup, shared_db_backup_if_due)
 * and its policy lives IN the database, in shared_settings, so one policy
 * serves Notes and Tasks: whichever app is running makes the backup when it
 * is due, and it is made once.  The keys are SHARED_SETTING_BACKUP_*:
 *
 *   backup_enabled       1|0, default SHARED_BACKUP_ENABLED_DEFAULT.
 *   backup_dir           destination folder; unset = the module's default
 *                        (`backups` beside the $CAMBIO_SHARED_DB file in a
 *                        sandboxed run, else ~/Library/Application
 *                        Support/camb.io/backups).
 *   backup_interval_min  minutes between backups, default
 *                        SHARED_BACKUP_INTERVAL_MIN_DEFAULT; 0 = only when
 *                        "Back Up Now" is pressed.
 *   backup_keep          files retained, SHARED_BACKUP_KEEP_MIN..MAX.
 *   backup_last_at       when the last backup was made (the module's).
 *   backup_last_error    why the last one failed (the module's).
 *
 * What this module adds is Tasks' part: the background thread (with its
 * OWN connection — a connection never crosses threads — so a slow
 * destination never blocks the UI), the periodic poll registered with the
 * shared scheduler (task_worker.h), and the one refusal the module cannot
 * know about: a destination inside the GTK app's live data directory
 * (task_paths.h).
 *
 * MAIN THREAD ONLY, apart from the pass's own thread.
 * =========================================================================== */

#ifndef TASK_BACKUP_H
#define TASK_BACKUP_H

#include "task_app.h"

/* How often the periodic worker asks the module whether a backup is due,
 * in minutes.  The module answers from the policy above, so a change in
 * Settings takes effect at the next poll without re-arming anything.     */
#define TASK_BACKUP_POLL_MIN 1

/* Completion callback; runs on the main thread.  `message` is a short
 * human-readable outcome (not owned by the callee).                        */
typedef void (*TaskBackupDoneFn)(TaskApp *app, gboolean ok, const gchar *message,
                                 gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_backup_setting() — one integer of the backup policy, read from
 * shared_settings on the app's connection, with the module's default when
 * it is unset (and backup_keep held to SHARED_BACKUP_KEEP_MIN..MAX).
 *
 * Inputs:
 *   app — the application context, its database open
 *   key — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN, _KEEP or _LAST_AT
 *
 * Output:
 *   the value; the default (0 for LAST_AT) when unset or unreadable.
 * ------------------------------------------------------------------------- */
gint64 task_backup_setting(TaskApp *app, const gchar *key);

/* ---------------------------------------------------------------------------
 * task_backup_set_setting() — write one integer of the backup policy to
 * shared_settings (a failure is logged).  Both apps read it from there.
 *
 * Inputs:
 *   app   — the application context, its database open
 *   key   — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN or _KEEP
 *   value — the new value
 * ------------------------------------------------------------------------- */
void task_backup_set_setting(TaskApp *app, const gchar *key, gint64 value);

/* ---------------------------------------------------------------------------
 * task_backup_dir() — where backups go: shared_db_backup_dir (the
 * backup_dir setting, else the module's default; created when missing),
 * unless that is a folder inside the GTK app's live data (refused).
 *
 * The single answer to "where?": Settings displays it and every pass asks
 * it first, so the label cannot disagree with the behaviour.
 *
 * Inputs:
 *   app — the application context, its database open
 *   why — receives the reason when the answer is NULL (free it)
 *
 * Output:
 *   the folder (free it), or NULL with *why set.
 * ------------------------------------------------------------------------- */
char *task_backup_dir(TaskApp *app, char **why);

/* ---------------------------------------------------------------------------
 * task_backup_set_dir() — make `dir` the backup destination, if it can be:
 * an absolute path to a writable folder, outside the live data.  A folder
 * that is refused is not stored.
 *
 * Inputs:
 *   app — the application context, its database open
 *   dir — the folder chosen
 *   why — receives the reason on refusal (free it), otherwise NULL
 *
 * Output:
 *   TRUE when it is now the destination.
 * ------------------------------------------------------------------------- */
gboolean task_backup_set_dir(TaskApp *app, const gchar *dir, char **why);

/* ---------------------------------------------------------------------------
 * task_backup_start() — "Back Up Now": one backup on a worker thread,
 * regardless of the schedule (shared_db_backup).  Refused, with `done`
 * called at once and the reason posted as a status message, when
 * task_backup_dir refuses; ignored (no callback) when a pass is already in
 * flight.  `done` may be NULL.  The outcome is also posted as a status
 * message.
 * ------------------------------------------------------------------------- */
void task_backup_start(TaskApp *app, TaskBackupDoneFn done, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_backup_init() — register the periodic worker with the shared
 * scheduler (task_worker.h): every TASK_BACKUP_POLL_MIN minutes, and once
 * when armed, a worker thread runs shared_db_backup_if_due, which makes a
 * backup only when the shared policy says one is due and claims it so that
 * a second app polling at once does not make another.  A poll that finds
 * nothing due says nothing; one that backs up or fails posts a status
 * message.  Call ONCE from main(), before any thread exists.
 * ------------------------------------------------------------------------- */
void task_backup_init(TaskApp *app);

#endif /* TASK_BACKUP_H */
