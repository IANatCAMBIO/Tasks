/* ===========================================================================
 * db.h — SQLite storage for Tasks
 *
 * The database is the ONE file Notes and Tasks share.  Its schema, its
 * stamps (application_id + user_version), the refusal of every other
 * file (an old tasks.db included), WAL, the busy timeout and where the file
 * lives all belong to the shared module (shared_db.h); task_db_open is a
 * connection to it, and this module is Tasks' queries over Tasks' tables:
 *
 *   list_groups  id, name, position              (local-only)
 *   lists        id, name, emoji, position, group_id (FK → list_groups.id;
 *                NULL = ungrouped), updated_at, deleted
 *   tasks        id, list_id, parent_id (NULL = top-level; ONE level of
 *                nesting only — a subtask can never be a parent),
 *                title, notes, due (unix local midnight; 0 = none),
 *                due_time (the time of day that date means, minutes past
 *                local midnight), status (TaskStatus), pinned, priority
 *                (sorts first in every view), position, updated_at,
 *                deleted, completed_at,
 *                recur_interval / recur_unit / recur_time / recur_lead /
 *                recur_next / recur_start (the recurrence schedule),
 *                scheduled_start / scheduled_duration (the day planner)
 *   attachments  id, task_id, path, added_at
 *   sync_state   key, value
 *
 * NOTHING belonging to a particular INTEGRATION is here.  An integration
 * keeps its per-row state in its own SIDE TABLE keyed by row id: the
 * Google Calendar sync's gcal_* tables are described in gcal_store.h.
 * A task a Notes action item IS has a note_task row naming it (the shared
 * schema); Tasks reads that link (task_db_task_note) and never purges such
 * a task.
 *
 * Deletion is a SOFT flag (`deleted` = tombstone): a tombstoned row is
 * hidden everywhere until it is purged.  Every mutation of a task's own
 * fields stamps `updated_at` (unix seconds).
 * =========================================================================== */

#ifndef TASK_DB_H
#define TASK_DB_H

#include <glib.h>
#include <sqlite3.h>

#include "shared_db.h"

/* ---------------------------------------------------------------------------
 * TaskDbHealth — the outcome of one health pass over a database.
 *
 * `ok` is the only thing a caller normally acts on; `ran` is what tells
 * "the checks found problems" apart from "the checks could not be run",
 * which are a very different sentence to put in front of a user — and the
 * dividing line is whether the checks came back with ANYTHING, not
 * whether sqlite returned an error, because a corrupt file does both.
 * `detail` is NULL when there is nothing to say; `when` is unix time, and
 * 0 means no pass has been made.
 * ------------------------------------------------------------------------- */
typedef struct {
    gboolean  ok;                    /* both checks ran AND both passed     */
    gboolean  ran;                   /* both checks actually executed       */
    gint64    when;                  /* unix time of the pass, 0 = never    */
    gchar    *detail;                /* what went wrong, or NULL (owned)    */
} TaskDbHealth;

/* ---------------------------------------------------------------------------
 * TaskDatabase — one open connection.  A connection must not cross threads:
 * the sync worker opens its own on the same path (task_db_open).  The
 * SQLite handle is shared_db_handle(db->shared), the path
 * shared_db_file(db->shared).
 * ------------------------------------------------------------------------- */
typedef struct {
    shared_db *shared;               /* the connection (owned)              */
    /* The last health pass over this file, held IN MEMORY for the life of
     * the connection and deliberately never written down — see
     * task_db_health_check.                                               */
    TaskDbHealth health;
    /* The 'tasks' change revision (shared_db_rev) of the data this
     * connection's app last loaded or wrote; -1 when unknown.  Kept by
     * task_db_outside_change and by every commit of this connection.      */
    int64_t      tasks_rev;
    /* Another connection committed while this one was committing, so that
     * change is still to be looked at by task_db_outside_change.          */
    gboolean     outside_pending;
} TaskDatabase;

