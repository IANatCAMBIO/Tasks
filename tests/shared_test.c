/*
 * shared_test.c — Tasks on the shared database: the column-diff update
 * (task_db_task_update) that keeps two writers from losing each other's
 * changes, and its stamping rules; and Notes action items (a note_task row
 * names the task) — the Action Items view and when it is shown, the
 * editor's note lookup, the note context every read
 * carries (action_items_ctx), ordinary deletion, and the purge Tasks
 * refuses; which outside changes refresh the app (a change to
 * Tasks' data refreshes, one to Notes' data re-reads only the editors'
 * note lines, its own writes never count, and an outside commit landing
 * right after its own is not lost); tombstones with subtasks; and writes
 * that fail because another PROCESS holds the file past the busy timeout —
 * they wait once, report it, write nothing, and succeed once the file is
 * free.
 *
 * Two connections to one file stand for Tasks and Notes (or two threads):
 * each loads the row, changes a different field and saves; both changes
 * must survive.
 */

#include "fixture.h"
#include "gcal_store.h"
#include "task_pane.h"
#include "task_view.h"

#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/*
 * new_task — a fresh task in a fresh list.
 *
 * Inputs:
 *   app   — the fixture
 *   title — its title
 *
 * Output:
 *   the task's id.
 */
static gint64
new_task(TaskApp *app, const char *title)
{
    return task_db_task_create(app->db, task_db_list_create(app->db, "Shared", ""), 0, title);
}

/*
 * test_no_lost_update — two connections load the same row; one changes the
 * title, the other the priority (and, again, the status); both survive.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_no_lost_update(TaskApp *app)
{
    gint64        id    = new_task(app, "Original");
    TaskDatabase *other = task_db_open(shared_db_file(app->db->shared), NULL);
    Task         *mine  = task_db_task_get(app->db, id);
    Task         *theirs = task_db_task_get(other, id);

    Task edit_mine = *mine;
    edit_mine.title = "Retitled here";
    Task edit_theirs = *theirs;
    edit_theirs.priority = TRUE;

    check(task_db_task_update(app->db, mine, &edit_mine) == 1,
          "diff: a changed title is written");
    check(task_db_task_update(other, theirs, &edit_theirs) == 1,
          "diff: the other connection's changed priority is written");

    Task *both = task_db_task_get(app->db, id);
    check(both != NULL && strcmp(both->title, "Retitled here") == 0 && both->priority,
          "diff: both changes survive (no lost update)");
    task_free(both);

    /* The other way round: the stale side changes the status while the
     * row's title has moved on under it. */
    Task edit_status = *theirs;
    edit_status.status = TASK_STATUS_DONE;
    check(task_db_task_update(other, theirs, &edit_status) == 1,
          "diff: a status change from a stale row is written");
    both = task_db_task_get(app->db, id);
    check(both != NULL && strcmp(both->title, "Retitled here") == 0 && both->priority &&
          both->status == TASK_STATUS_DONE && both->completed_at > 0,
          "diff: ... without writing back the stale title or priority");
    task_free(both);

    check(task_db_task_update(app->db, mine, mine) == 0,
          "diff: a row with nothing changed writes nothing");

    task_free(mine);
    task_free(theirs);
    task_db_close(other);
}

