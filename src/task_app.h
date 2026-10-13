/*
 * task_app — the shared application context and its event bus.
 *
 * One TaskApp is created at startup and passed to every part of the
 * program.  It owns the open database and hosts the four events the
 * windows subscribe to.  It holds NO user-interface objects: windows are
 * owned by the AppKit layer (see porting/INTERFACES.md), which is what lets
 * the core, the seed tool and the tests link without AppKit.
 *
 * The four events are lists rather than single hooks because more than one
 * subscriber is normal.  They start empty, and firing an empty list is a
 * no-op — which is what makes every notify safe both before the library
 * window exists and after it has gone.
 *
 * MAIN THREAD ONLY.  A background job hands its result to the main thread
 * with task_loop_main_async before it touches the TaskApp.
 */

#ifndef TASK_APP_H
#define TASK_APP_H

#include <glib.h>
#include "db.h"

/*
 * TaskApp — global application state.
 *
 *   db             — the open tasks database, or NULL before one is opened.
 *                    Owned: task_app_free closes it.
 *   changed_l      — listeners for a FULL refresh (sidebar + task pane +
 *                    open editors): lists created, renamed or deleted.
 *   tasks_l        — listeners for the LIGHTER event, the task pane only.
 *                    Editor saves and subtask/attachment edits use it —
 *                    they can never change the sidebar.
 *   notes_l        — listeners for a change to Notes' data made elsewhere
 *                    (a note renamed): only what Tasks shows of notes —
 *                    the editors' "From note" lines — is re-read.
 *   status_l       — listeners for a one-line event message.
 *   listener_next  — the next subscription id.
 *   backup_running — TRUE while the backup's worker (backup.h) is in
 *                    flight; blocks a second concurrent pass.
 *   backup_timer   — the backup's periodic task_loop timer id, or 0.
 *   watch_timer    — the outside-change watch's task_loop timer id, or 0
 *                    (task_app_watch_start).
 */
typedef struct TaskApp {
    TaskDatabase *db;
    GSList       *changed_l;
    GSList       *tasks_l;
    GSList       *notes_l;
    GSList       *status_l;
    guint         listener_next;
    gboolean      backup_running;
    guint         backup_timer;
    guint         watch_timer;
} TaskApp;

/*
 * task_app_new — allocate an empty context (no database, no listeners).
 *
 * Output:
 *   the new context; free it with task_app_free.  Never NULL.
 */
TaskApp *task_app_new(void);

/*
 * task_app_free — stop the outside-change watch, close the database, drop
 * any listener still subscribed and free the context.  The backup timer
 * must already be cancelled (the caller disarms the workers first; see
 * task_worker_disarm_all).
 *
 * Inputs:
 *   app — the context, or NULL (ignored)
 */
void task_app_free(TaskApp *app);

/*
 * task_app_status — post a one-line event message to every status listener
 * (printf-style).  A no-op until something is listening.
 *
 * Inputs:
 *   app — the context; NULL is ignored
 *   fmt — printf format, then its arguments
 */
void task_app_status(TaskApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* Why a write was not saved, in the user's words: the one text every
 * failed-write message gives (task_app_write_failed, the editor's notice). */
#define TASK_APP_WRITE_FAILED_REASON \
    "the database could not be written: another app or script held it " \
    "for too long, or the disk failed"

/*
 * task_app_write_failed — a write made for the user did not reach the
 * database (a db.h writer returned FALSE): say so in the status bar and
 * fire the FULL refresh, so every pane shows what the database really
 * holds rather than the change that was not saved.
 *
 * Inputs:
 *   app  — the context; NULL is ignored
 *   what — what was not saved, as a sentence subject ("The status of
 *          “Call Bob”", "3 tasks")
 */
void task_app_write_failed(TaskApp *app, const gchar *what);

/*
 * task_app_notify_changed — fire the FULL refresh event.  For structural
 * changes.  Safe with no listeners.
 *
 * Inputs:
 *   app — the context; NULL is ignored
 */
void task_app_notify_changed(TaskApp *app);

/*
 * task_app_notify_tasks — fire the task-pane event, for changes that cannot
 * touch the sidebar.  Falls back to the full event when nothing listens for
 * this one, so a caller never has to ask which listeners exist.
 *
 * Inputs:
 *   app — the context; NULL is ignored
 */
void task_app_notify_tasks(TaskApp *app);

/*
 * task_app_check_outside — has Tasks' or Notes' data been changed by
 * ANOTHER connection since the last look (Notes, a script, this app's own
 * background threads — task_db_outside_change on the main connection)?
 * When Tasks' data has, fire the FULL refresh event, so every pane and
 * editor re-reads it (each keeps its selection and scroll).  When only
 * Notes' data has, fire the notes event, so the editors re-read the titles
 * of the notes their tasks came from.  A commit that touched only the
 * shared settings fires nothing, and this connection's own writes never
 * count, so the app's ordinary edits cause no second refresh.
 *
 * Inputs:
 *   app — the context; NULL or no database open is a no-op
 *
 * Output:
 *   the TASK_DB_CHANGED_* bits found (0: nothing changed, nothing fired).
 */
guint task_app_check_outside(TaskApp *app);

/*
 * task_app_watch_start — check for outside changes every
 * SHARED_DB_POLL_SECONDS on the main loop (task_app_check_outside).  The
 * app also checks when it becomes active.  Restarting replaces the timer.
 *
 * Inputs:
 *   app — the context
 */
void task_app_watch_start(TaskApp *app);

/*
 * task_app_watch_stop — cancel the watch.  Safe when it is not running.
 *
 * Inputs:
 *   app — the context; NULL is ignored
 */
void task_app_watch_stop(TaskApp *app);

/* Listener signatures: a notify listener, and a status listener that also
 * receives the message (borrowed; copy it to keep it). */
typedef void (*TaskAppNotifyFn)(TaskApp *app, gpointer user_data);
typedef void (*TaskAppStatusFn)(TaskApp *app, const gchar *message,
                                gpointer user_data);

/*
 * task_app_listen_changed / _tasks / _notes / _status — subscribe to an
 * event.
 *
 * Listeners fire in registration order.  A listener MUST be removed before
 * the thing it refreshes is destroyed; the library window drops its four
 * BEFORE the editors close, because a closing editor's final save fires
 * the task event.  Removing a listener from inside its own callback is
 * safe; one added during a fire is not called until the next fire.
 *
 * Inputs:
 *   app       — the context
 *   fn        — the callback, not NULL
 *   user_data — passed through to fn (borrowed, never freed here)
 *
 * Output:
 *   a subscription id for task_app_unlisten (ids are never reused), or 0
 *   when app or fn is NULL.
 */
guint task_app_listen_changed(TaskApp *app, TaskAppNotifyFn fn,
                              gpointer user_data);
guint task_app_listen_tasks(TaskApp *app, TaskAppNotifyFn fn,
                            gpointer user_data);
guint task_app_listen_notes(TaskApp *app, TaskAppNotifyFn fn,
                            gpointer user_data);
guint task_app_listen_status(TaskApp *app, TaskAppStatusFn fn,
                             gpointer user_data);

/*
 * task_app_unlisten — remove a subscription from whichever event holds it.
 *
 * Inputs:
 *   app — the context; NULL is ignored
 *   id  — a subscription id; 0 and unknown ids are ignored
 */
void task_app_unlisten(TaskApp *app, guint id);

#endif /* TASK_APP_H */
