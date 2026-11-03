/*
 * cleanup_test.c — the completed-task cleanup (src/cleanup.h and the write
 * under it, task_db_tasks_purge_done in src/db.h): the retention policy and
 * its clamp, the calendar cutoff, what a pass takes and — the point of the
 * whole exercise — what it must NOT take: work still being done, a task
 * between occurrences of a repeat, and the text of a note whose action
 * item is being deleted.
 *
 * Every task here is backdated with SQL, because the thing under test is
 * how OLD a completed task is and no API sets that.
 */

#include "cal_store.h"
#include "cleanup.h"
#include "db.h"
#include "fixture.h"
#include "recur.h"
#include "task_config.h"

#include <string.h>
#include <time.h>

#define DAY (24 * 60 * 60)

/*
 * done_at — mark a task completed, as of `when`: the status, the
 * completion stamp the pass ages it by, and updated_at, which the pass
 * falls back to when nothing stamped the completion.
 *
 * Inputs:
 *   app  — the fixture
 *   id   — the task
 *   when — unix seconds; the completion stamp, or 0 to leave it unstamped
 *          (an old row, or another writer's)
 *   seen — unix seconds for updated_at
 */
static void
done_at(TaskApp *app, gint64 id, gint64 when, gint64 seen)
{
    char *sql = sqlite3_mprintf(
        "UPDATE tasks SET status = 2, completed_at = %lld, updated_at = %lld "
        " WHERE id = %lld",
        (long long)when, (long long)seen, (long long)id);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);
    sqlite3_free(sql);
}

/*
 * alive — does a task's row still exist?
 *
 * Inputs:
 *   app — the fixture
 *   id  — the task
 *
 * Output:
 *   TRUE when the row is there, tombstoned or not.
 */
static gboolean
alive(TaskApp *app, gint64 id)
{
    char sql[128];

    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM tasks WHERE id = %lld", (long long)id);
    return fixture_row_int(app, sql) == 1;
}

/*
 * tombstoned — is a task's row there but tombstoned (deleted = 1)?
 *
 * Inputs:
 *   app — the fixture
 *   id  — the task
 *
 * Output:
 *   TRUE when the row is a tombstone.
 */
static gboolean
tombstoned(TaskApp *app, gint64 id)
{
    char sql[128];

    snprintf(sql, sizeof sql, "SELECT deleted FROM tasks WHERE id = %lld", (long long)id);
    return fixture_row_int(app, sql) == 1;
}

/*
 * test_policy — the retention as stored and as read back: the number is
 * clamped to TASK_CLEANUP_AFTER_MIN..MAX, and a unit outside the three the
 * Settings menu offers (a hand-edited ini) reads as months rather than as
 * whatever the number happens to mean in minutes.
 */
static void
test_policy(void)
{
    gint          after = 0;
    TaskRecurUnit unit  = TASK_RECUR_MINUTE;

    task_cleanup_policy(&after, &unit);
    check(after == 6 && unit == TASK_RECUR_MONTH,
          "policy: six months out of the box");

    task_cleanup_policy_set(0, TASK_RECUR_DAY);
    task_cleanup_policy(&after, &unit);
    check(after == TASK_CLEANUP_AFTER_MIN && unit == TASK_RECUR_DAY,
          "policy: zero is clamped to the floor — a retention of none is not a policy");

    task_cleanup_policy_set(TASK_CLEANUP_AFTER_MAX + 50, TASK_RECUR_YEAR);
    task_cleanup_policy(&after, &unit);
    check(after == TASK_CLEANUP_AFTER_MAX && unit == TASK_RECUR_YEAR,
          "policy: a number past the ceiling is clamped to it");

    task_cleanup_policy_set(30, TASK_RECUR_MINUTE);
    task_cleanup_policy(&after, &unit);
    check(after == 30 && unit == TASK_RECUR_MONTH,
          "policy: a unit the menu does not offer reads as months");

    task_config_set_int(TASK_CFG_CLEANUP_UNIT, 99);
    task_cleanup_policy(NULL, &unit);
    check(unit == TASK_RECUR_MONTH, "policy: a nonsense unit in the ini reads as months");

    const TaskRecurUnit *units = task_cleanup_units();
    check(units[0] == TASK_RECUR_DAY && units[1] == TASK_RECUR_MONTH &&
          units[2] == TASK_RECUR_YEAR,
          "policy: the menu offers days, months and years, in that order");
}