/*
 * test_stamps — updated_at moves for a synced field only; completed_at is
 * stamped on entry to Done and left alone on the way out; a parent starts
 * when a subtask is done.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_stamps(TaskApp *app)
{
    gint64 id = new_task(app, "Stamped");
    Task  *t  = task_db_task_get(app->db, id);

    /* Back-date the row so a stamp is visible within the same second. */
    sqlite3_exec(shared_db_handle(app->db->shared),
                 "UPDATE tasks SET updated_at = 1000", NULL, NULL, NULL);
    t->updated_at = 1000;

    Task local = *t;
    local.pinned     = TRUE;
    local.recur_next = 12345;
    task_db_task_update(app->db, t, &local);
    Task *after = task_db_task_get(app->db, id);
    check(after->pinned && after->recur_next == 12345 && after->updated_at == 1000,
          "stamps: a local-only change does not stamp updated_at");
    task_free(after);

    Task synced = local;
    synced.notes = "a synced field";
    task_db_task_update(app->db, &local, &synced);
    after = task_db_task_get(app->db, id);
    check(after->updated_at > 1000 && strcmp(after->notes, "a synced field") == 0,
          "stamps: a synced change stamps updated_at");

    Task done = *after;
    done.status = TASK_STATUS_DONE;
    task_db_task_update(app->db, after, &done);
    Task *finished = task_db_task_get(app->db, id);
    check(finished->status == TASK_STATUS_DONE && finished->completed_at > 0,
          "stamps: entering Done stamps completed_at");

    sqlite3_exec(shared_db_handle(app->db->shared),
                 "UPDATE tasks SET completed_at = 777", NULL, NULL, NULL);
    finished->completed_at = 777;
    Task reopened = *finished;
    reopened.status = TASK_STATUS_IN_PROGRESS;
    task_db_task_update(app->db, finished, &reopened);
    Task *open = task_db_task_get(app->db, id);
    check(open->status == TASK_STATUS_IN_PROGRESS && open->completed_at == 777,
          "stamps: leaving Done keeps completed_at");

    gint64 sub      = task_db_task_create(app->db, open->list_id, id, "Subtask");
    Task  *child    = task_db_task_get(app->db, sub);
    Task   child_done = *child;
    child_done.status = TASK_STATUS_DONE;
    sqlite3_exec(shared_db_handle(app->db->shared),
                 "UPDATE tasks SET status = 0 WHERE parent_id IS NULL", NULL, NULL, NULL);
    task_db_task_update(app->db, child, &child_done);
    Task *parent = task_db_task_get(app->db, id);
    check(parent->status == TASK_STATUS_IN_PROGRESS,
          "stamps: a subtask entering Done starts its New parent");

    task_free(parent);
    task_free(child);
    task_free(open);
    task_free(finished);
    task_free(after);
    task_free(t);
}

/*
 * link_to_note — make a task a Notes action item, as Notes writes it: a
 * note, and a note_task row naming the task.
 *
 * Inputs:
 *   app     — the fixture
 *   task_id — the task
 *   title   — the note's title
 */
static void
link_to_note(TaskApp *app, gint64 task_id, const char *title)
{
    char *sql = sqlite3_mprintf(
        "INSERT INTO notes (title, created_at, updated_at) VALUES (%Q, 0, 0);"
        "INSERT INTO note_task (task_id, note_id, ord) VALUES (%lld, last_insert_rowid(), 0);"
        "UPDATE action_uid_seq SET next = (SELECT max(uid) + 1 FROM note_task);",
        title, (long long)task_id);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);
    sqlite3_free(sql);
}

/*
 * row_int — the first column of the first row of a query (a count, or one
 * value of one row).
 *
 * Inputs:
 *   app — the fixture
 *   sql — the query
 *
 * Output:
 *   the value, -1 on error or when there is no row.
 */
