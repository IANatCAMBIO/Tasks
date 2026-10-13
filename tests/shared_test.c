/*
 * shared_test.c — Tasks on the shared database: the column-diff update
 * (task_db_task_update) that keeps two writers from losing each other's
 * changes, and its stamping rules; and Notes action items (a note_task row
 * names the task) — the editor's note lookup, ordinary deletion, and the
 * purge Tasks refuses.
 *
 * Two connections to one file stand for Tasks and Notes (or two threads):
 * each loads the row, changes a different field and saves; both changes
 * must survive.
 */

#include "fixture.h"

#include <string.h>
#include <unistd.h>

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
 * count_rows — how many rows a query counts.
 *
 * Inputs:
 *   app — the fixture
 *   sql — a SELECT count(*)
 *
 * Output:
 *   the count, -1 on error.
 */
static int
count_rows(TaskApp *app, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int           n  = -1;

    if (sqlite3_prepare_v2(shared_db_handle(app->db->shared), sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
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
          count_rows(app, "SELECT count(*) FROM tasks WHERE title = 'Typed in Tasks'") == 1,
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
    check(gone != NULL && gone->deleted && gone->updated_at > 0 && count_rows(app, sql) == 1,
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
 * main — run the shared-database tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();

    test_no_lost_update(app);
    test_stamps(app);
    test_note_items(app);

    fixture_close(app);
    return check_result();
}