/*
 * test_cutoff — the cutoff is CALENDAR arithmetic: a month is a month
 * rather than 30 days, which is what makes "6 months" mean what it says in
 * February, and a year carries a leap day where there is one.
 *
 * SLACK of an hour and a bit in each bound, deliberately: a calendar step
 * keeps the WALL CLOCK across a daylight-saving boundary, so a step over
 * one is an hour more or less than the matching number of 24-hour days.
 * That is the behaviour wanted (a cutoff at the same time of day as now),
 * so the test allows for it rather than pinning the seconds.
 */
static void
test_cutoff(void)
{
    const gint64 slack = 2 * 60 * 60;

    task_cleanup_policy_set(90, TASK_RECUR_DAY);
    gint64 now  = (gint64)time(NULL);
    gint64 back = now - task_cleanup_cutoff();
    check(llabs((long long)back - 90LL * DAY) <= slack,
          "cutoff: 90 days back is 90 days, give or take a clock change");

    task_cleanup_policy_set(1, TASK_RECUR_YEAR);
    back = now - task_cleanup_cutoff();
    check(back >= 365 * (gint64)DAY - slack && back <= 366 * (gint64)DAY + slack,
          "cutoff: a year back is a calendar year, leap day included");

    task_cleanup_policy_set(1, TASK_RECUR_MONTH);
    back = now - task_cleanup_cutoff();
    check(back >= 28 * (gint64)DAY - slack && back <= 31 * (gint64)DAY + slack,
          "cutoff: a month back is a calendar month, not thirty days");
}

/*
 * test_pass — one pass over a database holding one of everything: an old
 * completed family goes with its subtasks; recent work, open work and a
 * completed task whose repeat is still running stay; a completed SUBTASK
 * of a task still being worked on stays, because families go whole or not
 * at all.  The count the Settings panel shows is the count that goes.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_pass(TaskApp *app)
{
    gint64 now    = (gint64)time(NULL);
    gint64 old    = now - 200 * DAY;
    gint64 recent = now - 3 * DAY;

    gint64 stale     = task_db_task_create(app->db, 0, "Long finished", "Work");
    gint64 stale_sub = task_db_task_create(app->db, stale, "Its subtask", NULL);
    gint64 fresh     = task_db_task_create(app->db, 0, "Just finished", "Work");
    gint64 open      = task_db_task_create(app->db, 0, "Still going", "Work");
    gint64 open_sub  = task_db_task_create(app->db, open, "Finished bit of it", NULL);
    gint64 repeat    = task_db_task_create(app->db, 0, "Quarterly report", "Work");
    gint64 unstamped = task_db_task_create(app->db, 0, "Done by another writer", "Work");

    done_at(app, stale, old, old);
    done_at(app, stale_sub, old, old);
    done_at(app, fresh, recent, recent);
    done_at(app, open_sub, old, old);
    done_at(app, repeat, old, old);
    done_at(app, unstamped, 0, old);   /* no completion stamp: aged by updated_at */
    /* The repeat is written straight to the row: the editor's path would
     * seed and roll the schedule, and what matters here is only that the
     * task HAS one while it sits completed. */
    char *sql = sqlite3_mprintf("UPDATE tasks SET recur_interval = 3, recur_unit = %d"
                                " WHERE id = %lld",
                                (int)TASK_RECUR_MONTH, (long long)repeat);
    sqlite3_exec(shared_db_handle(app->db->shared), sql, NULL, NULL, NULL);
    sqlite3_free(sql);

    task_cleanup_policy_set(6, TASK_RECUR_MONTH);
    check(task_cleanup_pending(app) == 2,
          "pass: two families are old enough — the stale one and the unstamped one");

    guint left = 0;
    guint went = task_cleanup_run(app, &left);
    check(went == 2 && left == 0, "pass: both went, neither held back for Notes");

    check(!alive(app, stale) && !alive(app, stale_sub),
          "pass: the old family is gone from the file, subtask and all");
    check(!alive(app, unstamped),
          "pass: a completed task with no completion stamp is aged by its last change");
    check(alive(app, fresh) && !tombstoned(app, fresh),
          "pass: work finished this week stays");
    check(alive(app, open) && alive(app, open_sub),
          "pass: a completed subtask of an open task stays — families go whole");
    check(alive(app, repeat) && !tombstoned(app, repeat),
          "pass: a repeating task is between occurrences, not finished, and stays");
    check(task_cleanup_pending(app) == 0, "pass: nothing is left for a second pass");

    check(task_db_tag_find(app->db, "Work") != NULL,
          "pass: a tag other tasks still carry stays");
}

