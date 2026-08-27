/* ===========================================================================
 * db.h — SQLite storage for Tasks
 *
 * Schema (PRAGMA user_version = 7 — see TASK_DB_SCHEMA_VERSION).  Every
 * column is declared in task_db_open's CREATE block; there are no
 * ALTER-based migrations, so a fresh file and an existing one have
 * identical structure.
 *
 *   list_groups  id, name, position              (local-only; never synced)
 *   lists        id, name, emoji, position, group_id (FK → list_groups.id;
 *                NULL = ungrouped), updated_at, deleted
 *   tasks        id, list_id, parent_id (NULL = top-level; ONE level of
 *                nesting only — a subtask can never be a parent),
 *                title, notes, due (unix local midnight; 0 = none),
 *                status (TaskStatus), pinned, priority (local-only;
 *                sorts first in every view), position, updated_at,
 *                deleted, completed_at
 *   attachments  id, task_id, path, added_at   (local-only; never synced)
 *   sync_state   key, value                    (e.g. "last_sync")
 *
 * NOTHING belonging to a particular INTEGRATION is here.  A sync's
 * per-row state — a remote id, an etag, the baseline a done-only source
 * was last known to hold — lives in a SIDE TABLE keyed by row id, owned
 * and created by whichever plugin the integration is (v8 moved the
 * Google columns out, v9 the Notes ones).  The plugin registers a delete
 * hook when its own bookkeeping has to ride inside task_db_task_delete's
 * transaction — see task_db_add_delete_hook.
 *
 * Deletion is a SOFT flag everywhere (`deleted` = tombstone): the Google
 * Tasks sync needs to see "this existed and was deleted locally" to
 * propagate the delete, after which the row is purged for real.  Every
 * mutation stamps `updated_at` (unix seconds) — rows whose stamp is newer
 * than sync_state.last_sync are the local dirty set.
 * =========================================================================== */

#ifndef TASK_DB_H
#define TASK_DB_H

#include <glib.h>
#include <sqlite3.h>

/* ---------------------------------------------------------------------------
 * The app's on-disk names.  They live here, in the lowest header everyone
 * already includes, because three subsystems need the same two strings:
 * the database (this file), the ini (app.c) and the OAuth client-file
 * lookup (oauth.c).  TASK_APP_DIR is the per-user directory name under the
 * data and config dirs.
 * ------------------------------------------------------------------------- */
#define TASK_DB_FILENAME "tasks.db"
#define TASK_APP_DIR     "tasks"

/* The schema version this build writes.  Kept here rather than spelled as
 * a literal in task_db_open so the pre-migration backup and the version
 * stamp cannot drift apart.                                               */
#define TASK_DB_SCHEMA_VERSION 9

/* ---------------------------------------------------------------------------
 * TaskDatabase — one open connection.  A connection must not cross threads:
 * the sync worker opens its own on the same path (task_db_open).
 * ------------------------------------------------------------------------- */
typedef struct {
    sqlite3 *sq;
    gchar   *path;                   /* absolute file path (owned)          */
} TaskDatabase;

/* ---------------------------------------------------------------------------
 * TaskStatus — a task's progress, and the ONLY completion state stored.
 * The values are the on-disk encoding of tasks.status, so they must not
 * be renumbered; New is 0 so a freshly INSERTed row needs no explicit
 * value.
 *
 * DONE is exactly the old boolean `done`: it strikes the title through,
 * hides the row under the completed-visibility toggle, stamps
 * completed_at, and pushes "completed" to Google Tasks / Notes.  NEW and
 * IN_PROGRESS are both "not done" and are indistinguishable to everything
 * outside Tasks — neither Google Tasks nor Notes has a third state.
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_STATUS_NEW         = 0,
    TASK_STATUS_IN_PROGRESS = 1,
    TASK_STATUS_DONE        = 2
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
 * "done or not": the task list's checkbox column, the context menu, the
 * Google Tasks sync and the Notes mirror.
 *
 *   done = TRUE   → TASK_STATUS_DONE.
 *   done = FALSE  → TASK_STATUS_IN_PROGRESS when `cur` was DONE (unticking
 *                   an item means work resumed on it, not that it was
 *                   never started), otherwise `cur` UNCHANGED — so a New
 *                   task stays New, and a round trip through a
 *                   done-only system cannot silently promote it.
 * ------------------------------------------------------------------------- */
TaskStatus task_status_apply_done(TaskStatus cur, gboolean done);

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
 * integration's own table keyed by task id (schema v8), so a task
 * carries only what a task is.                                            */
