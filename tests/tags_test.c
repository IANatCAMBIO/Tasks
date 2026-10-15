/*
 * tags_test.c — Tasks' tags (db.h): adding by name (an existing tag, in
 * any case, or a new one), the refusals, removal, the schema deleting a
 * tag nothing carries any more, and a tag's proto-list in the task pane
 * (task_pane.h) with its own saved order (task_order.h).
 */

#include "fixture.h"
#include "task_order.h"
#include "task_pane.h"

#include <string.h>

/*
 * tag_names — a task's tags as one string, "a,b,c".
 *
 * Inputs:
 *   app     — the fixture
 *   task_id — the task
 *
 * Output:
 *   new string (g_free).
 */
static gchar *
tag_names(TaskApp *app, gint64 task_id)
{
    GPtrArray *tags  = task_db_task_tags(app->db, task_id);
    GString   *names = g_string_new("");

    for (guint i = 0; i < tags->len; i++) {
        if (i > 0) g_string_append_c(names, ',');
        g_string_append(names, ((const TaskTag *)g_ptr_array_index(tags, i))->name);
    }
    task_ptr_array_free_tags(tags);
    return g_string_free(names, FALSE);
}

/*
 * tag_count — how many tags exist.
 *
 * Inputs:
 *   app — the fixture
 *
 * Output:
 *   the count.
 */
static guint
tag_count(TaskApp *app)
{
    GPtrArray *tags = task_db_tags(app->db);
    guint      n    = tags->len;

    task_ptr_array_free_tags(tags);
    return n;
}

/*
 * names_are — whether a task's tags read exactly `want`.
 *
 * Inputs:
 *   app     — the fixture
 *   task_id — the task
 *   want    — the expected "a,b,c"
 *
 * Output:
 *   non-zero when they match.
 */
static int
names_are(TaskApp *app, gint64 task_id, const char *want)
{
    gchar *got  = tag_names(app, task_id);
    int    same = strcmp(got, want) == 0;

    if (!same) printf("     got \"%s\", want \"%s\"\n", got, want);
    g_free(got);
    return same;
}

/*
 * pane_ids — whether a selection's pane lists exactly these task ids, in
 * this order.
 *
 * Inputs:
 *   app  — the fixture
 *   sel  — the selection
 *   want — the expected ids
 *   n    — how many
 *
 * Output:
 *   non-zero when they match.
 */
static int
pane_ids(TaskApp *app, task_sel_t sel, const gint64 *want, size_t n)
{
    task_pane_content_t *content = task_pane_collect(app, sel, NULL);
    int                  same    = content->rows->count == n;

    for (size_t i = 0; same && i < n; i++) {
        same = content->rows->rows[i]->id == want[i];
    }
    task_pane_content_free(content);
    return same;
}