/*
 * test_tag_goes — the rows a pass removes take their tag with them when
 * they were its last carriers (the schema's prune trigger, reached through
 * the cascade), so cleaning up does not leave the sidebar full of tags
 * with nothing under them.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_tag_goes(TaskApp *app)
{
    gint64 now  = (gint64)time(NULL);
    gint64 only = task_db_task_create(app->db, 0, "The only one tagged this", "Archived");

    done_at(app, only, now - 400 * DAY, now - 400 * DAY);
    check(task_db_tag_find(app->db, "Archived") != NULL, "tag: it is there to begin with");

    task_cleanup_policy_set(1, TASK_RECUR_YEAR);
    check(task_cleanup_run(app, NULL) == 1, "tag: its task went");
    check(task_db_tag_find(app->db, "Archived") == NULL,
          "tag: a tag whose last carrier went goes with it");
}

/*
 * test_side_tables — what hangs off a task when its row goes: the
 * attachment rows go with it (the schema's cascade; the FILES are links
 * and are not touched by anything here), and the calendar binding
 * deliberately does NOT — it outlives the row so that the sync's next pass
 * can still delete the event the planner session made (cal_store.h).
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_side_tables(TaskApp *app)
{
    gint64 now  = (gint64)time(NULL);
    gint64 id   = task_db_task_create(app->db, 0, "Had a file and a session", NULL);
    char   sql[160];

    check(task_db_attachment_add(app->db, id, "/tmp/ui-check-not-read.pdf") > 0,
          "side tables: the attachment is recorded");
    cal_binding_t bind = { id, "primary@x", "ev-cleanup", now, 60, "Had a file" };
    cal_store_bind(app->db, &bind);
    done_at(app, id, now - 400 * DAY, now - 400 * DAY);

    task_cleanup_policy_set(1, TASK_RECUR_YEAR);
    check(task_cleanup_run(app, NULL) == 1 && !alive(app, id), "side tables: the task went");

    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM attachments WHERE task_id = %lld",
             (long long)id);
    check(fixture_row_int(app, sql) == 0, "side tables: its attachment rows went with it");
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM cal_task WHERE task_id = %lld",
             (long long)id);
    check(fixture_row_int(app, sql) == 1,
          "side tables: the calendar binding stays, so the event can still be deleted");
    cal_store_unbind(app->db, id);   /* what the sync's pass then does      */
}

