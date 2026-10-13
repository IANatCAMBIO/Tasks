/*
 * shared_test.c — Tasks on the shared database: the column-diff update
 * (task_db_task_update) that keeps two writers from losing each other's
 * changes, and its stamping rules.
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

    fixture_close(app);
    return check_result();
}