/* ---------------------------------------------------------------------------
 * TaskStatus — a task's progress, and the ONLY completion state stored.
 * The values are the on-disk encoding of tasks.status, defined once for
 * both apps by the shared module (SHARED_DB_TASK_STATUS_*, shared_db.h);
 * New is 0 so a freshly INSERTed row needs no explicit value.
 *
 * DONE is exactly the old boolean `done`: it strikes the title through,
 * hides the row under the completed-visibility toggle, stamps
 * completed_at.  NEW and IN_PROGRESS are both "not done".
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_STATUS_NEW         = SHARED_DB_TASK_STATUS_NEW,
    TASK_STATUS_IN_PROGRESS = SHARED_DB_TASK_STATUS_IN_PROGRESS,
    TASK_STATUS_DONE        = SHARED_DB_TASK_STATUS_DONE
} TaskStatus;

/* Number of values, for the editor's dropdown and bounds checks.           */
#define TASK_STATUS_N_VALUES 3

/* task_status_label() — the user-facing name ("New" / "In Progress" /
 * "Done").  Returns a static string; an out-of-range value reads "New"
 * so a hand-edited database can never blank a cell.                        */
const gchar *task_status_label(TaskStatus status);

/* ---------------------------------------------------------------------------
 * task_status_apply_done() — the SINGLE rule mapping a binary done flag
 * onto the tri-state status, shared by every source that only knows
 * "done or not": the task list's checkbox column and the context menu.
 *
 *   done = TRUE   → TASK_STATUS_DONE.
 *   done = FALSE  → TASK_STATUS_IN_PROGRESS when `cur` was DONE (unticking
 *                   an item means work resumed on it, not that it was
 *                   never started), otherwise `cur` UNCHANGED — so a New
 *                   task stays New, and a round trip through a
 *                   done-only system cannot silently promote it.
 * ------------------------------------------------------------------------- */
TaskStatus task_status_apply_done(TaskStatus cur, gboolean done);

/* ---------------------------------------------------------------------------
 * TaskRecurUnit — the unit of a task's repeat interval.  Like TaskStatus
 * these values ARE the on-disk encoding of tasks.recur_unit, so they must
 * not be renumbered; MINUTE is 0 so a row that has never been given a
 * recurrence needs no explicit value (it is inert either way, because
 * recur_interval = 0 is what means "does not recur").
 *
 * They are in ASCENDING duration order, which the editor's unit combo
 * relies on (its active index is the enum value) and which recur.c's
 * period arithmetic reads as a switch.
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_RECUR_MINUTE = 0,
    TASK_RECUR_HOUR   = 1,
    TASK_RECUR_DAY    = 2,
    TASK_RECUR_WEEK   = 3,
    TASK_RECUR_MONTH  = 4,
    TASK_RECUR_YEAR   = 5
} TaskRecurUnit;

/* Number of values, for the editor's combo and bounds checks.              */
#define TASK_RECUR_N_UNITS 6

/* The column defaults a task starts with — the time of day a recurrence
 * is anchored at, the time a due date means, the recurrence lead's ceiling
 * and a planner session's length — are SHARED_DB_TASK_*_DEFAULT
 * (shared_db.h), one definition for both apps and the schema.  The recur
 * time PHASE-LOCKS the minute and hour units ("starting at 09:00, every 3
 * hours" means 9, 12, 3); the lead is a CEILING, task_recur_lead_default
 * takes the lesser of it and half the repeat period.                       */

/* One task list.  Strings are owned by the struct.                         */
typedef struct {
    gint64    id;
    gchar    *name;
    gchar    *emoji;                 /* optional display prefix ("")        */
    gint64    updated_at;
    gint      position;
    gint64    group_id;              /* 0 = ungrouped                       */
    gboolean  deleted;
} TaskList;

/* One list group (local-only; never synced to Google).  Strings owned.     */
typedef struct {
    gint64  id;
    gchar  *name;
    gint    position;
} TaskGroup;

/* One task or subtask.  Strings are owned by the struct.
 *
 * There is nothing here belonging to a particular INTEGRATION.  A sync's
 * per-task state — a remote id, an etag, a deep link — lives in that
 * integration's own table keyed by task id, so a task
 * carries only what a task is.                                            */
