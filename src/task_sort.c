#include "task_sort.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <xlocale.h>

/* The columns' names, indexed by task_sort_column_t (see
 * task_sort_column_name: they are part of the ini format). */
static const char * const column_names[task_sort_column_count] = {
    [task_sort_title]     = "title",
    [task_sort_tags]      = "tags",
    [task_sort_done]      = "done",
    [task_sort_status]    = "status",
    [task_sort_due]       = "due",
    [task_sort_schedule]  = "schedule",
    [task_sort_completed] = "completed",
    [task_sort_created]   = "created",
};

/* What a merge needs to compare two entries of a display order. */
typedef struct {
    task_row_t *const     *rows;
    const task_sort_key_t *keys;
    size_t                 n_keys;
} sort_ctx_t;

/* ── Comparators ────────────────────────────────────────────────────── */

/*
 * fold_case — a code point's lower-case form, the Unicode way: "É" equals
 * "é", not only "A" equals "a".  The C library folds beyond ASCII only
 * through a UTF-8 locale, which is made once and kept for the life of the
 * process; if the system has none, only ASCII folds.
 *
 * Inputs:
 *   cp — a Unicode code point
 *
 * Output:
 *   its lower-case code point, or cp when it has none.
 */
static uint32_t
fold_case(uint32_t cp)
{
    static locale_t utf8_locale;
    static bool     looked_up;

    if (cp < 0x80) return cp >= 'A' && cp <= 'Z' ? cp + ('a' - 'A') : cp;

    if (!looked_up) {
        utf8_locale = newlocale(LC_CTYPE_MASK, "UTF-8", NULL);
        looked_up   = true;
    }
    return utf8_locale != NULL ? (uint32_t)towlower_l((wint_t)cp, utf8_locale) : cp;
}

/*
 * next_folded — decode one UTF-8 character and fold its case.  A malformed
 * sequence is not an error here: its bytes decode to SOME code point, the
 * same one every time, which is all an ordering needs.
 *
 * Inputs:
 *   text — the text, at a character that is not the terminating NUL
 *   out  — receives the folded code point
 *
 * Output:
 *   the text just after that character (never past the NUL).
 */
static const char *
next_folded(const char *text, uint32_t *out)
{
    const unsigned char *p  = (const unsigned char *)text;
    uint32_t             cp = *p++;
    int                  extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;

    if (extra > 0) cp &= 0x3Fu >> extra;        /* the lead byte's payload */
    for (; extra > 0 && (*p & 0xC0) == 0x80; extra--) cp = (cp << 6) | (*p++ & 0x3F);

    *out = fold_case(cp);
    return (const char *)p;
}

/*
 * compare_titles — order two titles without regard to case, by code point.
 *
 * Inputs:
 *   a, b — the titles (UTF-8, not NULL)
 *
 * Output:
 *   negative, zero or positive.
 */
static int
compare_titles(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        uint32_t ca;
        uint32_t cb;

        a = next_folded(a, &ca);
        b = next_folded(b, &cb);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return (*a != '\0') - (*b != '\0');         /* the shorter one first */
}

/*
 * compare_times — order two timestamps, soonest first, with 0 ("none")
 * after every real one.
 *
 * Inputs:
 *   a, b — unix timestamps; 0 means the row has none
 *
 * Output:
 *   negative, zero or positive.
 */
static int
compare_times(int64_t a, int64_t b)
{
    if (a == 0) a = INT64_MAX;
    if (b == 0) b = INT64_MAX;
    return (a > b) - (a < b);
}

/*
 * compare_column — the ascending order of two rows by one column.
 *
 * Inputs:
 *   a, b   — the rows
 *   column — the column to compare by
 *
 * Output:
 *   negative, zero or positive.
 */
static int
compare_column(const task_row_t *a, const task_row_t *b, task_sort_column_t column)
{
    switch (column) {
    case task_sort_title:
        return compare_titles(a->title, b->title);
    case task_sort_tags: {
        int cmp = compare_titles(a->tags_text, b->tags_text);
        return cmp != 0 ? cmp : compare_titles(a->title, b->title);
    }
    case task_sort_done:
        return (a->status == TASK_STATUS_DONE) - (b->status == TASK_STATUS_DONE);
    case task_sort_status:
        return ((int)a->status > (int)b->status) - ((int)a->status < (int)b->status);
    case task_sort_due:
        return compare_times(a->due_instant, b->due_instant);
    case task_sort_schedule:
        return compare_times(a->scheduled_start, b->scheduled_start);
    case task_sort_completed:
        return compare_times(a->completed_at, b->completed_at);
    case task_sort_created:
        return compare_times(a->created_at, b->created_at);
    case task_sort_column_count:
        break;
    }
    return 0;
}

/*
 * compare_entries — the order of two entries of a display order under every
 * key, the first key that tells them apart deciding.
 *
 * Inputs:
 *   ctx  — the rows and the keys
 *   a, b — indices into the rows
 *
 * Output:
 *   negative, zero (no key tells them apart) or positive.
 */
