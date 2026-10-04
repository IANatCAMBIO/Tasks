/*
 * sort_test.c — the arrangement of the task list's rows (src/task_sort.h):
 * the five column comparators, the stable multi-key sort, the saved
 * spelling of a sort, and the block move of a manual drag.
 */

#include "check.h"
#include "task_sort.h"

#include <string.h>
#include <time.h>

/* How many rows the scale check sorts: a large real database's worth. */
#define MANY_ROWS 20000

/* The rows every small check sorts.  Index: what it is there for. */
static task_row_t sample_rows[] = {
    /* 0 */ { .title = "banana",  .status = TASK_STATUS_NEW,         .due_instant = 300 },
    /* 1 */ { .title = "Apple",   .status = TASK_STATUS_DONE,        .due_instant = 100,
              .completed_at = 900 },
    /* 2 */ { .title = "cherry",  .status = TASK_STATUS_IN_PROGRESS, .due_instant = 0 },
    /* 3 */ { .title = "apple",   .status = TASK_STATUS_DONE,        .due_instant = 200,
              .completed_at = 500 },
    /* 4 */ { .title = "\xc3\x89" "clair", .status = TASK_STATUS_NEW, .due_instant = 100 },
    /* 5 */ { .title = "\xc3\xa9" "clair", .status = TASK_STATUS_IN_PROGRESS },
};
#define SAMPLE_COUNT ((int)(sizeof sample_rows / sizeof sample_rows[0]))

/*
 * sorted — the display order of the sample rows under a sort, starting from
 * the order they are declared in.
 *
 * Inputs:
 *   order  — receives SAMPLE_COUNT indices
 *   keys   — the keys
 *   n_keys — how many
 */
static void
sorted(int *order, const task_sort_key_t *keys, size_t n_keys)
{
    task_row_t *rows[SAMPLE_COUNT];

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        rows[i]  = &sample_rows[i];
        order[i] = i;
    }
    task_sort_apply(rows, order, SAMPLE_COUNT, keys, n_keys);
}

/*
 * same — whether a display order equals the expected one.
 *
 * Inputs:
 *   got  — the order
 *   want — the expected indices
 *   n    — how many
 *
 * Output:
 *   non-zero when they match.
 */
static int
same(const int *got, const int *want, int n)
{
    return memcmp(got, want, (size_t)n * sizeof *got) == 0;
}

/*
 * check_columns — each column's comparator, ascending and descending.
 */
static void
check_columns(void)
{
    int order[SAMPLE_COUNT];

    sorted(order, NULL, 0);
    check(same(order, (int[]){ 0, 1, 2, 3, 4, 5 }, SAMPLE_COUNT),
          "sort: no keys leaves the order alone");

    /* Case does not separate Apple/apple or É/é, which stay in the order
     * they came in; É sorts after every ASCII letter, by code point. */
    sorted(order, (task_sort_key_t[]){ { task_sort_title, true } }, 1);
    check(same(order, (int[]){ 1, 3, 0, 2, 4, 5 }, SAMPLE_COUNT),
          "title: case-insensitive beyond ASCII, ties keep their order");

    sorted(order, (task_sort_key_t[]){ { task_sort_title, false } }, 1);
    check(same(order, (int[]){ 4, 5, 2, 0, 1, 3 }, SAMPLE_COUNT),
          "title: descending reverses the groups, not the ties inside them");

    sorted(order, (task_sort_key_t[]){ { task_sort_done, true } }, 1);
    check(same(order, (int[]){ 0, 2, 4, 5, 1, 3 }, SAMPLE_COUNT),
          "done: Done rows after the others");

    sorted(order, (task_sort_key_t[]){ { task_sort_status, true } }, 1);
    check(same(order, (int[]){ 0, 4, 2, 5, 1, 3 }, SAMPLE_COUNT),
          "status: New, In Progress, Done");

    sorted(order, (task_sort_key_t[]){ { task_sort_due, true } }, 1);
    check(same(order, (int[]){ 1, 4, 3, 0, 2, 5 }, SAMPLE_COUNT),
          "due: soonest first, undated rows last");

    sorted(order, (task_sort_key_t[]){ { task_sort_due, false } }, 1);
    check(same(order, (int[]){ 2, 5, 0, 3, 1, 4 }, SAMPLE_COUNT),
          "due: descending is the exact reverse, undated rows first");

    sorted(order, (task_sort_key_t[]){ { task_sort_completed, true } }, 1);
    check(same(order, (int[]){ 3, 1, 0, 2, 4, 5 }, SAMPLE_COUNT),
          "completed: oldest first, never-completed rows last");

    /* A prefix sorts before the longer title. */
    task_row_t  shorter = { .title = "app" };
    task_row_t *pair[]  = { &sample_rows[3], &shorter };
    int         two[]   = { 0, 1 };
    task_sort_apply(pair, two, 2, (task_sort_key_t[]){ { task_sort_title, true } }, 1);
    check(same(two, (int[]){ 1, 0 }, 2), "title: a prefix sorts first");
}