typedef struct {
    gint64    id;
    gint64    list_id;
    gint64    parent_id;             /* 0 = top-level task                  */
    gchar    *title;
    gchar    *notes;
    gint64    due;                   /* unix local midnight; 0 = no date.
                                      * The DATE only — always midnight,
                                      * because Google's due is date-only
                                      * and every day-bucketing view
                                      * (the 3-Day Forecast) compares
                                      * against it                          */
    gint      due_time;              /* the time of day that date means,
                                      * minutes past local midnight,
                                      * default SHARED_DB_TASK_DUE_TIME_DEFAULT      */
    TaskStatus status;             /* New / In Progress / Done — DONE is
                                      * what every "is it complete?" test
                                      * asks for                            */
    gboolean  pinned;
    gboolean  priority;              /* high priority — local-only, like
                                      * pinned (Google has no priority);
                                      * sorts to the top of every view      */
    gint      position;
    gint64    updated_at;
    gboolean  deleted;
    gint64    completed_at;          /* unix; when the task was LAST
                                      * completed.  0 = never.  Nothing
                                      * clears it, so it outlives someone
                                      * reopening the task               */

    /* The recurrence schedule.  See recur.h for the rule these six spell
     * out together.                                                        */
    gint      recur_interval;        /* repeat every N units; 0 = never     */
    TaskRecurUnit recur_unit;        /* the unit of that N                  */
    gint      recur_time;            /* minutes past local midnight; the
                                      * time of day a dated occurrence
                                      * lands on (ignored by the minute
                                      * and hour units)                    */
    gint      recur_lead;            /* minutes BEFORE an occurrence that a
                                      * completed task is reset to New     */
    gint64    recur_next;            /* unix time of the next occurrence;
                                      * 0 = not computed yet (the pass
                                      * seeds it)                          */
    gint64    recur_start;           /* unix LOCAL MIDNIGHT of the day
                                      * the schedule is anchored on — the
                                      * "starting Monday" half of "every
                                      * Monday at 9:00 AM".  The time of
                                      * day is recur_time, never this.
                                      * 0 = unset, and the anchor falls
                                      * back to the due date and then to
                                      * today                            */

    /* Day planner schedule — LOCAL-ONLY (never synced).                     */
    gint64    scheduled_start;       /* unix seconds; 0 = not scheduled       */
    gint      scheduled_duration;    /* working session length in minutes      */
} Task;

/* One file attachment on a task.                                           */
typedef struct {
    gint64    id;
    gint64    task_id;
    gchar    *path;                  /* absolute file path (owned)          */
} TaskAttachment;

void task_list_free(TaskList *l);
void task_free(Task *t);

/* ---------------------------------------------------------------------------
 * task_db_open() — open a connection to the shared database at `path`
 * (shared_db_open: a missing file is created complete, in one step; any
 * file that is not a stamped shared database — an empty one, an old
 * tasks.db, a Notes notes.db — is refused untouched, as is one on a
 * network volume or in a cloud-synced folder).  Nothing in Tasks creates
 * a file at the path first.  The path to open is shared_db_path().  Returns
 * the handle, or NULL with `err` set to the module's own words.
 *
 * A path inside the GTK app's live data directory (task_paths.h) is
 * REFUSED, as it is by task_db_verify_file and task_db_file_sha256: this
 * app never reads or writes there.
 * ------------------------------------------------------------------------- */
TaskDatabase *task_db_open(const gchar *path, GError **err);

/* task_db_close() — close the connection and free the handle.  NULL-safe.  */
void task_db_close(TaskDatabase *db);

/* ---------------------------------------------------------------------------
 * task_db_outside_change() — has ANOTHER connection (Notes, a script, this
 * app's own worker threads) changed Tasks' data since the last look?
 * Asks shared_db_changed first (cheap), then, only when something was
 * committed, compares the 'tasks' change revision (shared_db_rev,
 * SHARED_DB_REV_TASKS) with the one remembered on `db`: a commit that
 * touched only Notes' tables or the shared settings is not a change to
 * Tasks' data.  This connection's own commits move the remembered
 * revision with them, so they never count.
 *
 * Inputs:
 *   db — the app's main connection
 *
 * Output:
 *   TRUE when Tasks' data changed elsewhere (the caller refreshes); FALSE
 *   otherwise.  A revision that cannot be read answers TRUE.
 * ------------------------------------------------------------------------- */
gboolean task_db_outside_change(TaskDatabase *db);

