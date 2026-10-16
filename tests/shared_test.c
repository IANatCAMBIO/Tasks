/*
 * shared_test.c — Tasks on the shared database: the column-diff update
 * (task_db_task_update) that keeps two writers from losing each other's
 * changes, and its stamping rules; and Notes action items (a note_task row
 * names the task) — the "Action Items" tag the schema gives them, the
 * editor's note lookup, the tags Notes shares, ordinary deletion, and the
 * purge Tasks
 * refuses; which outside changes refresh the app (a change to
 * Tasks' data refreshes, one to Notes' data re-reads only the editors'
 * note lines, its own writes never count, and an outside commit landing
 * right after its own is not lost); tombstones with subtasks, and the last
 * tag a live task keeps; and writes
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
 * new_task — a fresh task, tagged "Shared".
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
    return task_db_task_create(app->db, 0, title, "Shared");
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
    local.priority   = TRUE;
    local.recur_next = 12345;
    task_db_task_update(app->db, t, &local);
    Task *after = task_db_task_get(app->db, id);
    check(after->priority && after->recur_next == 12345 && after->updated_at == 1000,
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

    gint64 sub      = task_db_task_create(app->db, id, "Subtask", NULL);
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
 * tag_listed — is a tag of this name among task_db_tags(scope)?
 *
 * Inputs:
 *   app   — the fixture
 *   scope — TASK_TAGS_CARRIED or TASK_TAGS_ALL
 *   name  — the tag's name (exact spelling)
 *
 * Output:
 *   TRUE when listed.
 */
static gboolean
tag_listed(TaskApp *app, TaskTagScope scope, const char *name)
{
    GPtrArray *tags  = task_db_tags(app->db, scope);
    gboolean   found = FALSE;
    for (guint i = 0; i < tags->len && !found; i++)
        found = strcmp(((const TaskTag *)g_ptr_array_index(tags, i))->name, name) == 0;
    task_ptr_array_free_tags(tags);
    return found;
}

/*
 * test_shared_tags — the tags are Notes' too: a tag only a note carries is
 * offered (TASK_TAGS_ALL) but not shown (TASK_TAGS_CARRIED); tagging a task
 * with it, in any case, uses the note's spelling; the tag outlives the
 * task's letting go while the note carries it; Tasks' favorite flag is its
 * own.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_shared_tags(TaskApp *app)
{
    sqlite3_exec(shared_db_handle(app->db->shared),
                 "INSERT INTO notes (title, created_at, updated_at) VALUES ('Plan', 0, 0);"
                 "INSERT INTO tags (name, favorite_notes) VALUES ('Stride', 1);"
                 "INSERT INTO note_tags (note_id, tag_id, in_field)"
                 " SELECT (SELECT MAX(id) FROM notes), id, 1 FROM tags WHERE name = 'Stride';",
                 NULL, NULL, NULL);
    check(tag_listed(app, TASK_TAGS_ALL, "Stride") &&
          !tag_listed(app, TASK_TAGS_CARRIED, "Stride"),
          "shared tags: a note's tag is offered, not shown");

    gint64   id  = task_db_task_create(app->db, 0, "rerun the reports", "Shared");
    TaskTag *tag = task_db_tag_find(app->db, "STRIDE");
    check(id > 0 && tag != NULL && task_db_task_tag_add(app->db, id, "stride") == tag->id &&
          tag_listed(app, TASK_TAGS_CARRIED, "Stride"),
          "shared tags: a task takes the note's tag, its spelling kept, and shows it");
    check(task_db_tag_set_favorite(app->db, tag->id, TRUE), "shared tags: favorite in Tasks");
    task_tag_free(tag);
    tag = task_db_tag_find(app->db, "Stride");
    check(tag != NULL && tag->favorite, "shared tags: Tasks' favorite flag reads back");
    sqlite3_stmt *st = NULL;
    gint          notes_flag = -1;
    if (sqlite3_prepare_v2(shared_db_handle(app->db->shared),
                           "SELECT favorite_notes FROM tags WHERE name = 'Stride'", -1, &st,
                           NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        notes_flag = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    check(notes_flag == 1, "shared tags: Notes' favorite flag is its own");

    check(tag != NULL && task_db_task_tag_remove(app->db, id, tag->id) == 1 &&
          !tag_listed(app, TASK_TAGS_CARRIED, "Stride") &&
          tag_listed(app, TASK_TAGS_ALL, "Stride"),
          "shared tags: the task lets go; the note still carries it");
    task_tag_free(tag);
}

/*
 * tag_names_of — a task's tags as one string, "a,b".
 *
 * Inputs:
 *   app — the fixture
 *   id  — the task
 *
 * Output:
 *   new string (g_free).
 */
