/*
 * sidebar_test.c — the sidebar's toolkit-free rules (src/task_sidebar.h):
 * a list's and a tag's label, the selection fallback after a rebuild, and where a
 * dragged list lands in the saved order.
 */

#include "check.h"
#include "task_sidebar.h"

#include <string.h>

/* The rows of a sidebar with one group (id 5, holding list 11) open, and
 * two ungrouped lists.  Views 0 and 1 are on screen. */
static const task_sel_t rows[] = {
    { task_sel_view,   0 },
    { task_sel_view,   1 },
    { task_sel_header, 0 },
    { task_sel_group,  5 },
    { task_sel_list,  11 },
    { task_sel_list,  12 },
    { task_sel_list,  13 },
};
#define ROW_COUNT ((int)(sizeof rows / sizeof rows[0]))

/*
 * label_is — whether a list with this emoji and name gets this label.
 *
 * Inputs:
 *   emoji — the list's emoji, "" for none
 *   name  — the list's name
 *   want  — the expected label
 *
 * Output:
 *   non-zero when the label matches.
 */
static int
label_is(const char *emoji, const char *name, const char *want)
{
    TaskList list  = { .id = 1, .name = (gchar *)name, .emoji = (gchar *)emoji };
    char    *label = task_sidebar_list_label(&list);
    int      same  = strcmp(label, want) == 0;

    free(label);
    return same;
}

/*
 * pick — the row task_sidebar_pick chooses among `rows`.
 *
 * Inputs:
 *   kind — the wanted selection's kind
 *   id   — its id
 *
 * Output:
 *   the chosen row index, or -1.
 */
static int
pick(task_sel_kind_t kind, int64_t id)
{
    task_sel_t wanted = { kind, id };

    return task_sidebar_pick(rows, ROW_COUNT, wanted);
}

/*
 * moved — drag one of the lists { 1, 2, 3, 4, 5 } and compare the result.
 *
 * Inputs:
 *   drag_id — the list dragged
 *   before  — the index it is dropped in front of
 *   changed — whether the order should change
 *   want    — the expected order afterwards (five ids)
 *
 * Output:
 *   non-zero when both the return value and the order are as expected.
 */
static int
moved(int64_t drag_id, int before, bool changed, const int64_t *want)
{
    int64_t ids[] = { 1, 2, 3, 4, 5 };
    bool    got   = task_sidebar_move_list(ids, 5, drag_id, before);

    return got == changed && memcmp(ids, want, sizeof ids) == 0;
}

/*
 * main — run the sidebar tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    /* Labels. */
    check(label_is("\xf0\x9f\x9a\x80", "Deployments", "\xf0\x9f\x9a\x80  Deployments"),
          "label: emoji, two spaces, name");
    check(label_is("", "Someday", "Someday"), "label: the name alone without an emoji");
    TaskGroup group       = { .id = 1, .name = "Work" };
    char     *group_label = task_sidebar_group_label(&group);
    check(strcmp(group_label, "\xe2\x96\xbc  Work") == 0, "label: a group is \xe2\x96\xbc, two spaces, name");
    free(group_label);
    TaskTag tag       = { .id = 3, .name = (gchar *)"errands" };
    char   *tag_label = task_sidebar_tag_label(&tag);
    check(strcmp(tag_label, "\xf0\x9f\x8f\xb7\xef\xb8\x8f  errands") == 0,
          "label: a tag is the tag emoji, two spaces, name");
    free(tag_label);

    /* Selection: step 1, the same row. */
    check(pick(task_sel_view, 1) == 1, "pick: the same view");
    check(pick(task_sel_list, 12) == 5, "pick: the same list");
    check(pick(task_sel_group, 5) == 3, "pick: the same group");

    /* Step 2: a view that has gone leaves the user on the first view. */
    check(pick(task_sel_view, 7) == 0, "pick: a vanished view falls back to the first view");
    check(pick(task_sel_tag, 4) == 0, "pick: a vanished tag falls back to the first view");

    /* Step 3: a list or group that has gone leaves the user on the first
     * list — never on another group, and never on a view while a list is
     * on screen. */
    check(pick(task_sel_list, 99) == 4, "pick: a vanished list falls back to the first list");
    check(pick(task_sel_list, 0) == 4, "pick: list id 0 (\"any list\") is the first list");
    check(pick(task_sel_group, 99) == 4, "pick: a vanished group falls back to the first list");

    /* The header is never chosen, even when asked for. */
    check(pick(task_sel_header, 0) == 4, "pick: the header is never selected");

    /* Step 3 for a view when no view is on screen. */
    check(task_sidebar_pick(rows + 2, ROW_COUNT - 2, (task_sel_t){ task_sel_view, 0 }) == 2,
          "pick: with no views, a view falls back to the first list");

    /* Step 4: no lists on screen (the Lists header is collapsed). */
    check(task_sidebar_pick(rows, 3, (task_sel_t){ task_sel_list, 11 }) == 0,
          "pick: with no list on screen, the first row that is not the header");
    const task_sel_t header_then_group[] = { { task_sel_header, 0 }, { task_sel_group, 5 } };
    check(task_sidebar_pick(header_then_group, 2, (task_sel_t){ task_sel_list, 11 }) == 1,
          "pick: ... skipping the header");

    /* Nothing selectable. */
    check(task_sidebar_pick(rows + 2, 1, (task_sel_t){ task_sel_list, 11 }) == -1,
          "pick: only the header means no selection");
    check(task_sidebar_pick(rows, 0, (task_sel_t){ task_sel_view, 0 }) == -1,
          "pick: no rows means no selection");

    /* Reorder: `before` counts in the order before the move. */
    check(moved(2, 4, true, (int64_t[]){ 1, 3, 4, 2, 5 }), "move: down, in front of a later list");
    check(moved(4, 1, true, (int64_t[]){ 1, 4, 2, 3, 5 }), "move: up, in front of an earlier list");
    check(moved(1, 5, true, (int64_t[]){ 2, 3, 4, 5, 1 }), "move: first to after the last");
    check(moved(5, 0, true, (int64_t[]){ 5, 1, 2, 3, 4 }), "move: last to the front");

    /* In front of itself, or of its own successor, is where it already is. */
    check(moved(3, 2, false, (int64_t[]){ 1, 2, 3, 4, 5 }), "move: in front of itself changes nothing");
    check(moved(3, 3, false, (int64_t[]){ 1, 2, 3, 4, 5 }),
          "move: in front of its successor changes nothing");
    check(moved(5, 5, false, (int64_t[]){ 1, 2, 3, 4, 5 }),
          "move: the last list dropped at the end changes nothing");

    /* Bad input leaves the order alone. */
    check(moved(9, 0, false, (int64_t[]){ 1, 2, 3, 4, 5 }), "move: an unknown list changes nothing");
    check(moved(2, 99, true, (int64_t[]){ 1, 3, 4, 5, 2 }), "move: a place past the end is the end");
    check(moved(2, -3, true, (int64_t[]){ 2, 1, 3, 4, 5 }), "move: a place before the start is the start");
    check(!task_sidebar_move_list(NULL, 0, 1, 0), "move: no lists");

    return check_result();
}