/* ---------------------------------------------------------------------------
 * task_db_verify_file() — is the database at `path` structurally sound?
 *
 * Runs the shared module's full check (shared_db_check: integrity_check,
 * foreign_key_check, stamps, WAL, cross-table invariants) on a SEPARATE
 * read-only connection, so it says nothing about whatever is open
 * elsewhere; a file that is not a stamped shared database is refused.  Returns TRUE only when both actually RAN and both passed;
 * `detail` (optional, g_free) receives sqlite's own words on failure.
 *
 * This exists because "sqlite3_open succeeded" proves nothing: SQLite
 * opens a malformed file happily and only errors when a damaged page is
 * READ.  Anything that copies the database must verify the copy this way
 * before it is trusted — never by opening it.
 * ------------------------------------------------------------------------- */
gboolean task_db_verify_file(const gchar *path, gchar **detail);

/* ---------------------------------------------------------------------------
 * task_db_health_check() — verify the OPEN database and remember the result.
 *
 * Runs the same check task_db_verify_file runs, against this
 * connection, and stores what it found on the connection so a later
 * reader (the Settings window) can say WHEN the answer was true.
 * Returns TRUE when the database is sound.
 *
 * It writes NOTHING to the database, and that is the point rather than an
 * optimisation.  The verdict used to be stamped into sync_state, which
 * made a health check MUTATE THE FILE IT WAS MEASURING: every run changed
 * the database's bytes, so the SHA-256 shown beside it moved on every
 * press of Update while the user's tasks sat untouched.  A fingerprint
 * that its own refresh invalidates is worse than no fingerprint.  Holding
 * the verdict in memory also makes "checked 14:55 today" mean a check
 * made in THIS session, where a stamp read off disk could describe a file
 * that has changed many times since.
 *
 * The cost, accepted: the verdict does not survive a restart, so a launch
 * with the startup check switched off shows "Not checked" until Update is
 * pressed.  Living on the connection is also what makes a database SWITCH
 * safe — the old connection's verdict cannot outlive the file it describes.
 * ------------------------------------------------------------------------- */
gboolean task_db_health_check(TaskDatabase *db);

/* task_db_health() — the stored result, or NULL when no pass has been made
 * on this connection.  Borrowed, owned by the connection: do not free it,
 * and do not keep it across a task_db_health_check.                      */
const TaskDbHealth *task_db_health(TaskDatabase *db);

/* ---------------------------------------------------------------------------
 * task_db_health_check_file() — the health pass made over the FILE at
 * `path` instead of an open handle, for a caller that must not hold the
 * main thread while it runs (the Settings window's Update).
 *
 * Runs on the separate read-only connection task_db_verify_file uses, so
 * it may be called from any thread and can never create or change the
 * file.  The verdict is RETURNED in *out — `ran` included, which
 * task_db_verify_file does not report — and stored nowhere:
 * task_db_health_record is what puts it on the open handle, back on the
 * main thread.  out->detail is the caller's (g_free), NULL when sound.
 * Returns out->ok.
 * ------------------------------------------------------------------------- */
gboolean task_db_health_check_file(const gchar *path, TaskDbHealth *out);

/* ---------------------------------------------------------------------------
 * task_db_health_record() — make `verdict` the stored result of `db`: what
 * task_db_health answers with from now on.  The one writer of the stored
 * verdict; task_db_health_check goes through it too.
 *
 * Takes verdict->detail over (it is NULL afterwards).  The caller is
 * responsible for the verdict being about the file `db` has open.
 * ------------------------------------------------------------------------- */
void task_db_health_record(TaskDatabase *db, TaskDbHealth *verdict);

/* ---------------------------------------------------------------------------
 * task_db_file_sha256() — hex SHA-256 of the file at `path`, or NULL when
 * it cannot be read (g_free the result).
 *
 * Answers about the file AS IT STANDS, and is never stored: a live
 * database changes with the next thing the user types, and writing the
 * digest into the database would change it again.  A caller showing one
 * is showing a fingerprint of this moment, which is exactly what makes it
 * comparable with `shasum -a 256` or with a backup.
 * ------------------------------------------------------------------------- */
gchar *task_db_file_sha256(const gchar *path);

/* --------------------------------- lists --------------------------------- */

/* Every write below that returns gboolean returns TRUE when its change was
 * committed and FALSE when it was not (logged): above all when another
 * writer held the file past the busy timeout (SHARED_DB_BUSY_TIMEOUT_MS).
 * A caller acting for the user must not let such a failure pass silently:
 * the editor keeps its edit and retries, the list and board toggles say so
 * and show the database's true state.                                     */