/*
 * check_keys — several keys together, and sorting an order that is not the
 * identity (a header sort laid over a hand-made order).
 */
static void
check_keys(void)
{
    int order[SAMPLE_COUNT];

    sorted(order, (task_sort_key_t[]){ { task_sort_done, false }, { task_sort_title, true } }, 2);
    check(same(order, (int[]){ 1, 3, 0, 2, 4, 5 }, SAMPLE_COUNT),
          "keys: the second key orders the rows the first cannot tell apart");

    task_row_t *rows[SAMPLE_COUNT];
    int manual[] = { 5, 4, 3, 2, 1, 0 };
    for (int i = 0; i < SAMPLE_COUNT; i++) rows[i] = &sample_rows[i];
    task_sort_apply(rows, manual, SAMPLE_COUNT, (task_sort_key_t[]){ { task_sort_done, true } }, 1);
    check(same(manual, (int[]){ 5, 4, 2, 0, 3, 1 }, SAMPLE_COUNT),
          "keys: ties keep the order the rows were GIVEN in, not their index order");
}

/*
 * check_saved — the saved spelling of a sort, both ways.
 */
static void
check_saved(void)
{
    task_sort_key_t keys[TASK_SORT_MAX_KEYS];

    check(task_sort_keys_parse(NULL, keys) == 0 && task_sort_keys_parse("", keys) == 0,
          "saved: nothing saved means unsorted");

    size_t n = task_sort_keys_parse("due,-title", keys);
    check(n == 2 && keys[0].column == task_sort_due && keys[0].ascending &&
          keys[1].column == task_sort_title && !keys[1].ascending,
          "saved: names in order, '-' for descending");

    char *text = task_sort_keys_format(keys, n);
    check(text != NULL && strcmp(text, "due,-title") == 0, "saved: format is what parse reads");
    free(text);
    check(task_sort_keys_format(keys, 0) == NULL, "saved: no keys format to no setting");

    n = task_sort_keys_parse("bogus,,-done,titles,done,-,status", keys);
    check(n == 2 && keys[0].column == task_sort_done && !keys[0].ascending &&
          keys[1].column == task_sort_status && keys[1].ascending,
          "saved: unknown names and a repeated column are skipped");

    n = task_sort_keys_parse("title,done,status,due,completed,title,due", keys);
    check(n == TASK_SORT_MAX_KEYS, "saved: never more keys than columns");

    check(strcmp(task_sort_column_name(task_sort_completed), "completed") == 0 &&
          task_sort_column_name(task_sort_column_count) == NULL,
          "saved: a column has a name, a non-column has none");
}

/*
 * moved — move a block of a six-entry order and compare the result.
 *
 * Inputs:
 *   from   — the positions to move
 *   n_from — how many
 *   to     — the gap
 *   want   — the expected order
 *   start  — where the block's first row should end up
 *
 * Output:
 *   non-zero when the order and the returned position are as expected.
 */
static int
moved(const int *from, int n_from, int to, const int *want, int start)
{
    int order[] = { 10, 11, 12, 13, 14, 15 };
    int got     = task_sort_move(order, 6, from, n_from, to);

    return got == start && same(order, want, 6);
}

