/*
 * order_test.c — saved task orders (src/task_order.h): the key spelling,
 * reading an order back as a permutation, and dropping a deleted list's
 * keys.
 */

#include "fixture.h"
#include "task_order.h"

#include <string.h>

/*
 * same — whether a permutation equals the expected one.
 *
 * Inputs:
 *   got  — the permutation, or NULL
 *   want — the expected indices
 *   n    — how many
 *
 * Output:
 *   non-zero when they match.
 */
static int
same(const int *got, const int *want, int n)
{
    return got != NULL && memcmp(got, want, (size_t)n * sizeof *got) == 0;
}

/*
 * main — run the order tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    TaskApp   *app  = fixture_open();
    task_sel_t list = { task_sel_list, 7 };
    task_sel_t grp  = { task_sel_group, 7 };
    task_sel_t all  = task_sel_default();
    task_sel_t head = { task_sel_header, 0 };
    int64_t    ids[] = { 10, 20, 30, 40 };

    check(task_order_load(TASK_CFG_FAMILY_MANUAL, list, ids, 4) == NULL,
          "load: no saved order means nothing to do");

    /* Save, and read the raw keys: the spelling is part of the ini format. */
    int64_t wanted[] = { 30, 10, 0, 40, 20 };
    task_order_save(TASK_CFG_FAMILY_MANUAL, list, wanted, 5);
    gchar *raw = task_config_get("manual_order_list_7");
    check(raw != NULL && strcmp(raw, "30,10,40,20") == 0,
          "save: list key spelling, comma separated, zero ids skipped");
    g_free(raw);

    task_order_save(TASK_CFG_FAMILY_KANBAN, grp, wanted, 2);
    raw = task_config_get("kanban_order_group_7");
    check(raw != NULL && strcmp(raw, "30,10") == 0, "save: group key, kanban family");
    g_free(raw);

    task_order_save(TASK_CFG_FAMILY_MANUAL, all, wanted, 1);
    raw = task_config_get("manual_order_all");
    check(raw != NULL && strcmp(raw, "30") == 0, "save: a view's key uses its id");
    g_free(raw);

    task_order_save(TASK_CFG_FAMILY_MANUAL, head, wanted, 5);
    check(task_order_load(TASK_CFG_FAMILY_MANUAL, head, ids, 4) == NULL,
          "save/load: the Lists header keeps no order");

    /* Load: saved ids first, in saved sequence. */
    int *order = task_order_load(TASK_CFG_FAMILY_MANUAL, list, ids, 4);
    check(same(order, (int[]){ 2, 0, 3, 1 }, 4), "load: the saved sequence");
    free(order);

    /* The families are separate. */
    order = task_order_load(TASK_CFG_FAMILY_KANBAN, list, ids, 4);
    check(order == NULL, "load: the board's order of a list is not the list's");

    /* Unknown ids are skipped; unmentioned rows keep their order at the
     * tail; a stale token parses to nothing. */
    task_config_set("manual_order_list_7", "99,40,junk,10");
    order = task_order_load(TASK_CFG_FAMILY_MANUAL, list, ids, 4);
    check(same(order, (int[]){ 3, 0, 1, 2 }, 4),
          "load: unknown ids skipped, the rest appended in their current order");
    free(order);

    /* Duplicate ids on screen are each placed once. */
    int64_t dup[] = { 5, 6, 5 };
    task_config_set("manual_order_list_7", "5,5,6");
    order = task_order_load(TASK_CFG_FAMILY_MANUAL, list, dup, 3);
    check(same(order, (int[]){ 0, 2, 1 }, 3), "load: a valid permutation with duplicate ids");
    free(order);

    check(task_order_load(TASK_CFG_FAMILY_MANUAL, list, ids, 1) == NULL,
          "load: a single row has nothing to reorder");

    /* Dropping a list's keys removes both families and nothing else. */
    task_order_save(TASK_CFG_FAMILY_KANBAN, list, wanted, 2);
    task_order_drop(list);
    check(task_config_get("manual_order_list_7") == NULL &&
          task_config_get("kanban_order_list_7") == NULL,
          "drop: both families' keys of the list are gone");
    raw = task_config_get("kanban_order_group_7");
    check(raw != NULL, "drop: the group with the same id keeps its order");
    g_free(raw);
    task_order_drop(all);
    raw = task_config_get("manual_order_all");
    check(raw != NULL, "drop: a view's order is never dropped");
    g_free(raw);

    fixture_close(app);
    return check_result();
}
