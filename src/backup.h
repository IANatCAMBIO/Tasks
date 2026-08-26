/* ===========================================================================
 * backup.h — optional rotating database backups for Tasks
 *
 * OFF by default.  Enabled in Settings → Database, where the user may
 * also pick a destination directory — and when they have not, backups go
 * to the DEFAULT DATABASE LOCATION under the home directory
 * (`<user data dir>/tasks`, i.e. ~/.local/share/tasks).  Switching the
 * feature on therefore always does something; there is no enabled-but-
 * inert state to fall into.
 *
 * The reason to choose a folder anyway: the point of the feature is a copy
 * somewhere INDEPENDENT of wherever the live file lives, so one mishap
 * cannot take both.  That matters here because the live database routinely
 * sits in a sync folder (iCloud Drive), and a sync daemon can replace or
 * re-generate a file underneath an open SQLite connection.  The default
 * is off in the home directory, which is already independent of a synced
 * database — but it is the SAME folder when the database itself is at its
 * default location, and Settings says so rather than implying otherwise.
 *
 * Each pass writes "tasks-YYYYMMDD-HHMMSS.db" into that directory and
 * then prunes the oldest, keeping at most `backup_keep` files — bounded
 * by design, so an hourly timer cannot fill a disk.  A pass whose source
 * has not changed since the last backup does nothing at all, so an idle
 * app writes nothing.
 *
 * Safety rules this module will not bend:
 *   - the copy is VACUUM INTO (task_db_copy_file), never a byte copy: it
 *     runs in a read transaction and so cannot capture a torn page;
 *   - every new backup is VERIFIED (task_db_verify_file) before it counts,
 *     and a copy that fails verification is removed rather than left to
 *     be mistaken for a good one;
 *   - PRUNING HAPPENS ONLY AFTER a new backup has verified, so a failing
 *     backup can never erode the history that is already there.
 *
 * Threading matches the sync engines: a worker thread with its OWN
 * SQLite connection (a connection never crosses threads), so a slow or
 * unreachable destination — a network mount, a sleeping external disk —
 * never blocks the UI.  Completion is marshalled back with g_idle_add.
 *
 * Config keys (all in the [tasks] group):
 *   backup_enabled       1|0, default 0 — the master switch.
 *   backup_dir           destination directory; absent = the default
 *                        database location under the home directory.
 *   backup_interval_min  minutes between passes, default 60; 0 = only
 *                        when "Back Up Now" is pressed.
 *   backup_keep          how many files to retain, default 10.
 * =========================================================================== */

#ifndef TASK_BACKUP_H
#define TASK_BACKUP_H

#include "app.h"

/* Default retention and cadence, shared with the Settings spin buttons so
 * the UI and the timer cannot disagree about what "unset" means.           */
#define TASK_BACKUP_KEEP_DEFAULT     10
#define TASK_BACKUP_INTERVAL_DEFAULT 60

/* Completion callback; runs on the main thread.  `message` is a short
 * human-readable summary or error (not owned by the callee).               */
typedef void (*TaskBackupDoneFn)(TaskApp *app, gboolean ok, const gchar *message,
                                 gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_backup_dir() — where backups actually go: `backup_dir` when it names
 * something, otherwise the default database directory under the home
 * directory, which is CREATED if missing so the fallback is always usable.
 *
 * Returns a new string (g_free).  Never NULL.  This is the single answer
 * to "where?" — Settings displays it and the worker writes to it, so the
 * label can never disagree with the behavior.
 * ------------------------------------------------------------------------- */
gchar *task_backup_dir(void);

/* ---------------------------------------------------------------------------
 * task_backup_ready() — is the feature usable right now?
 *
 * Returns TRUE when backups are enabled and task_backup_dir() is a writable
 * directory.  On FALSE, *reason (optional; g_free) says which of those
 * failed, in words fit for a status bar or a dialog.  Callers use it to
 * explain themselves rather than failing silently.
 * ------------------------------------------------------------------------- */
gboolean task_backup_ready(gchar **reason);

/* ---------------------------------------------------------------------------
 * task_backup_start() — run one backup pass on a worker thread.
 *
 * Early-outs, each with a status message: disabled or misconfigured (see
 * task_backup_ready), and "already running" (which does not fire `done`).
 * A pass whose source is unchanged since the last backup reports that and
 * writes nothing.  `done` may be NULL.  Main thread only.
 * ------------------------------------------------------------------------- */
void task_backup_start(TaskApp *app, const gchar *db_path,
                       TaskBackupDoneFn done, gpointer user_data);

/* ---------------------------------------------------------------------------
 * task_backup_auto_start() — (re)install the periodic timer from
 * `backup_interval_min`, or remove it when backups are off.  Safe to call
 * again whenever the settings change; must be called again after
 * task_app_switch_database, since the timer carries the database path.
 *
 * Unlike the sync timers this does NOT run an immediate pass: an app that
 * has just started has nothing new to preserve, and a backup on every
 * launch would churn the rotation for anyone who opens the app often.
 * ------------------------------------------------------------------------- */
void task_backup_auto_start(TaskApp *app, const gchar *db_path);

/* ---------------------------------------------------------------------------
 * task_backup_init() — register the backup's periodic worker with the
 * shared scheduler (see task_worker.h).  Call ONCE from main(), before
 * any thread exists.
 * ------------------------------------------------------------------------- */
void task_backup_init(TaskApp *app);

#endif /* TASK_BACKUP_H */