/*
 * check_move — the block move of a manual drag.
 */
static void
check_move(void)
{
    check(moved((int[]){ 1 }, 1, 4, (int[]){ 10, 12, 13, 11, 14, 15 }, 3),
          "move: one row down lands before the row at the gap");
    check(moved((int[]){ 4 }, 1, 1, (int[]){ 10, 14, 11, 12, 13, 15 }, 1),
          "move: one row up");
    check(moved((int[]){ 0 }, 1, 6, (int[]){ 11, 12, 13, 14, 15, 10 }, 5),
          "move: to the end");
    check(moved((int[]){ 5 }, 1, 0, (int[]){ 15, 10, 11, 12, 13, 14 }, 0),
          "move: to the top");
    check(moved((int[]){ 2 }, 1, 2, (int[]){ 10, 11, 12, 13, 14, 15 }, 2) &&
          moved((int[]){ 2 }, 1, 3, (int[]){ 10, 11, 12, 13, 14, 15 }, 2),
          "move: the gaps either side of a row leave it where it is");
    check(moved((int[]){ 0, 2, 5 }, 3, 4, (int[]){ 11, 13, 10, 12, 15, 14 }, 2),
          "move: a scattered block gathers at the gap in its own order");
    check(moved((int[]){ 3, 4 }, 2, 1, (int[]){ 10, 13, 14, 11, 12, 15 }, 1),
          "move: a block up");
    check(moved(NULL, 0, 3, (int[]){ 10, 11, 12, 13, 14, 15 }, 3),
          "move: nothing to move changes nothing");
}

/*
 * check_scale — a large list sorts correctly, and sorts in n log n time: a
 * quadratic sort of this many rows would take seconds.
 */
static void
check_scale(void)
{
    task_row_t  *store = calloc(MANY_ROWS, sizeof *store);
    task_row_t **rows  = calloc(MANY_ROWS, sizeof *rows);
    int         *order = calloc(MANY_ROWS, sizeof *order);
    char       (*names)[16] = calloc(MANY_ROWS, sizeof *names);
    if (store == NULL || rows == NULL || order == NULL || names == NULL) abort();

    /* Titles and due dates in a scrambled, repeating pattern. */
    for (int i = 0; i < MANY_ROWS; i++) {
        snprintf(names[i], sizeof names[i], "Task %05d", (i * 7919) % 5000);
        store[i].title       = names[i];
        store[i].due_instant = (i * 104729) % 977;
        rows[i]              = &store[i];
        order[i]             = i;
    }

    task_sort_key_t keys[] = { { task_sort_due, true }, { task_sort_title, false } };
    clock_t began = clock();
    task_sort_apply(rows, order, MANY_ROWS, keys, 2);
    double seconds = (double)(clock() - began) / CLOCKS_PER_SEC;

    int in_order = 1;
    for (int i = 1; i < MANY_ROWS; i++) {
        const task_row_t *a = rows[order[i - 1]];
        const task_row_t *b = rows[order[i]];
        int64_t da = a->due_instant == 0 ? INT64_MAX : a->due_instant;
        int64_t db = b->due_instant == 0 ? INT64_MAX : b->due_instant;
        int     by_title = strcmp(a->title, b->title);

        if (da > db || (da == db && by_title < 0) ||
            (da == db && by_title == 0 && order[i - 1] > order[i])) {
            in_order = 0;
        }
    }
    printf("     %d rows sorted by two keys in %.1f ms\n", MANY_ROWS, seconds * 1000.0);
    check(in_order, "scale: a large list comes out in order, ties stable");
    check(seconds < 0.5, "scale: ... in n log n time");

    free(names);
    free(order);
    free(rows);
    free(store);
}

/*
 * main — run the sort tests.
 *
 * Output:
 *   0 when every check passed.
 */
int
main(void)
{
    check_columns();
    check_keys();
    check_saved();
    check_move();
    check_scale();
    return check_result();
}
