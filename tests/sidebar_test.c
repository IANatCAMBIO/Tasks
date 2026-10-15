/*
 * sidebar_test.c — the sidebar's toolkit-free rules: a tag's label
 * (task_tag_label, db.h) and the selection fallback after a rebuild
 * (src/task_sidebar.h).
 */

#include "check.h"
#include "db.h"
#include "task_sidebar.h"

#include <string.h>

/* The rows of a sidebar: three views, the Tags header, three tags. */
static const task_sel_t rows[] = {
    { task_sel_view,   0 },
    { task_sel_view,   1 },
    { task_sel_view,   2 },
    { task_sel_header, 0 },
    { task_sel_tag,   11 },
    { task_sel_tag,   12 },
    { task_sel_tag,   13 },
};
#define ROW_COUNT ((int)(sizeof rows / sizeof rows[0]))

/*
 * label_is — whether a tag with this emoji and name gets this label.
 *
 * Inputs:
 *   emoji — the tag's emoji, "" for none
 *   name  — the tag's name
 *   want  — the expected label
 *
 * Output:
 *   non-zero when the label matches.
 */
static int
label_is(const char *emoji, const char *name, const char *want)
{
    TaskTag tag   = { .id = 1, .name = (gchar *)name, .emoji = (gchar *)emoji };
    gchar  *label = task_tag_label(&tag);
    int     same  = strcmp(label, want) == 0;

    g_free(label);
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
 * main — run the sidebar rule tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    check(label_is("\xf0\x9f\x9b\x92", "Errands", "\xf0\x9f\x9b\x92  Errands"),
          "label: emoji, two spaces, name");
    check(label_is("", "Someday", "Someday"), "label: the name alone without an emoji");

    /* Step 1, the same row. */
    check(pick(task_sel_view, 1) == 1, "pick: the same view");
    check(pick(task_sel_tag, 12) == 5, "pick: the same tag");

    /* Step 2: a row that has gone leaves the user on the first view. */
    check(pick(task_sel_view, 7) == 0, "pick: a vanished view falls back to the first view");
    check(pick(task_sel_tag, 99) == 0, "pick: a vanished tag falls back to the first view");

    /* The header is never chosen, even when asked for. */
    check(pick(task_sel_header, 0) == 0, "pick: the header is never selected");

    /* Step 3: no view on screen. */
    check(task_sidebar_pick(rows + 3, ROW_COUNT - 3, (task_sel_t){ task_sel_tag, 99 }) == 1,
          "pick: with no views, the first row that is not the header");

    /* Nothing selectable. */
    check(task_sidebar_pick(rows + 3, 1, (task_sel_t){ task_sel_tag, 11 }) == -1,
          "pick: only the header means no selection");
    check(task_sidebar_pick(rows, 0, (task_sel_t){ task_sel_view, 0 }) == -1,
          "pick: no rows means no selection");

    return check_result();
}