/* All lists — alphabetical (case-insensitive) by DEFAULT; once the user
 * drag-reorders the sidebar (sync_state "lists_custom_order" set by
 * task_db_lists_reorder) the stored positions rule, name-tiebroken.
 * include_deleted also returns tombstoned rows (sync).  Returns TaskList*
 * elements; free the array with g_ptr_array_free after task_list_free-ing
 * elements (or use task_ptr_array_free_lists).                             */
GPtrArray *task_db_lists(TaskDatabase *db, gboolean include_deleted);

/* Persist a sidebar drag-reorder: position = index of each id in `ids`
 * (one transaction) and switch task_db_lists to custom-order mode.  The
 * order is local-only — Google tasklists have no ordering — so rows are
 * NOT dirtied for sync.                                                    */
gboolean task_db_lists_reorder(TaskDatabase *db, const gint64 *ids, gsize n);

TaskList  *task_db_list_get(TaskDatabase *db, gint64 id);

/* Create a list.  `emoji` is the optional local-only display prefix
 * (NULL/"" for none) — it is never part of the synced name.                */
gint64   task_db_list_create(TaskDatabase *db, const gchar *name,
                             const gchar *emoji);

/* Update a list's name + emoji (stamps updated_at; a changed name syncs
 * to Google, the emoji never does).                                        */
gboolean task_db_list_update(TaskDatabase *db, gint64 id, const gchar *name,
                             const gchar *emoji);

/* Tombstone the list AND every task in it (they must disappear from the
 * remote side too).                                                        */
gboolean task_db_list_delete(TaskDatabase *db, gint64 id);

/* --------------------------------- tasks --------------------------------- */

Task    *task_db_task_get(TaskDatabase *db, gint64 id);

/* Visible top-level tasks of one list, ordered by position.                */
GPtrArray *task_db_tasks_toplevel(TaskDatabase *db, gint64 list_id);

/* Visible subtasks of one task, ordered by position.                       */
GPtrArray *task_db_subtasks(TaskDatabase *db, gint64 parent_id);

/* ALL visible subtasks (every list), ordered by parent then position —
 * one query for the task pane instead of one per top-level row.  Their
 * `notes` come back EMPTY: the pane and the search read a subtask's title
 * and status only, and the notes are the bulk of a task's bytes.          */
GPtrArray *task_db_subtasks_all_visible(TaskDatabase *db);

/* Visible pinned tasks across all lists (any level), pinned order = list
 * then position.                                                           */
GPtrArray *task_db_tasks_pinned(TaskDatabase *db);

/* TRUE when any non-tombstoned task is pinned.  Drives the sidebar's
 * Favorites row visibility.                                                */
gboolean task_db_has_pinned(TaskDatabase *db);

/* Visible top-level tasks across ALL lists (the "All Tasks" meta list),
 * ordered by list then position.                                           */
GPtrArray *task_db_tasks_all_visible(TaskDatabase *db);

/* Visible top-level tasks of every list in one GROUP, ordered by list then
 * position — the same shape as task_db_tasks_all_visible, narrowed to the
 * group's lists.  A tombstoned list contributes nothing.  An empty group
 * (or an id naming no group) yields the empty array, never NULL.           */
GPtrArray *task_db_tasks_in_group(TaskDatabase *db, gint64 group_id);

/* Visible tasks (any level) with lo <= due < hi, soonest first; open_only
 * leaves the Done ones out (the notification worker's question).           */
GPtrArray *task_db_tasks_due_between(TaskDatabase *db, gint64 lo, gint64 hi,
                                     gboolean open_only);

/* Every task row of ONE list, including subtasks and tombstones, parents
 * before subtasks.                                                         */
GPtrArray *task_db_tasks_in_list_all(TaskDatabase *db, gint64 list_id);

/* Create a task ('' title allowed).  parent_id = 0 for top-level; the
 * parent must itself be top-level (one nesting level — enforced here).
 * Returns the new id, or 0 on constraint failure.                          */
gint64 task_db_task_create(TaskDatabase *db, gint64 list_id, gint64 parent_id,
                           const gchar *title);