static gint64
row_int(TaskApp *app, const char *sql)
{
    sqlite3_stmt *st = NULL;
    gint64        n  = -1;

    if (sqlite3_prepare_v2(shared_db_handle(app->db->shared), sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

/*
 * context_of — the note context a fresh read of a task carries.
 *
 * Inputs:
 *   app     — the fixture
 *   task_id — the task
 *
 * Output:
 *   new string (g_free): Task.context, or NULL when the task is gone.
 */
static gchar *
context_of(TaskApp *app, gint64 task_id)
{
    Task  *t   = task_db_task_get(app->db, task_id);
    gchar *ctx = t != NULL ? g_strdup(t->context) : NULL;
    task_free(t);
    return ctx;
}

/*
 * test_note_context — every task read carries its note context: the
 * folder of the note it was written in (action_items_ctx.context), ""
 * for a note at the top level and for a task not born in Notes, and the
 * folder's new name once Notes renames it.  List loads carry it too.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_note_context(TaskApp *app)
{
    gint64 list   = task_db_list_create(app->db, "Context", "");
    gint64 filed  = task_db_task_create(app->db, list, 0, "lock down my diet RE keto");
    gint64 loose  = task_db_task_create(app->db, list, 0, "Top-level note item");
    gint64 plain  = task_db_task_create(app->db, list, 0, "Typed in Tasks");
    char  *sql    = sqlite3_mprintf(
        "INSERT INTO folders (name) VALUES ('Captain''s Log');"
        "INSERT INTO notes (folder_id, title, created_at, updated_at)"
        " VALUES (last_insert_rowid(), 'Log', 0, 0);"
        "INSERT INTO note_task (task_id, note_id, ord) VALUES (%lld, last_insert_rowid(), 0);"
        "UPDATE action_uid_seq SET next = (SELECT max(uid) + 1 FROM note_task);",
        (long long)filed);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    link_to_note(app, loose, "Loose note");

    gchar *ctx = context_of(app, filed);
    check(ctx != NULL && strcmp(ctx, "Captain's Log") == 0,
          "context: a note in a folder gives its items the folder's name");
    g_free(ctx);
    ctx = context_of(app, loose);
    check(ctx != NULL && *ctx == '\0', "context: a note at the top level gives none");
    g_free(ctx);
    ctx = context_of(app, plain);
    check(ctx != NULL && *ctx == '\0', "context: a task not born in Notes has none");
    g_free(ctx);

    GPtrArray *rows  = task_db_tasks_toplevel(app->db, list);
    guint      with  = 0;               /* rows carrying any context      */
    gboolean   right = FALSE;           /* the filed one carries its own  */
    for (guint i = 0; i < rows->len; i++) {
        const Task *t = g_ptr_array_index(rows, i);
        if (*t->context != '\0') with++;
        if (t->id == filed) right = strcmp(t->context, "Captain's Log") == 0;
    }
    check(rows->len == 3 && with == 1 && right,
          "context: a list load carries it on each row, and only on those");
    task_ptr_array_free_tasks(rows);

    Task  *t     = task_db_task_get(app->db, filed);
    gchar *shown = t != NULL ? task_display_title(t, TRUE) : NULL;
    check(shown != NULL && strcmp(shown, "Captain's Log - lock down my diet RE keto") == 0 &&
          strcmp(t->title, "lock down my diet RE keto") == 0,
          "context: shown before the title, the stored title untouched");
    g_free(shown);
    task_free(t);

    sqlite3_exec(shared_db_handle(app->db->shared),
                 "UPDATE folders SET name = 'Ship''s Log' WHERE name = 'Captain''s Log'",
                 NULL, NULL, NULL);
    ctx = context_of(app, filed);
    check(ctx != NULL && strcmp(ctx, "Ship's Log") == 0,
          "context: a folder renamed in Notes is reflected at the next read");
    g_free(ctx);
}

/*
 * view_has — whether a view's query returns a task.
 *
 * Inputs:
 *   app — the fixture
 *   v   — the view
 *   id  — the task
 *
 * Output:
 *   TRUE when the view's query returns it.
 */
static gboolean
view_has(TaskApp *app, const TaskView *v, gint64 id)
{
    GPtrArray *tasks = v->query(app, v->user_data);
    gboolean   found = FALSE;
    for (guint i = 0; i < tasks->len && !found; i++) {
        found = ((const Task *)g_ptr_array_index(tasks, i))->id == id;
    }
    task_ptr_array_free_tasks(tasks);
    return found;
}

/*
 * pane_count — how many rows the task pane shows for a view (the
 * show_completed setting applied).
 *
 * Inputs:
 *   app — the fixture
 *   v   — the view
 *
 * Output:
 *   the row count.
 */
static size_t
pane_count(TaskApp *app, const TaskView *v)
{
    task_sel_t           sel     = { task_sel_view, task_view_index_of(v) };
    task_pane_content_t *content = task_pane_collect(app, sel, NULL);
    size_t               n       = content->rows->count;
    task_pane_content_free(content);
    return n;
}

/*
 * test_action_items_view — the Action Items view: registered right after
 * the 3-Day Forecast; hidden while no live Notes-born task exists, shown
 * once one does, hidden again once it is deleted; it gathers every live
 * Notes-born task from every list (a subtask too) and nothing else; a Done
 * one is returned and left to the show_completed setting, as in every
 * view.  Runs on a database without action items.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_action_items_view(TaskApp *app)
{
    const TaskView *v        = task_view_find("action_items");
    gint            forecast = task_view_index_of(task_view_find("forecast"));

    check(v != NULL && task_view_index_of(v) == forecast + 1 &&
          strcmp(v->name, SHARED_DB_ACTION_LIST_NAME) == 0 &&
          strcmp(v->label, SHARED_DB_ACTION_LIST_EMOJI "  " SHARED_DB_ACTION_LIST_NAME) == 0,
          "action items: the view follows the 3-Day Forecast, named like Notes' list");
    if (v == NULL) return;

    gint64 work   = task_db_list_create(app->db, "Work items", "");
    gint64 home   = task_db_list_create(app->db, "Home items", "");
    gint64 plain  = task_db_task_create(app->db, work, 0, "Typed, not noted");
    gint64 in_w   = task_db_task_create(app->db, work, 0, "From a meeting note");
    gint64 in_h   = task_db_task_create(app->db, home, 0, "From a home note");
    gint64 sub    = task_db_task_create(app->db, work, plain, "A noted subtask");
    gint64 done   = task_db_task_create(app->db, home, 0, "A finished note item");
    gint64 gone   = task_db_task_create(app->db, work, 0, "A deleted note item");

    check(!task_view_shown(v, app) && pane_count(app, v) == 0,
          "action items: hidden while there are none");

    link_to_note(app, in_w, "Meeting");
    check(task_view_shown(v, app), "action items: shown once there is one");
    task_db_task_delete(app->db, in_w);
    check(!task_view_shown(v, app), "action items: hidden again once it is deleted");
    char sql[128];
    snprintf(sql, sizeof sql, "UPDATE tasks SET deleted = 0 WHERE id = %lld", (long long)in_w);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);
    check(task_view_shown(v, app), "action items: shown again once it is restored");

    link_to_note(app, in_h, "Home");
    link_to_note(app, sub, "Subtask note");
    link_to_note(app, done, "Done note");
    link_to_note(app, gone, "Deleted note");
    task_db_task_delete(app->db, gone);
    snprintf(sql, sizeof sql, "UPDATE tasks SET status = %d WHERE id = %lld",
             SHARED_DB_TASK_STATUS_DONE, (long long)done);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);

    check(view_has(app, v, in_w) && view_has(app, v, in_h),
          "action items: Notes-born tasks of two lists are both listed");
    check(view_has(app, v, sub), "action items: a Notes-born subtask is listed");
    check(!view_has(app, v, plain), "action items: a task typed in Tasks is not");
    check(!view_has(app, v, gone), "action items: a deleted one is not");
    check(view_has(app, v, done), "action items: a Done one is returned to the pane");

    gboolean was = task_config_get_bool(TASK_CFG_SHOW_COMPLETED);
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, TRUE);
    size_t with_done = pane_count(app, v);
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, FALSE);
    size_t open_only = pane_count(app, v);
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, was);
    check(with_done == 4 && open_only == 3,
          "action items: the pane shows the Done one only with show_completed");

    /* Leave none behind for the tests after this one. */
    const gint64 born[] = { in_w, in_h, sub, done };
    for (gsize i = 0; i < G_N_ELEMENTS(born); i++) task_db_task_delete(app->db, born[i]);
    check(!task_view_shown(v, app), "action items: hidden once all are deleted");
}

/*
 * test_note_items — a Notes-born task: named by its note; never purged
 * (nor its parent, nor its list); deleted as an ordinary tombstone with
 * the link kept, so Notes can unmark the line.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_note_items(TaskApp *app)
{
    gint64 list  = task_db_list_create(app->db, "Action Items", "");
    gint64 born  = task_db_task_create(app->db, list, 0, "From a note");
    gint64 plain = task_db_task_create(app->db, list, 0, "Typed in Tasks");
    gint64 sub   = task_db_task_create(app->db, list, plain, "A subtask");
    char   sql[256];

    link_to_note(app, born, "Weekly sync");
    gchar *note = task_db_task_note(app->db, born);
    check(note != NULL && strcmp(note, "Weekly sync") == 0,
          "notes: a task a note_task row names reports its note's title");
    g_free(note);
    check(task_db_task_note(app->db, plain) == NULL,
          "notes: any other task reports none");

    check(!task_db_task_purge(app->db, born) && task_db_task_get(app->db, born) != NULL,
          "notes: a Notes-born task is never purged");
    check(!task_db_list_purge(app->db, list) && task_db_list_get(app->db, list) != NULL &&
          row_int(app, "SELECT count(*) FROM tasks WHERE title = 'Typed in Tasks'") == 1,
          "notes: nor is its list, and nothing in it is removed");

    /* A note-linked SUBTASK protects its parent from a purge as well. */
    link_to_note(app, sub, "Another note");
    check(!task_db_task_purge(app->db, plain) && task_db_task_get(app->db, sub) != NULL,
          "notes: a parent whose subtask is Notes-born is not purged");
    sqlite3_exec(shared_db_handle(app->db->shared), "DELETE FROM note_task WHERE task_id IN "
                 "(SELECT id FROM tasks WHERE title = 'A subtask')", NULL, NULL, NULL);

    /* Deleting it in Tasks is the usual tombstone; the link stays for
     * Notes to see (seen_deleted = 0: someone else deleted it). */
    task_db_task_delete(app->db, born);
    Task *gone = task_db_task_get(app->db, born);
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM note_task WHERE task_id = %lld AND seen_deleted = 0",
             (long long)born);
    check(gone != NULL && gone->deleted && gone->updated_at > 0 && row_int(app, sql) == 1,
          "notes: deleting it tombstones the task and keeps the link for Notes");
    task_free(gone);

    /* A plain task purges as before. */
    gint64 other = task_db_list_create(app->db, "Plain", "");
    gint64 temp  = task_db_task_create(app->db, other, 0, "Throwaway");
    check(task_db_task_purge(app->db, temp) && task_db_task_get(app->db, temp) == NULL,
          "notes: a task not born in Notes is still purged");
    check(task_db_list_purge(app->db, other) && task_db_list_get(app->db, other) == NULL,
          "notes: as is a list holding none");

    shared_db_findings found;
    shared_db_check(app->db->shared, 1, &found, NULL);
    check(found.count == 0, "notes: the file passes the shared module's check afterwards");
    for (size_t i = 0; i < found.count; i++) printf("     %s\n", found.items[i].message);
    shared_db_findings_free(&found);
}