static gchar *
tag_names_of(TaskApp *app, gint64 id)
{
    GPtrArray *tags  = task_db_task_tags(app->db, id);
    GString   *names = g_string_new("");
    for (guint i = 0; i < tags->len; i++) {
        if (i > 0) g_string_append_c(names, ',');
        g_string_append(names, ((const TaskTag *)g_ptr_array_index(tags, i))->name);
    }
    task_ptr_array_free_tags(tags);
    return g_string_free(names, FALSE);
}

/*
 * test_action_tag — a task Notes writes carries the "Action Items" tag
 * (the schema's trigger), with Notes' emoji, beside the tag it had; it is
 * an ordinary tag: its view lists the Notes-born tasks and nothing else,
 * and it goes when the last of them is deleted.  Runs on a database
 * without action items.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_action_tag(TaskApp *app)
{
    gint64 plain = task_db_task_create(app->db, 0, "Typed, not noted", "Work items");
    gint64 in_w  = task_db_task_create(app->db, 0, "From a meeting note", "Work items");
    gint64 in_h  = task_db_task_create(app->db, 0, "From a home note", "Home items");

    check(task_db_tag_find(app->db, SHARED_DB_ACTION_LIST_NAME) == NULL,
          "action tag: none while there are no Notes items");
    link_to_note(app, in_w, "Meeting");
    link_to_note(app, in_h, "Home");

    TaskTag *action = task_db_tag_find(app->db, SHARED_DB_ACTION_LIST_NAME);
    check(action != NULL && strcmp(action->emoji, SHARED_DB_ACTION_LIST_EMOJI) == 0,
          "action tag: Notes' items carry it, with Notes' emoji");
    gchar *names = tag_names_of(app, in_w);
    check(strcmp(names, SHARED_DB_ACTION_LIST_NAME ",Work items") == 0,
          "action tag: beside the tag the task already had");
    g_free(names);
    if (action == NULL) return;

    GPtrArray *tagged = task_db_tasks_tagged(app->db, action->id);
    gboolean   only   = tagged->len == 2;
    for (guint i = 0; i < tagged->len; i++) {
        gint64 id = ((const Task *)g_ptr_array_index(tagged, i))->id;
        only = only && (id == in_w || id == in_h);
    }
    check(only, "action tag: its view lists the Notes-born tasks and nothing else");
    task_ptr_array_free_tasks(tagged);

    task_db_task_delete(app->db, in_w);
    task_db_task_delete(app->db, in_h);
    check(task_db_tag_get(app->db, action->id) == NULL,
          "action tag: it goes with the last Notes item");
    task_tag_free(action);
    (void)plain;
}

/*
 * test_note_items — a Notes-born task: named by its note; never purged
 * (nor its parent); deleted as an ordinary tombstone with
 * the link kept, so Notes can unmark the line.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_note_items(TaskApp *app)
{
    gint64 born  = task_db_task_create(app->db, 0, "From a note", "Inbox");
    gint64 plain = task_db_task_create(app->db, 0, "Typed in Tasks", "Inbox");
    gint64 sub   = task_db_task_create(app->db, plain, "A subtask", NULL);
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
    gint64 temp  = task_db_task_create(app->db, 0, "Throwaway", "Plain");
    check(task_db_task_purge(app->db, temp) && task_db_task_get(app->db, temp) == NULL &&
          task_db_tag_find(app->db, "Plain") == NULL,
          "notes: a task not born in Notes is still purged, its last tag with it");

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
 * test_tombstones — deleting a task or a tag's completed tasks tombstones
 * each task with its live subtasks; a subtask already deleted keeps its
 * own updated_at (SHARED_DB_SQL_TOMBSTONE_WHERE), so an undo can tell it
 * apart.  A subtask entering Done moves its New parent to In Progress.
 * A subtask is born with its parent's tags; a live task's last tag cannot
 * be taken off, any other can.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_tombstones(TaskApp *app)
{
    gint64 done = task_db_task_create(app->db, 0, "Done parent", "Tombs");
    gint64 kid  = task_db_task_create(app->db, done, "Live child", NULL);
    gint64 old  = task_db_task_create(app->db, done, "Deleted child", "Extra");
    gint64 open = task_db_task_create(app->db, 0, "Open parent", "Tombs");
    gint64 sub  = task_db_task_create(app->db, open, "Open child", NULL);
    char   sql[160];

    gchar *names = tag_names_of(app, kid);
    check(strcmp(names, "Tombs") == 0, "tags: a subtask is born with its parent's tags");
    g_free(names);
    names = tag_names_of(app, old);
    check(strcmp(names, "Extra,Tombs") == 0, "tags: ... and the one it was given");
    g_free(names);
    TaskTag *tombs = task_db_tag_find(app->db, "Tombs");
    TaskTag *extra = task_db_tag_find(app->db, "Extra");
    check(task_db_task_tag_remove(app->db, sub, tombs->id) == 0,
          "tags: a live task's last tag cannot be taken off");
    names = tag_names_of(app, sub);
    check(strcmp(names, "Tombs") == 0, "tags: ... and it keeps it");
    g_free(names);
    check(task_db_task_tag_remove(app->db, old, tombs->id) == 1 &&
          task_db_task_tag_remove(app->db, old, extra->id) == 0,
          "tags: another can, then not the one left");
    task_tag_free(extra);

    snprintf(sql, sizeof sql, "UPDATE tasks SET deleted = 1, updated_at = 1 WHERE id = %lld",
             (long long)old);
    other_exec(app, sql);
    check(task_db_task_set_status(app->db, kid, TASK_STATUS_DONE) &&
          task_column(app, done, "status") == TASK_STATUS_IN_PROGRESS,
          "tombstone: (a subtask entering Done starts its New parent)");
    task_db_task_set_status(app->db, done, TASK_STATUS_DONE);

    check(task_db_tasks_delete_done(app->db, tombs->id) == 1,
          "tombstone: deleting a tag's done tasks counts the top-level ones");
    task_tag_free(tombs);
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

    task_db_task_set_priority(app->db, id, TRUE);
    check(task_app_check_outside(app) == 0, "refresh: the app's own write does not refresh");
    task_db_task_set_priority(app->db, id, FALSE);
    other_exec(app, "UPDATE notes SET title = 'N2' WHERE title = 'N'");
    check(task_app_check_outside(app) == TASK_DB_CHANGED_NOTES,
          "refresh: ... nor does a later Notes-only change after it");

    other_exec(app, sql);                              /* outside, then own */
    task_db_task_set_priority(app->db, id, TRUE);
    check(task_app_check_outside(app) & TASK_DB_CHANGED_TASKS,
          "refresh: an outside change made just before the app's own write still refreshes");

    task_db_task_set_priority(app->db, id, FALSE);     /* own, then outside */
    other_exec(app, "UPDATE tasks SET title = 'After' WHERE title = 'Elsewhere'");
    check(task_app_check_outside(app) == TASK_DB_CHANGED_TASKS,
          "refresh: an outside change made just after the app's own write refreshes");

    /* The race: another commit lands between the app's COMMIT and the
     * poll that follows it, so that poll sees both moves at once.         */
    snprintf(race_sql, sizeof race_sql,
             "UPDATE tasks SET title = 'Raced' WHERE id = %lld", (long long)id);
    sqlite3_wal_hook(shared_db_handle(app->db->shared), commit_right_after, app);
    task_db_task_set_priority(app->db, id, TRUE);
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
    gint64 child  = task_db_task_create(app->db, id, "Busy child", NULL);
    int    out[2];
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
    gboolean flagged = task_db_task_set_priority(app->db, id, TRUE);
    check(!flagged && time(NULL) - started >= SHARED_DB_BUSY_TIMEOUT_MS / 1000 - 1,
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
    check(stored != NULL && !stored->priority && strcmp(stored->title, "Busy") == 0 &&
          stored->status == TASK_STATUS_NEW && kid != NULL && kid->status == TASK_STATUS_NEW,
          "busy: nothing was written meanwhile (neither the subtask nor its parent)");
    task_free(stored);
    task_free(kid);
    GPtrArray *none = gcal_store_calendars(app->db);
    check(none->len == 0, "busy: ... not even part of the calendar list");
    g_ptr_array_free(none, TRUE);

    check(task_db_task_set_priority(app->db, id, TRUE) &&
          task_db_task_update(app->db, before, &after) == 1 &&
          gcal_store_set_calendars(app->db, cals),
          "busy: the same writes succeed once the file is free");
    stored = task_db_task_get(app->db, id);
    check(stored != NULL && stored->priority && strcmp(stored->title, "Typed while busy") == 0,
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
    test_action_tag(app);
    test_note_items(app);
    test_shared_tags(app);
    test_refresh_rev(app);
    test_tombstones(app);
    test_busy(app, argv[0]);

    fixture_close(app);
    return check_result();
}