/* ---------------------------------------------------------------------------
 * task_db_task_update() — write the EDITABLE fields in which `after`
 * differs from `before`, and no others: one UPDATE naming only those
 * columns.  `before` is the row as the caller loaded it (the editor keeps
 * it from the moment it showed the task); a field the caller did not
 * change is never written, so a change another app or connection made to
 * it in the meantime survives.  Nothing differing writes nothing.
 *
 * The editable fields: title, notes, due, due_time, status, pinned,
 * priority and the recurrence schedule (recur_interval, recur_unit,
 * recur_time, recur_lead, recur_next, recur_start).  updated_at is stamped
 * only when a SYNCED field is among them (title, notes, due, due_time,
 * status), never for the local-only ones (pinned, priority, the
 * schedule).  completed_at is stamped when status ENTERS Done (the CASE
 * reads the stored row), and a parent moves from New to In Progress when
 * a subtask enters Done.
 *
 * Returns 1 when a change was written, 0 when nothing differed (nothing
 * is written), -1 when the write failed (logged).
 * ------------------------------------------------------------------------- */
gint task_db_task_update(TaskDatabase *db, const Task *before, const Task *after);

/* task_dup() — a deep copy of a task (its strings copied too).  Free with
 * task_free.                                                               */
Task *task_dup(const Task *t);

/* Field setters used by the list-view toggles.  `status` is the
 * successor of a SYNCED field, so EVERY change to it stamps updated_at
 * and dirties the row — including New ↔ In Progress, which neither
 * Google nor Notes can represent.  `pinned` and `priority` are
 * LOCAL-ONLY and never stamp (see the .c banners).                         */
gboolean task_db_task_set_status(TaskDatabase *db, gint64 id, TaskStatus status);
gboolean task_db_task_set_pinned(TaskDatabase *db, gint64 id, gboolean pinned);
gboolean task_db_task_set_priority(TaskDatabase *db, gint64 id, gboolean priority);

/* ---------------------------------------------------------------------------
 * The recurrence pass's two write paths (see recur.h for the rule; these
 * are only the SQL).  Both are called from the main thread.
 *
 * task_db_tasks_recurring() — every visible task whose recur_interval is
 * set, in id order.  The candidate set of one pass; a few thousand rows
 * scanned every few minutes needs no index of its own.
 *
 * task_db_task_recur_apply() — an occurrence has come due: write `due`
 * and the `due_time` it lands at, reset a DONE task to New, store the
 * FOLLOWING occurrence in recur_next, and stamp updated_at.  The status CASE reads the OLD row (gotcha 8), so
 * "was it Done?" is asked once.  completed_at is left ALONE — reopening a
 * task for its next repeat does not un-complete the last one.
 * updated_at IS stamped here —
 * due and status are synced fields, and a roll-forward the sync cannot see
 * is a roll-forward that never reaches Google or Notes.
 *
 * task_db_task_recur_set_next() — store recur_next ALONE, with NO
 * updated_at bump.  The next-occurrence stamp is local bookkeeping (the
 * same rule pinned and priority follow): seeding it, or advancing past an
 * occurrence that changed nothing, must not dirty the row for sync.
 * ------------------------------------------------------------------------- */
GPtrArray *task_db_tasks_recurring(TaskDatabase *db);
gboolean task_db_task_recur_apply(TaskDatabase *db, gint64 id, gint64 due,
                              gint due_time, gint64 next);
gboolean task_db_task_recur_set_next(TaskDatabase *db, gint64 id, gint64 next);

/* Set or clear a task's day-planner schedule.  Both fields are
 * LOCAL-ONLY and do NOT stamp updated_at.  Pass start_unix = 0 to
 * remove the schedule.                                                    */
gboolean task_db_task_set_scheduled(TaskDatabase *db, gint64 id,
                                gint64 start_unix, gint duration_min);

/* Tasks scheduled within a local day, [lo, hi) in unix seconds (the
 * bounds from task_day_bounds), ordered by scheduled_start.  Returns
 * Task* elements; free with task_ptr_array_free_tasks after use.
 * Never NULL.                                                             */
GPtrArray *task_db_tasks_scheduled_for_day(TaskDatabase *db, gint64 lo, gint64 hi);

/* Tombstone the task and its subtasks, in one transaction.                */
gboolean task_db_task_delete(TaskDatabase *db, gint64 id);

/* Tombstone every completed top-level task of a list and their subtasks,
 * in ONE transaction.  Returns how many top-level tasks went.              */
guint task_db_tasks_delete_done(TaskDatabase *db, gint64 list_id);