/*
 * test_action_item — THE SPECIAL CASE.  A task a Notes '!' line created is
 * not Tasks' row to remove: the pass TOMBSTONES it and leaves the link, so
 * that Notes unmarks the line and keeps the text the user typed.  The note
 * row is not touched at all.  Once Notes has dropped the link (what its
 * pull does), the next pass takes the row itself.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_action_item(TaskApp *app)
{
    gint64 now   = (gint64)time(NULL);
    gint64 born  = task_db_task_create(app->db, 0, "Chase the invoice", NULL);
    gint64 plain = task_db_task_create(app->db, 0, "Typed in Tasks", NULL);
    gint64 host  = task_db_task_create(app->db, 0, "Has a noted subtask", NULL);
    gint64 noted = task_db_task_create(app->db, host, "Subtask from a note", NULL);

    fixture_link_to_note(app, born, "Finance");
    fixture_link_to_note(app, noted, "Finance");
    done_at(app, born, now - 400 * DAY, now - 400 * DAY);
    done_at(app, plain, now - 400 * DAY, now - 400 * DAY);
    done_at(app, host, now - 400 * DAY, now - 400 * DAY);
    done_at(app, noted, now - 400 * DAY, now - 400 * DAY);

    task_cleanup_policy_set(1, TASK_RECUR_YEAR);
    guint left = 0;
    guint went = task_cleanup_run(app, &left);
    check(went == 1 && left == 2,
          "action items: the typed task went; both noted families were held back");

    check(!alive(app, plain), "action items: a task of Tasks' own is removed outright");
    check(tombstoned(app, born),
          "action items: a noted task is tombstoned, so Notes can unmark its line");
    check(tombstoned(app, host) && tombstoned(app, noted),
          "action items: a family whose SUBTASK came from a note is tombstoned whole");
    check(fixture_row_int(app, "SELECT COUNT(*) FROM note_task") == 2,
          "action items: the links stay — they are what tells Notes to unmark the lines");
    check(fixture_row_int(app, "SELECT COUNT(*) FROM notes WHERE title = 'Finance'") == 2,
          "action items: the notes themselves are not touched");
    check(fixture_row_int(app,
              "SELECT COUNT(*) FROM note_task nt JOIN tasks t ON t.id = nt.task_id"
              " WHERE t.deleted = 1 AND nt.seen_deleted = 0") == 2,
          "action items: the links read as \"deleted by someone else\", which is the pull");

    /* What Notes' pull then does: it unmarks the line — keeping the text,
     * with a check mark where the '!' was for a task that was done — and
     * drops the link.  The next pass is free to take the rows.  The mark
     * is Notes' business and is checked there
     * (notes/tests/action_sync_test.c, "deleted done"). */
    sqlite3_exec(shared_db_handle(app->db->shared), "DELETE FROM note_task", NULL, NULL, NULL);
    check(task_cleanup_run(app, &left) == 2 && left == 0,
          "action items: once Notes has let go, the next pass removes the rows");
    check(!alive(app, born) && !alive(app, host) && !alive(app, noted),
          "action items: nothing of them is left in the file");

    shared_db_findings found;
    shared_db_check(app->db->shared, 1, &found, NULL);
    check(found.count == 0, "action items: the file passes the shared module's check after");
    for (size_t i = 0; i < found.count; i++) printf("     %s\n", found.items[i].message);
    shared_db_findings_free(&found);
}

/*
 * test_switch — the switch (TASK_CFG_CLEANUP_ENABLED) governs the
 * SCHEDULED passes, not this entry point: a pass asked for by hand (Clean
 * Up Now) runs with it off, which is what makes a one-off tidy-up possible
 * without turning automatic deletion on.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_switch(TaskApp *app)
{
    gint64 now = (gint64)time(NULL);
    gint64 id  = task_db_task_create(app->db, 0, "Ancient", NULL);

    done_at(app, id, now - 400 * DAY, now - 400 * DAY);
    check(!task_config_get_bool(TASK_CFG_CLEANUP_ENABLED),
          "switch: off until the user turns it on");

    task_cleanup_policy_set(1, TASK_RECUR_YEAR);
    check(task_cleanup_run(app, NULL) == 1 && !alive(app, id),
          "switch: a pass asked for by hand runs with the switch off");
}

/*
 * main — run the cleanup tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();

    test_policy();
    test_cutoff();
    test_pass(app);
    test_tag_goes(app);
    test_side_tables(app);
    test_action_item(app);
    test_switch(app);

    fixture_close(app);
    return check_result();
}
