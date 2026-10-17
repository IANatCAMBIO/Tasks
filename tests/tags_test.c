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
 * tag_count — how many tags the tasks carry.
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
    GPtrArray *tags = task_db_tags(app->db, TASK_TAGS_CARRIED);
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
 * a second add, a subtask's inherited tags, the refusals; taking one off;
 * a live task's last tag comes off; the last task's tag going deletes the tag;
 * a tombstoned task loses its tags.
 *
 * Inputs:
 *   app — the fixture
 */
static void
test_add_remove(TaskApp *app)
{
    gint64 a   = task_db_task_create(app->db, 0, "Buy milk", "Chores");
    gint64 b   = task_db_task_create(app->db, 0, "Fix the shelf", "Chores");
    gint64 sub = task_db_task_create(app->db, b, "Find the drill", NULL);

    check(names_are(app, a, "Chores") && names_are(app, sub, "Chores"),
          "create: a task has its tag, a subtask its parent's");
    gint64 errands = task_db_task_tag_add(app->db, a, "Errands");
    check(errands != 0 && names_are(app, a, "Chores,Errands"), "add: a new tag");
    check(task_db_task_tag_add(app->db, b, "  errands ") == errands &&
          names_are(app, b, "Chores,Errands"),
          "add: an existing tag, in another case and with spaces, is the same tag");
    check(task_db_task_tag_add(app->db, a, "ERRANDS") == errands &&
          names_are(app, a, "Chores,Errands"),
          "add: a tag the task already carries is not added twice");
    gint64 diy = task_db_task_tag_add(app->db, b, "diy");
    check(diy != 0 && diy != errands && names_are(app, b, "Chores,diy,Errands"),
          "add: a third tag; a task's tags read alphabetically, ignoring case");
    check(task_db_task_tag_add(app->db, sub, "diy") == diy, "add: a subtask can carry a tag");
    check(task_db_task_tag_add(app->db, a, "   ") == 0 && names_are(app, a, "Chores,Errands"),
          "add: an empty name is refused");
    check(tag_count(app) == 3, "tags: three tags exist");

    GHashTable *counts = task_db_tag_open_counts(app->db);
    check(GPOINTER_TO_INT(g_hash_table_lookup(counts, GINT_TO_POINTER(errands))) == 2 &&
          GPOINTER_TO_INT(g_hash_table_lookup(counts, GINT_TO_POINTER(diy))) == 2,
          "counts: each tag's open tasks, a subtask among them");
    g_hash_table_destroy(counts);
    task_db_task_set_status(app->db, sub, TASK_STATUS_DONE);
    counts = task_db_tag_open_counts(app->db);
    check(GPOINTER_TO_INT(g_hash_table_lookup(counts, GINT_TO_POINTER(diy))) == 1,
          "counts: a Done task is not counted");
    g_hash_table_destroy(counts);
    task_db_task_set_status(app->db, sub, TASK_STATUS_NEW);

    TaskTag *tag = task_db_tag_get(app->db, errands);
    check(tag != NULL && strcmp(tag->name, "Errands") == 0 && strcmp(tag->emoji, "") == 0 &&
          !tag->favorite, "get: a tag by id, no emoji, not a favorite");
    task_tag_free(tag);
    check(task_db_tag_set_emoji(app->db, errands, " \xf0\x9f\x9b\x92 ") &&
          task_db_tag_set_favorite(app->db, errands, TRUE), "tag: emoji and favorite set");
    tag = task_db_tag_find(app->db, " errands ");
    check(tag != NULL && tag->id == errands && strcmp(tag->emoji, "\xf0\x9f\x9b\x92") == 0 &&
          tag->favorite, "find: by name in any case; the emoji trimmed, the flag kept");
    task_tag_free(tag);
    check(task_db_tag_find(app->db, "nothing") == NULL, "find: no tag of that name");

    check(task_db_task_tag_remove(app->db, a, errands) == 1 && names_are(app, a, "Chores"),
          "remove: the tag comes off the task");
    tag = task_db_tag_get(app->db, errands);
    check(tag != NULL, "remove: a tag another task carries stays");
    task_tag_free(tag);
    check(task_db_task_tag_remove(app->db, b, errands) == 1 &&
          task_db_tag_get(app->db, errands) == NULL,
          "remove: the last task's tag going deletes the tag");
    TaskTag *chores = task_db_tag_find(app->db, "Chores");
    check(task_db_task_tag_remove(app->db, a, chores->id) == 1 && names_are(app, a, ""),
          "remove: a task's last tag comes off too; it carries none");
    tag = task_db_tag_get(app->db, chores->id);
    check(tag != NULL, "remove: the tag the other tasks carry stays");
    task_tag_free(tag);
    task_tag_free(chores);

    task_db_task_delete(app->db, b);
    check(names_are(app, b, "") && names_are(app, sub, "") && tag_count(app) == 0,
          "delete: a deleted task and its subtask lose their tags, and diy goes");
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
    gint64 w1    = task_db_task_create(app->db, 0, "Quarterly report", "Work");
    gint64 h1    = task_db_task_create(app->db, 0, "Pay the bills", "Home");
    gint64 plain = task_db_task_create(app->db, 0, "Water the plants", "Home");
    gint64 done  = task_db_task_create(app->db, 0, "Book the venue", "Work");
    gint64 tag   = task_db_task_tag_add(app->db, w1, "urgent");

    task_db_task_tag_add(app->db, h1, "Urgent");
    task_db_task_tag_add(app->db, done, "urgent");
    task_db_task_set_status(app->db, done, TASK_STATUS_DONE);

    task_sel_t sel = { task_sel_tag, tag };
    check(task_sel_tag_id(sel) == tag, "selection: a tag selection names its tag");

    gboolean was = task_config_get_bool(TASK_CFG_SHOW_COMPLETED);
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, FALSE);
    check(pane_ids(app, sel, (gint64[]){ w1, h1 }, 2),
          "pane: the tasks carrying the tag, in their one order, not the others");
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, TRUE);
    check(pane_ids(app, sel, (gint64[]){ w1, h1, done }, 3),
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