/*
 * other_exec — run SQL on a second connection to the fixture's file, as
 * another app would.
 *
 * Inputs:
 *   app — the fixture
 *   sql — the statements
 */
static void
other_exec(TaskApp *app, const char *sql)
{
    TaskDatabase *other = task_db_open(shared_db_file(app->db->shared), NULL);
    sqlite3_exec(shared_db_handle(other->shared), sql, NULL, NULL, NULL);
    task_db_close(other);
}

/*
 * task_column — one integer column of a task's row, deleted or not.
 *
 * Inputs:
 *   app    — the fixture
 *   id     — the task
 *   column — the column name ("deleted", "updated_at", "status")
 *
 * Output:
 *   the value, -1 when the row is missing.
 */
static gint64
task_column(TaskApp *app, gint64 id, const char *column)
{
    char sql[160];
    snprintf(sql, sizeof sql, "SELECT %s FROM tasks WHERE id = %lld", column, (long long)id);
    return row_int(app, sql);
}

/*
 * test_tombstones — deleting a task, a list's completed tasks or a list
 * tombstones each task with its live subtasks; a subtask already deleted
 * keeps its own updated_at (SHARED_DB_SQL_TOMBSTONE_WHERE), so an undo can
 * tell it apart.  A subtask entering Done moves its New parent to In
 * Progress.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_tombstones(TaskApp *app)
{
    gint64 list = task_db_list_create(app->db, "Tombs", "");
    gint64 done = task_db_task_create(app->db, list, 0, "Done parent");
    gint64 kid  = task_db_task_create(app->db, list, done, "Live child");
    gint64 old  = task_db_task_create(app->db, list, done, "Deleted child");
    gint64 open = task_db_task_create(app->db, list, 0, "Open parent");
    gint64 sub  = task_db_task_create(app->db, list, open, "Open child");
    char   sql[160];

    snprintf(sql, sizeof sql, "UPDATE tasks SET deleted = 1, updated_at = 1 WHERE id = %lld",
             (long long)old);
    other_exec(app, sql);
    check(task_db_task_set_status(app->db, kid, TASK_STATUS_DONE) &&
          task_column(app, done, "status") == TASK_STATUS_IN_PROGRESS,
          "tombstone: (a subtask entering Done starts its New parent)");
    task_db_task_set_status(app->db, done, TASK_STATUS_DONE);

    check(task_db_tasks_delete_done(app->db, list) == 1,
          "tombstone: deleting done tasks counts the top-level ones");
    check(task_column(app, done, "deleted") == 1 &&
          task_column(app, kid, "deleted") == 1,
          "tombstone: ... and takes their live subtasks");
    check(task_column(app, old, "updated_at") == 1, "tombstone: ... an already-deleted one keeps its stamp");
    check(task_column(app, open, "deleted") == 0 &&
          task_column(app, sub, "deleted") == 0,
          "tombstone: ... and leaves open tasks alone");

    check(task_db_task_delete(app->db, open) &&
          task_column(app, open, "deleted") == 1 &&
          task_column(app, sub, "deleted") == 1,
          "tombstone: deleting a task takes its subtasks");

    gint64 left = task_db_task_create(app->db, list, 0, "Left");
    gint64 dead = task_db_task_create(app->db, list, 0, "Dead");
    snprintf(sql, sizeof sql, "UPDATE tasks SET deleted = 1, updated_at = 1 WHERE id = %lld",
             (long long)dead);
    other_exec(app, sql);
    snprintf(sql, sizeof sql, "SELECT deleted FROM lists WHERE id = %lld", (long long)list);
    check(task_db_list_delete(app->db, list) && row_int(app, sql) == 1 &&
          task_column(app, left, "deleted") == 1 && task_column(app, dead, "updated_at") == 1,
          "tombstone: deleting a list takes its live tasks; a deleted one keeps its stamp");
}

/* The commit the wal hook makes once, on another connection, the moment
 * the app's own commit has landed (before own_commit polls).               */