/*
 * test_add_remove — tags by name: new, existing in another case, trimmed,
 * a second add, the refusals; taking one off; the last one off deletes
 * the tag; a tombstoned task loses its tags.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_add_remove(TaskApp *app)
{
    gint64 list  = task_db_list_create(app->db, "Chores", "");
    gint64 a     = task_db_task_create(app->db, list, 0, "Buy milk");
    gint64 b     = task_db_task_create(app->db, list, 0, "Fix the shelf");
    gint64 sub   = task_db_task_create(app->db, list, b, "Find the drill");

    gint64 errands = task_db_task_tag_add(app->db, a, "Errands");
    check(errands != 0 && names_are(app, a, "Errands"), "add: a new tag");
    check(task_db_task_tag_add(app->db, b, "  errands ") == errands && names_are(app, b, "Errands"),
          "add: an existing tag, in another case and with spaces, is the same tag");
    check(task_db_task_tag_add(app->db, a, "ERRANDS") == errands && names_are(app, a, "Errands"),
          "add: a tag the task already carries is not added twice");
    gint64 diy = task_db_task_tag_add(app->db, b, "diy");
    check(diy != 0 && diy != errands && names_are(app, b, "diy,Errands"),
          "add: a second tag; a task's tags read alphabetically, ignoring case");
    check(task_db_task_tag_add(app->db, sub, "diy") == diy, "add: a subtask can carry a tag");
    check(task_db_task_tag_add(app->db, a, "   ") == 0 && names_are(app, a, "Errands"),
          "add: an empty name is refused");
    check(tag_count(app) == 2, "tags: two tags exist");

    TaskTag *tag = task_db_tag_get(app->db, errands);
    check(tag != NULL && strcmp(tag->name, "Errands") == 0, "get: a tag by id");
    task_tag_free(tag);

    check(task_db_task_tag_remove(app->db, a, errands) && names_are(app, a, ""),
          "remove: the tag comes off the task");
    tag = task_db_tag_get(app->db, errands);
    check(tag != NULL, "remove: a tag another task carries stays");
    task_tag_free(tag);
    check(task_db_task_tag_remove(app->db, b, errands) && task_db_tag_get(app->db, errands) == NULL,
          "remove: the last task's tag going deletes the tag");
    check(tag_count(app) == 1, "tags: one tag left");

    task_db_task_delete(app->db, b);
    check(names_are(app, b, "") && names_are(app, sub, "") && tag_count(app) == 0,
          "delete: a deleted task and its subtask lose their tags, and the tag goes");
    check(task_db_task_tag_add(app->db, b, "late") == 0 && tag_count(app) == 0,
          "add: refused for a deleted task, writing no tag");
    check(task_db_task_tag_add(app->db, 999999, "ghost") == 0 && tag_count(app) == 0,
          "add: refused for a task that does not exist");
}

/*
 * test_proto_list — a tag's proto-list: every live task carrying it, from
 * every list and of any level, in the pane like a list (with the tag's
 * name in the status line); Done ones follow show_completed; its manual
 * order is its own.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_proto_list(TaskApp *app)
{
    gint64 work  = task_db_list_create(app->db, "Work", "");
    gint64 home  = task_db_list_create(app->db, "Home", "");
    gint64 w1    = task_db_task_create(app->db, work, 0, "Quarterly report");
    gint64 h1    = task_db_task_create(app->db, home, 0, "Pay the bills");
    gint64 plain = task_db_task_create(app->db, home, 0, "Water the plants");
    gint64 done  = task_db_task_create(app->db, work, 0, "Book the venue");
    gint64 tag   = task_db_task_tag_add(app->db, w1, "urgent");

    task_db_task_tag_add(app->db, h1, "Urgent");
    task_db_task_tag_add(app->db, done, "urgent");
    task_db_task_set_status(app->db, done, TASK_STATUS_DONE);

    task_sel_t sel = { task_sel_tag, tag };
    check(task_sel_tag_id(sel) == tag && task_sel_is_virtual(sel) && task_sel_takes_new_tasks(sel),
          "selection: a tag is virtual and takes new tasks");

    gboolean was = task_config_get_bool(TASK_CFG_SHOW_COMPLETED);
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, FALSE);
    check(pane_ids(app, sel, (gint64[]){ w1, h1 }, 2),
          "pane: the tagged tasks of two lists, list by list, not the untagged one");
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, TRUE);
    check(pane_ids(app, sel, (gint64[]){ w1, done, h1 }, 3),
          "pane: a Done one is shown with show_completed");
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, was);

    task_pane_content_t *content = task_pane_collect(app, sel, NULL);
    check(strncmp(content->status_text, "urgent - ", 9) == 0,
          "pane: the status line names the tag");
    task_pane_content_free(content);
    (void)plain;

    int *order = task_order_load(TASK_CFG_FAMILY_MANUAL, sel, (int64_t[]){ h1, w1 }, 2);
    check(order == NULL, "order: none saved yet");
    task_order_save(TASK_CFG_FAMILY_MANUAL, sel, (int64_t[]){ w1, h1 }, 2);
    order = task_order_load(TASK_CFG_FAMILY_MANUAL, sel, (int64_t[]){ h1, w1 }, 2);
    check(order != NULL && order[0] == 1 && order[1] == 0, "order: the tag keeps its own order");
    free(order);
    gchar *key = g_strdup_printf("%s_tag_%" G_GINT64_FORMAT, TASK_CFG_FAMILY_MANUAL, tag);
    gchar *saved = task_config_get(key);
    check(saved != NULL && saved[0] != '\0', "order: kept under <family>_tag_<id>");
    g_free(saved);
    g_free(key);
}

/*
 * main — run the tag tests on a fresh database.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp *app = fixture_open();

    test_add_remove(app);
    test_proto_list(app);

    fixture_close(app);
    return check_result();
}