typedef struct {
    gint64    id;
    gint64    list_id;
    gint64    parent_id;             /* 0 = top-level task                  */
    gchar    *title;
    gchar    *notes;
    gint64    due;                   /* unix local midnight; 0 = no date    */
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
    gint64    completed_at;          /* unix; 0 = never / not done         */
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
 * task_db_open() — open (creating/migrating as needed) the database at
 * `path`.  Returns the handle, or NULL with `err` set.
 * ------------------------------------------------------------------------- */
TaskDatabase *task_db_open(const gchar *path, GError **err);

/* task_db_close() — close the connection and free the handle.  NULL-safe.  */
void task_db_close(TaskDatabase *db);

/* task_db_default_path() — "<user data dir>/tasks/tasks.db" (the names are
 * TASK_APP_DIR / TASK_DB_FILENAME), creating the directory.  Returns a new
 * string (g_free it).                                                      */
gchar *task_db_default_path(void);

/* ---------------------------------------------------------------------------
 * task_db_verify_file() — is the database at `path` structurally sound?
 *
 * Runs PRAGMA integrity_check (and foreign_key_check) on a SEPARATE
 * read-only connection, so it says nothing about whatever is open
 * elsewhere.  Returns TRUE only when both actually RAN and both passed;
 * `detail` (optional, g_free) receives sqlite's own words on failure.
 *
 * This exists because "sqlite3_open succeeded" proves nothing: SQLite
 * opens a malformed file happily and only errors when a damaged page is
 * READ.  Anything that copies the database must verify the copy this way
 * before it is trusted — never by opening it.
 * ------------------------------------------------------------------------- */
gboolean task_db_verify_file(const gchar *path, gchar **detail);

/* ---------------------------------------------------------------------------
 * task_db_copy_file() — a CONSISTENT copy of the open database at `dest`,
 * made with VACUUM INTO rather than a byte copy: it runs inside a read
 * transaction, so it cannot capture a torn page, and it cannot be
 * confused by a sync daemon rewriting the source mid-read.
 *
 * `dest` must not already exist (VACUUM INTO refuses to overwrite) — the
 * caller removes it first if that is what it means to do.  Returns TRUE
 * on success; on failure *err (optional, g_free) gets sqlite's message.
 * ------------------------------------------------------------------------- */
gboolean task_db_copy_file(TaskDatabase *db, const gchar *dest, gchar **err);

/* ---------------------------------------------------------------------------
 * task_db_resolve_path() — the database file to open.
 *
 *   dir — the configured db_dir, or NULL/"" for the default location.
 *
 * Returns a new string (g_free): the path task_db_open should be handed.
 * The single answer to "which file", so no caller has to remember that
 * the default location is not simply a directory join.
 * ------------------------------------------------------------------------- */
gchar *task_db_resolve_path(const gchar *dir);

/* --------------------------------- lists --------------------------------- */

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
void task_db_lists_reorder(TaskDatabase *db, const gint64 *ids, gsize n);

TaskList  *task_db_list_get(TaskDatabase *db, gint64 id);

/* Seed a list's emoji — ONLY while it is still empty, so a later edit by
 * the user is never overwritten.  Does NOT stamp updated_at: an emoji is
 * local-only, and stamping would make every launch look like a change.
 *
 * Keyed on the LIST ID.  It used to take a Google tasklist id and find
 * the list by it, which put one integration's addressing scheme into a
 * core function; the caller resolves its own id to a list now.           */
void task_db_list_emoji_if_empty(TaskDatabase *db, gint64 list_id,
                                 const gchar *emoji);

/* Create a list.  `emoji` is the optional local-only display prefix
 * (NULL/"" for none) — it is never part of the synced name.                */
gint64   task_db_list_create(TaskDatabase *db, const gchar *name,
                             const gchar *emoji);

/* Update a list's name + emoji (stamps updated_at; a changed name syncs
 * to Google, the emoji never does).                                        */
void     task_db_list_update(TaskDatabase *db, gint64 id, const gchar *name,
                             const gchar *emoji);

/* Tombstone the list AND every task in it (they must disappear from the
 * remote side too).                                                        */
void     task_db_list_delete(TaskDatabase *db, gint64 id);

/* Undo a list tombstone: restore the list and its still-tombstoned
 * tasks (used when Google refuses the deletion — its default tasklist
 * cannot be deleted; remote remains the source of truth).                  */
void     task_db_list_restore(TaskDatabase *db, gint64 id);

/* --------------------------------- tasks --------------------------------- */

Task    *task_db_task_get(TaskDatabase *db, gint64 id);

/* Visible top-level tasks of one list, ordered by position.                */
GPtrArray *task_db_tasks_toplevel(TaskDatabase *db, gint64 list_id);

/* Visible subtasks of one task, ordered by position.                       */
GPtrArray *task_db_subtasks(TaskDatabase *db, gint64 parent_id);

/* ALL visible subtasks (every list), ordered by parent then position —
 * one query for the task pane instead of one per top-level row.            */
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

/* Visible tasks (any level) with lo <= due < hi, soonest first.            */
GPtrArray *task_db_tasks_due_between(TaskDatabase *db, gint64 lo, gint64 hi);

/* Every task row of ONE list, including subtasks and tombstones, parents
 * before subtasks (sync — a new parent must own a gtasks_id before its
 * children push).                                                          */
GPtrArray *task_db_tasks_in_list_all(TaskDatabase *db, gint64 list_id);

/* Create a task ('' title allowed).  parent_id = 0 for top-level; the
 * parent must itself be top-level (one nesting level — enforced here).
 * Returns the new id, or 0 on constraint failure.                          */
gint64 task_db_task_create(TaskDatabase *db, gint64 list_id, gint64 parent_id,
                           const gchar *title);

/* Write the editable fields (title/notes/due/status/pinned/priority) from
 * `t` back to its row and stamp updated_at.                                */
void task_db_task_update(TaskDatabase *db, const Task *t);

/* Field setters used by the list-view toggles.  `status` is the
 * successor of a SYNCED field, so EVERY change to it stamps updated_at
 * and dirties the row — including New ↔ In Progress, which neither
 * Google nor Notes can represent.  `pinned` and `priority` are
 * LOCAL-ONLY and never stamp (see the .c banners).                         */
void task_db_task_set_status(TaskDatabase *db, gint64 id, TaskStatus status);
void task_db_task_set_pinned(TaskDatabase *db, gint64 id, gboolean pinned);
void task_db_task_set_priority(TaskDatabase *db, gint64 id, gboolean priority);

/* Tombstone the task and its subtasks.  Every registered delete hook
 * contributes its own statements to the SAME transaction — see
 * task_db_add_delete_hook().                                               */
void task_db_task_delete(TaskDatabase *db, gint64 id);

/* ---------------------------------------------------------------------------
 * Delete hooks — how a feature that keeps its own per-task row reacts to
 * a task being tombstoned, without db.c knowing that feature exists.
 *
 * A hook APPENDS complete, semicolon-terminated SQL to `sql`; those
 * statements run inside task_db_task_delete()'s transaction, BEFORE the
 * tombstone UPDATEs and while the row is still untouched.  Splicing SQL
 * rather than calling back out is what keeps the whole delete atomic: a
 * hook that ran as its own transaction could commit while the tombstone
 * rolled back, or vice versa.
 *
 * The registry is process-wide, not per-connection, because a worker
 * thread deletes through its OWN connection (see TaskDatabase above) and
 * must get the same treatment.  Registration is not undoable and is
 * expected once, at startup, before any thread exists.
 *
 *   db        — the connection the delete is running on, for context;
 *               a hook must NOT execute on it.
 *   task_id   — the task being tombstoned.
 *   sql       — append here; never read or truncate what is already in it.
 * ------------------------------------------------------------------------- */
typedef void (*TaskDbDeleteSqlFn)(TaskDatabase *db, gint64 task_id,
                                  GString *sql, gpointer user_data);
void task_db_add_delete_hook(TaskDbDeleteSqlFn fn, gpointer user_data);

/* Swap the display position of subtask `id` with its neighbor in the
 * current sorted order (direction = -1 up, +1 down).  No-op at the
 * edges or for top-level tasks.  Position is LOCAL-ONLY for ordering —
 * updated_at is not stamped.                                               */
void task_db_subtask_move(TaskDatabase *db, gint64 id, gint direction);

/* Move a top-level task (and its subtasks) to another list, appended at
 * the end; stamps updated_at.                                              */
void task_db_task_move_list(TaskDatabase *db, gint64 id, gint64 dest_list);

/* Physically remove every DONE task of a list (and their subtasks) —
 * the local half of "Clear Completed" (the remote half is tasks.clear,
 * which hides them on Google's side, so no tombstones are needed).         */
void task_db_purge_done(TaskDatabase *db, gint64 list_id);

/* Insert a bare tombstone row in `list_id` and return its id, or 0.
 *
 * The caller is an integration recording "something that used to exist
 * here has gone" — it attaches its own remote identity to the returned
 * id in its own table.  The tombstone itself carries none, which is why
 * this takes no identity argument.                                        */
gint64 task_db_insert_remote_tombstone(TaskDatabase *db, gint64 list_id);

/* ------------------------------ attachments ------------------------------ */

GPtrArray *task_db_attachments(TaskDatabase *db, gint64 task_id);
gint64     task_db_attachment_add(TaskDatabase *db, gint64 task_id,
                                  const gchar *path);
void       task_db_attachment_remove(TaskDatabase *db, gint64 id);

/* task_id → attachment count for every task, as one query.  Keys/values
 * are packed into the pointers (GINT_TO_POINTER); free with
 * g_hash_table_destroy.                                                    */
GHashTable *task_db_attachment_counts(TaskDatabase *db);

/* -------------------------------- vitals --------------------------------- */

/* Live totals for the About dialog: non-tombstoned task and list counts.
 * Either out-pointer may be NULL; a failed query leaves 0.                 */
void task_db_totals(TaskDatabase *db, gint *n_tasks, gint *n_lists);

/* ---------------------------------------------------------------------------
 * task_db_task_apply_done_source() — apply what a DONE-ONLY source
 * reports about a task: its title, its due date, and whether it is done.
 *
 * Stamps updated_at, so the change propagates on to anything else
 * watching the row.  The status transition is the app's own rule, spelled
 * as a CASE over the row's CURRENT status so it needs no read-back:
 * done → Done; not-done → In Progress only if it WAS Done (unticking
 * means work resumed), otherwise unchanged, so a New task survives a
 * round trip through a system that has no third state.
 *
 * `completed_at` is stamped on ENTERING Done and cleared on leaving; an
 * already-Done task keeps its first stamp.
 *
 * The source's own bookkeeping — what it last knew, for diffing — is
 * NOT here.  That belongs to the integration, in its own table.
 * ------------------------------------------------------------------------- */
void task_db_task_apply_done_source(TaskDatabase *db, gint64 id,
                                    const gchar *title, gboolean done,
                                    gint64 due);

/* ---------------------------------------------------------------------------
 * Generic query helpers.
 *
 * Not tied to any one feature: they exist because an integration keeping
 * its own side table needs to read and write it, and the alternative was
 * a public db.c function per integration — which is the coupling the
 * side tables were introduced to remove.  The plugin API exposes exactly
 * these (see plugin.h), so in-tree and out-of-tree callers use one
 * implementation.
 *
 * `task_db_exec_sql` runs statements with no result; FALSE on failure,
 * with sqlite's own message logged.
 *
 * `task_db_scalar` returns a one-value SELECT, or -1 when the statement
 * could not run AT ALL — a caller checking a count must be able to tell
 * "zero problems" from "the check never ran".
 *
 * `task_db_exec_query` is sqlite3_exec's callback shape without the
 * sqlite3 types.  Return non-zero from `cb` to stop early; that is the
 * documented way and is NOT reported as failure.
 * ------------------------------------------------------------------------- */
gboolean task_db_exec_sql(TaskDatabase *db, const gchar *sql);
gint64   task_db_scalar(TaskDatabase *db, const gchar *sql);
gboolean task_db_exec_query(TaskDatabase *db, const gchar *sql,
                            gint (*cb)(gpointer user_data, gint n_cols,
                                       gchar **values, gchar **names),
                            gpointer user_data);

/* ------------------------------- sync state ------------------------------ */

/* Get/set one sync_state row.  Getter returns a new string or NULL.        */
gchar *task_db_state_get(TaskDatabase *db, const gchar *key);
void   task_db_state_set(TaskDatabase *db, const gchar *key, const gchar *value);


/* Overwrite a row from remote data WITHOUT the usual now() stamp — the
 * caller passes the remote updated time so the row is clean afterwards.    */
void task_db_list_apply_remote(TaskDatabase *db, gint64 id, const gchar *name,
                               gint64 updated_at);
void task_db_task_apply_remote(TaskDatabase *db, const Task *t);

/* Physically remove tombstoned/remotely-deleted rows.                      */
void task_db_list_purge(TaskDatabase *db, gint64 id);   /* + its tasks      */
void task_db_task_purge(TaskDatabase *db, gint64 id);   /* + its subtasks   */

/* Free helper for the GPtrArrays above.                                    */
void task_ptr_array_free_lists(GPtrArray *a);
void task_ptr_array_free_tasks(GPtrArray *a);
void task_ptr_array_free_attachments(GPtrArray *a);

/* --------------------------------- groups -------------------------------- */

void      task_group_free(TaskGroup *g);
void      task_ptr_array_free_groups(GPtrArray *a);

/* All groups, ordered by position then name.                               */
GPtrArray *task_db_groups(TaskDatabase *db);

/* Create a group.  Returns the new id, or 0 on failure.                    */
gint64    task_db_group_create(TaskDatabase *db, const gchar *name);

/* Delete a group: un-groups all its lists (sets group_id = NULL).          */
void      task_db_group_delete(TaskDatabase *db, gint64 id);

/* Rename a group.                                                          */
void      task_db_group_rename(TaskDatabase *db, gint64 id, const gchar *name);

/* Move a list into a group (group_id 0 = ungrouped → sets NULL).           */
void      task_db_list_set_group(TaskDatabase *db, gint64 list_id,
                                 gint64 group_id);

#endif /* TASK_DB_H */