static char race_sql[256];

/*
 * commit_right_after — sqlite3_wal_hook callback: runs after a commit of
 * the app's connection, its write lock already released, and lands another
 * connection's commit there once (race_sql), then uninstalls itself.
 *
 * Inputs:
 *   app  — the fixture (as the hook's context)
 *   h, name, pages — unused
 *
 * Output:
 *   SQLITE_OK.
 */
static int
commit_right_after(void *app, sqlite3 *h, const char *name, int pages)
{
    (void)name;
    (void)pages;
    sqlite3_wal_hook(h, NULL, NULL);   /* (the fixture's file never needs
                                          its automatic checkpoint)      */
    other_exec(app, race_sql);
    return SQLITE_OK;
}

/*
 * count_notes_event — a notes-event listener: counts its calls.
 *
 * Inputs:
 *   app  — unused
 *   data — an int counter
 */
static void
count_notes_event(TaskApp *app, gpointer data)
{
    (void)app;
    (*(int *)data)++;
}

/*
 * test_refresh_rev — the app refreshes only what changed elsewhere: a
 * commit to Notes' tables fires the notes event alone, its own writes
 * never count, and an outside change that lands just before or just after
 * its own write is not hidden by that write.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_refresh_rev(TaskApp *app)
{
    gint64 id    = new_task(app, "Watched");
    int    notes = 0;
    guint  sub   = task_app_listen_notes(app, count_notes_event, &notes);
    char   sql[256];

    task_app_check_outside(app);                       /* start from quiet */
    check(task_app_check_outside(app) == 0 && notes == 0,
          "refresh: nothing changed, nothing refreshes");

    other_exec(app, "INSERT INTO notes (title, created_at, updated_at) VALUES ('N', 0, 0)");
    check(task_app_check_outside(app) == TASK_DB_CHANGED_NOTES && notes == 1,
          "refresh: a change to Notes' data alone fires only the notes event");

    snprintf(sql, sizeof sql, "UPDATE tasks SET title = 'Elsewhere' WHERE id = %lld",
             (long long)id);
    other_exec(app, sql);
    check(task_app_check_outside(app) == TASK_DB_CHANGED_TASKS && notes == 1,
          "refresh: a change to Tasks' data refreshes");
    check(task_app_check_outside(app) == 0, "refresh: ... once");

    task_db_task_set_pinned(app->db, id, TRUE);
    check(task_app_check_outside(app) == 0, "refresh: the app's own write does not refresh");
    task_db_task_set_priority(app->db, id, TRUE);
    other_exec(app, "UPDATE notes SET title = 'N2' WHERE title = 'N'");
    check(task_app_check_outside(app) == TASK_DB_CHANGED_NOTES,
          "refresh: ... nor does a later Notes-only change after it");

    other_exec(app, sql);                              /* outside, then own */
    task_db_task_set_pinned(app->db, id, FALSE);
    check(task_app_check_outside(app) & TASK_DB_CHANGED_TASKS,
          "refresh: an outside change made just before the app's own write still refreshes");

    task_db_task_set_pinned(app->db, id, TRUE);        /* own, then outside */
    other_exec(app, "UPDATE tasks SET title = 'After' WHERE title = 'Elsewhere'");
    check(task_app_check_outside(app) == TASK_DB_CHANGED_TASKS,
          "refresh: an outside change made just after the app's own write refreshes");

    /* The race: another commit lands between the app's COMMIT and the
     * poll that follows it, so that poll sees both moves at once.         */
    snprintf(race_sql, sizeof race_sql,
             "UPDATE tasks SET title = 'Raced' WHERE id = %lld", (long long)id);
    sqlite3_wal_hook(shared_db_handle(app->db->shared), commit_right_after, app);
    task_db_task_set_pinned(app->db, id, FALSE);
    Task *raced = task_db_task_get(app->db, id);
    check(raced != NULL && strcmp(raced->title, "Raced") == 0,
          "refresh: (another commit landed right after the app's own)");
    task_free(raced);
    check(task_app_check_outside(app) == TASK_DB_CHANGED_TASKS,
          "refresh: ... and is not lost: the next poll refreshes");
    check(task_app_check_outside(app) == 0, "refresh: ... once");

    task_app_unlisten(app, sub);
}

