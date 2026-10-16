/* ===========================================================================
 * search.h — the toolbar search box's query language and matcher.
 *
 * A query is parsed ONCE into a TaskSearch and then run against every task
 * of the current view.  Parsing per task would casefold the same needle
 * once per row, and the task pane can hold thousands.
 *
 * WHAT IS SEARCHED — the task's TITLE, its NOTES, and the TITLES of its
 * subtasks, as one haystack.  A subtask match surfaces its PARENT, which
 * is the only thing it could do: the task pane lists top-level tasks, and
 * a subtask has no row of its own to select.
 *
 * THE QUERY LANGUAGE
 * ------------------
 * Whitespace separates TERMS, and every term must be satisfied — terms
 * AND together, they do not OR.  Two operators:
 *
 *   "quoted phrase"   the whole phrase, spaces included, as one term.
 *                     An unterminated quote runs to the end of the query,
 *                     so a phrase still filters sensibly while it is being
 *                     typed.
 *   -word             EXCLUDE: no task containing `word` anywhere in the
 *                     searched text.  `-"quoted phrase"` excludes a phrase.
 *
 * The '-' is an operator only at the START of a term, so "well-known"
 * searches for the hyphen literally, and a leading one can be searched for
 * by quoting it ("-5 degrees").
 *
 * Matching is CASE-INSENSITIVE and Unicode-aware (g_utf8_casefold, not
 * ASCII tolower), so an accented or non-Latin query matches the way the
 * user expects.  There is no regex mode and no field: prefix; if either is
 * wanted later it belongs here, behind this same parse-once API.
 *
 * A query of nothing but whitespace, or one whose every term is empty
 * (a bare '-', an empty ""), parses to NULL — "no filter at all" rather
 * than "a filter that matches everything".  That makes the caller's "is a
 * search active?" test a NULL check and nothing more.
 * =========================================================================== */

#ifndef TASK_SEARCH_H
#define TASK_SEARCH_H

#include <glib.h>

#include "db.h"

typedef struct TaskSearch TaskSearch;

/* ---------------------------------------------------------------------------
 * task_search_parse() — compile `query` into a matcher.
 *   query — the raw text from the search entry; may be NULL.
 *
 * Returns a new TaskSearch (free with task_search_free), or NULL when the
 * query holds no usable term — see the header comment: NULL means "do not
 * filter", and is the normal answer for an empty box.
 * ------------------------------------------------------------------------- */
TaskSearch *task_search_parse(const gchar *query);

/* task_search_free() — release a parsed query.  NULL is a no-op.          */
void task_search_free(TaskSearch *q);

/* ---------------------------------------------------------------------------
 * task_search_matches() — does one task satisfy the query?
 *   q    — the parsed query, or NULL (which matches EVERYTHING, so an
 *          unfiltered caller needs no branch of its own).
 *   t    — the task, whose title and notes are searched.  Must not be NULL.
 *   subs — its subtasks (borrowed Task*, as the row context groups them),
 *          whose TITLES are searched too.  NULL for a task with none.
 *
 * Returns TRUE when every non-negated term appears in that text and no
 * negated one does.  The fields are joined with newlines before matching,
 * so a quoted phrase cannot match across the seam between a title and the
 * notes under it.
 * ------------------------------------------------------------------------- */
gboolean task_search_matches(const TaskSearch *q, const Task *t,
                             GPtrArray *subs);

#endif /* TASK_SEARCH_H */