/* A transaction round several writes that belong together (a drop that
 * changes the status of five cards): one journalled write instead of
 * five.  begin returns FALSE when the transaction could not start (the
 * writes then run on their own; nothing is lost but the batching); end
 * COMMITs on ok and ROLLS BACK otherwise, and after a failed COMMIT, so
 * the connection is never left inside a transaction, and returns TRUE
 * only when the work was committed.                                      */
gboolean task_db_transaction_begin(TaskDatabase *db);
gboolean task_db_transaction_end(TaskDatabase *db, gboolean ok);

/* Swap the display position of subtask `id` with its neighbor in the
 * current sorted order (direction = -1 up, +1 down).  No-op at the
 * edges or for top-level tasks.  Position is LOCAL-ONLY for ordering —
 * updated_at is not stamped.                                               */
gboolean task_db_subtask_move(TaskDatabase *db, gint64 id, gint direction);

/* Move a top-level task (and its subtasks) to another list, appended at
 * the end; stamps updated_at.                                              */
gboolean task_db_task_move_list(TaskDatabase *db, gint64 id, gint64 dest_list);

/* ------------------------------ attachments ------------------------------ */

GPtrArray *task_db_attachments(TaskDatabase *db, gint64 task_id);
gint64     task_db_attachment_add(TaskDatabase *db, gint64 task_id,
                                  const gchar *path);
gboolean   task_db_attachment_remove(TaskDatabase *db, gint64 id);

/* task_id → attachment count for every task, as one query.  Keys/values
 * are packed into the pointers (GINT_TO_POINTER); free with
 * g_hash_table_destroy.                                                    */
GHashTable *task_db_attachment_counts(TaskDatabase *db);

/* -------------------------------- vitals --------------------------------- */

/* Live totals for the About dialog: non-tombstoned task and list counts.
 * Either out-pointer may be NULL; a failed query leaves 0.                 */
void task_db_totals(TaskDatabase *db, gint *n_tasks, gint *n_lists);

/* ---------------------------------------------------------------------------
 * task_db_list_purge() / task_db_task_purge() — physically remove a list
 * and its tasks, or a task and its subtasks, in one transaction.  REFUSED,
 * deleting nothing, when any task they would remove is a Notes action item
 * (a note_task row names it): Tasks never purges one of those — deleting
 * such a task is a tombstone (task_db_task_delete), which Notes answers by
 * unmarking the line.  Returns TRUE when the rows were removed.
 * ------------------------------------------------------------------------- */
gboolean task_db_list_purge(TaskDatabase *db, gint64 id);
gboolean task_db_task_purge(TaskDatabase *db, gint64 id);

/* ---------------------------------------------------------------------------
 * task_db_task_note() — the title of the note a task was written in, when
 * it is a Notes action item: a task is Notes-born iff a note_task row
 * references it.  Returns a new string (g_free; "" for an untitled note),
 * or NULL when the task was not born in Notes.
 * ------------------------------------------------------------------------- */
gchar *task_db_task_note(TaskDatabase *db, gint64 task_id);

/* Free helper for the GPtrArrays above.                                    */
void task_ptr_array_free_lists(GPtrArray *a);
void task_ptr_array_free_tasks(GPtrArray *a);
void task_ptr_array_free_attachments(GPtrArray *a);

/* --------------------------------- groups -------------------------------- */

void      task_group_free(TaskGroup *g);
void      task_ptr_array_free_groups(GPtrArray *a);

/* All groups, ordered by position then name.                               */
GPtrArray *task_db_groups(TaskDatabase *db);

/* One group row, or NULL when no group has that id.  Free with
 * task_group_free.                                                         */
TaskGroup *task_db_group_get(TaskDatabase *db, gint64 id);

/* Create a group.  Returns the new id, or 0 on failure.                    */
gint64    task_db_group_create(TaskDatabase *db, const gchar *name);

/* Delete a group: un-groups all its lists (sets group_id = NULL).          */
gboolean  task_db_group_delete(TaskDatabase *db, gint64 id);

/* Rename a group.                                                          */
gboolean  task_db_group_rename(TaskDatabase *db, gint64 id, const gchar *name);

/* Move a list into a group (group_id 0 = ungrouped → sets NULL).           */
gboolean  task_db_list_set_group(TaskDatabase *db, gint64 list_id,
                                 gint64 group_id);

#endif /* TASK_DB_H */