/* How long the holder process keeps the write lock, in seconds: longer
 * than the four writes that each wait out the busy timeout.                */
#define HOLD_SECONDS 23

/*
 * hold_lock — the holder process (this binary run with --hold): take the
 * file's write lock with BEGIN IMMEDIATE, say so on stdout, keep it for
 * `seconds`, then let go.
 *
 * Inputs:
 *   path    — the database file
 *   seconds — how long to hold it
 *
 * Output:
 *   the process's exit status: 0, or 2 when the lock was not taken.
 */
static int
hold_lock(const char *path, int seconds)
{
    shared_db *db = shared_db_open(path, NULL);
    if (db == NULL || shared_db_begin(db, NULL) != 0) return 2;
    fputs("held\n", stdout);
    fflush(stdout);
    sleep((unsigned)seconds);
    shared_db_rollback(db);
    shared_db_close(db);
    return 0;
}

/*
 * test_busy — another process holds the write lock past the busy timeout:
 * each write reports failure and changes nothing (the editor's column
 * update, a list toggle's setter, a subtask's status change with its
 * parent's, the calendar store's transaction); once
 * the lock is let go the same writes succeed.
 *
 * Inputs:
 *   app  — the fixture
 *   self — this binary's path, run again as the holder
 */
static void
test_busy(TaskApp *app, const char *self)
{
    gint64 id     = new_task(app, "Busy");
    Task  *parent = task_db_task_get(app->db, id);
    gint64 child  = task_db_task_create(app->db, parent->list_id, id, "Busy child");
    int    out[2];
    task_free(parent);
    pid_t  pid  = 0;
    char   secs[8];

    snprintf(secs, sizeof secs, "%d", HOLD_SECONDS);
    char *argv[] = { (char *)self, "--hold", (char *)shared_db_file(app->db->shared), secs, NULL };
    posix_spawn_file_actions_t actions;
    if (pipe(out) != 0) abort();
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    int spawned = posix_spawn(&pid, self, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    char said[8] = "";
    check(spawned == 0 && read(out[0], said, 4) == 4 && strncmp(said, "held", 4) == 0,
          "busy: (another process holds the write lock)");
    close(out[0]);

    time_t   started = time(NULL);
    gboolean pinned  = task_db_task_set_pinned(app->db, id, TRUE);
    check(!pinned && time(NULL) - started >= SHARED_DB_BUSY_TIMEOUT_MS / 1000 - 1,
          "busy: a toggle's write fails after the busy timeout, and says so");

    started = time(NULL);
    gboolean done = task_db_task_set_status(app->db, child, TASK_STATUS_DONE);
    time_t   took = time(NULL) - started;
    check(!done && took >= SHARED_DB_BUSY_TIMEOUT_MS / 1000 - 1 &&
          took < 2 * (SHARED_DB_BUSY_TIMEOUT_MS / 1000),
          "busy: a subtask's status change fails after ONE busy wait, and says so");

    Task *before = task_db_task_get(app->db, id);
    Task  after  = *before;
    after.title  = "Typed while busy";
    check(task_db_task_update(app->db, before, &after) < 0,
          "busy: the editor's update fails, and says so");

    GPtrArray      *cals = g_ptr_array_new();
    gcal_calendar_t cal  = { "c@x", "Cal", "", TRUE, TRUE };
    g_ptr_array_add(cals, &cal);
    check(!gcal_store_set_calendars(app->db, cals),
          "busy: the calendar store fails its pass when BEGIN fails");

    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "busy: (the lock is let go)");

    Task *stored = task_db_task_get(app->db, id);
    Task *kid = task_db_task_get(app->db, child);
    check(stored != NULL && !stored->pinned && strcmp(stored->title, "Busy") == 0 &&
          stored->status == TASK_STATUS_NEW && kid != NULL && kid->status == TASK_STATUS_NEW,
          "busy: nothing was written meanwhile (neither the subtask nor its parent)");
    task_free(stored);
    task_free(kid);
    GPtrArray *none = gcal_store_calendars(app->db);
    check(none->len == 0, "busy: ... not even part of the calendar list");
    g_ptr_array_free(none, TRUE);

    check(task_db_task_set_pinned(app->db, id, TRUE) &&
          task_db_task_update(app->db, before, &after) == 1 &&
          gcal_store_set_calendars(app->db, cals),
          "busy: the same writes succeed once the file is free");
    stored = task_db_task_get(app->db, id);
    check(stored != NULL && stored->pinned && strcmp(stored->title, "Typed while busy") == 0,
          "busy: ... and are stored");
    task_free(stored);
    task_free(before);
    g_ptr_array_free(cals, TRUE);
}

/*
 * main — run the shared-database tests, or, run with --hold, be the
 * process that holds the write lock for test_busy.
 *
 * Inputs:
 *   argc, argv — none normally; "--hold <path> <seconds>" for the holder
 *
 * Output:
 *   0 when every check passed.
 */
int
main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--hold") == 0)
        return hold_lock(argv[2], atoi(argv[3]));

    TaskApp *app = fixture_open();

    test_no_lost_update(app);
    test_stamps(app);
    test_action_items_view(app);
    test_note_items(app);
    test_note_context(app);
    test_refresh_rev(app);
    test_tombstones(app);
    test_busy(app, argv[0]);

    fixture_close(app);
    return check_result();
}