static int
compare_entries(const sort_ctx_t *ctx, int a, int b)
{
    for (size_t i = 0; i < ctx->n_keys; i++) {
        int cmp = compare_column(ctx->rows[a], ctx->rows[b], ctx->keys[i].column);
        if (cmp != 0) return ctx->keys[i].ascending ? cmp : -cmp;
    }
    return 0;
}

/* ── Sorting ────────────────────────────────────────────────────────── */

/*
 * sort_range — merge sort of order[lo, hi).  A merge sort because it is
 * stable and takes its context as an argument; qsort is neither.
 *
 * Inputs:
 *   ctx     — the rows and the keys
 *   order   — the display order
 *   scratch — working room, as long as order
 *   lo, hi  — the range, hi exclusive
 */
static void
sort_range(const sort_ctx_t *ctx, int *order, int *scratch, int lo, int hi)
{
    if (hi - lo < 2) return;

    int mid = lo + (hi - lo) / 2;
    sort_range(ctx, order, scratch, lo, mid);
    sort_range(ctx, order, scratch, mid, hi);

    /* The halves already follow one another: nothing to merge.  This is
     * what makes re-sorting an already sorted list nearly free. */
    if (compare_entries(ctx, order[mid - 1], order[mid]) <= 0) return;

    memcpy(scratch + lo, order + lo, (size_t)(hi - lo) * sizeof *order);
    int left  = lo;
    int right = mid;
    for (int i = lo; i < hi; i++) {
        /* On a tie the left entry goes first: that is the stability. */
        bool take_left = right >= hi ||
            (left < mid && compare_entries(ctx, scratch[left], scratch[right]) <= 0);
        order[i] = take_left ? scratch[left++] : scratch[right++];
    }
}

/*
 * task_sort_apply — sort a display order by the keys, stably.
 * Inputs are documented in task_sort.h.
 */
void
task_sort_apply(task_row_t *const *rows, int *order, int n,
                const task_sort_key_t *keys, size_t n_keys)
{
    if (n < 2 || n_keys == 0) return;

    sort_ctx_t ctx     = { rows, keys, n_keys };
    int       *scratch = malloc((size_t)n * sizeof *scratch);
    if (scratch == NULL) abort();

    sort_range(&ctx, order, scratch, 0, n);
    free(scratch);
}

/* ── The saved sort ─────────────────────────────────────────────────── */

/*
 * task_sort_column_name — a column's name.
 * Inputs/output are documented in task_sort.h.
 */
const char *
task_sort_column_name(task_sort_column_t column)
{
    return (unsigned)column < task_sort_column_count ? column_names[column] : NULL;
}

/*
 * task_sort_keys_parse — read a saved sort.
 * Inputs/output are documented in task_sort.h.
 */
size_t
task_sort_keys_parse(const char *text, task_sort_key_t *keys)
{
    bool   seen[task_sort_column_count] = { false };
    size_t n_keys = 0;

    for (const char *p = text; p != NULL && *p != '\0'; ) {
        bool   ascending = *p != '-';
        if (!ascending) p++;
        size_t length = strcspn(p, ",");

        for (int column = 0; column < task_sort_column_count; column++) {
            if (strlen(column_names[column]) == length &&
                strncmp(p, column_names[column], length) == 0 && !seen[column]) {
                seen[column]           = true;
                keys[n_keys].column    = (task_sort_column_t)column;
                keys[n_keys].ascending = ascending;
                n_keys++;
            }
        }
        p += length;
        if (*p == ',') p++;
    }
    return n_keys;
}

/*
 * task_sort_keys_format — the saved spelling of a sort.
 * Inputs/output are documented in task_sort.h.
 */
char *
task_sort_keys_format(const task_sort_key_t *keys, size_t n_keys)
{
    if (n_keys == 0) return NULL;

    /* Each key is at most '-', the longest name ("completed") and a comma. */
    size_t room = n_keys * 12 + 1;
    char  *text = malloc(room);
    size_t used = 0;
    if (text == NULL) abort();

    for (size_t i = 0; i < n_keys; i++) {
        used += (size_t)snprintf(text + used, room - used, "%s%s%s", i > 0 ? "," : "",
                                 keys[i].ascending ? "" : "-",
                                 task_sort_column_name(keys[i].column));
    }
    return text;
}

/* ── The manual move ────────────────────────────────────────────────── */

/*
 * task_sort_move — move a block of positions to a gap.
 * Inputs/output are documented in task_sort.h.
 */
int
task_sort_move(int *order, int n, const int *from, int n_from, int to)
{
    if (n_from <= 0) return to;

    int *moved = malloc((size_t)n_from * sizeof *moved);
    int  taken = 0;
    int  kept  = 0;
    int  start = to;
    if (moved == NULL) abort();

    /* Lift the block out and close the rows up behind it.  Every lifted
     * row that sat above the gap brings the gap one place up. */
    for (int i = 0; i < n; i++) {
        if (taken < n_from && from[taken] == i) {
            moved[taken++] = order[i];
            if (i < to) start--;
        } else {
            order[kept++] = order[i];
        }
    }

    memmove(order + start + n_from, order + start, (size_t)(kept - start) * sizeof *order);
    memcpy(order + start, moved, (size_t)n_from * sizeof *order);
    free(moved);
    return start;
}
