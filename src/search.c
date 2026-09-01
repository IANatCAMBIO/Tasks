/* ===========================================================================
 * search.c — the query language and matcher (see search.h for the contract
 * and for what the operators mean).
 *
 * The shape: parse builds a flat array of terms, each already casefolded,
 * and match folds ONE haystack per task and runs every term over it with
 * strstr.  Both halves fold exactly once, which is the whole reason the
 * parsed query is a type rather than a string passed around.
 *
 * The scanner walks BYTES for its three delimiters — space, '"', '-' —
 * and characters for everything else.  That is safe rather than sloppy:
 * a UTF-8 continuation byte is always >= 0x80 and so can never be mistaken
 * for an ASCII delimiter.  Whitespace still goes through g_unichar_isspace,
 * because a query pasted from a document can carry a non-breaking or figure
 * space and those must split terms like any other blank.
 * =========================================================================== */

#include "search.h"

#include <string.h>

/* One term of a query: a needle plus whether its presence disqualifies a
 * task instead of qualifying it.  `text` is already casefolded.            */
typedef struct {
    gchar    *text;
    gboolean  negated;
} SearchTerm;

struct TaskSearch {
    GArray *terms;                   /* SearchTerm, owns each text          */
};

/* term_clear() — GArray clear func: release one term's needle.             */
static void
term_clear(gpointer data)
{
    SearchTerm *t = data;
    g_free(t->text);
}

TaskSearch *
task_search_parse(const gchar *query)
{
    if (query == NULL)
        return NULL;

    GArray *terms = g_array_new(FALSE, FALSE, sizeof(SearchTerm));
    g_array_set_clear_func(terms, term_clear);

    const gchar *p = query;          /* scan position                       */
    while (*p != '\0') {
        /* Skip the run of whitespace before the next term.                 */
        while (*p != '\0' && g_unichar_isspace(g_utf8_get_char(p)))
            p = g_utf8_next_char(p);
        if (*p == '\0')
            break;

        /* '-' is the exclude operator only HERE, at the term's first byte;
         * anywhere else it is an ordinary character (see search.h).        */
        gboolean negated = FALSE;    /* this term excludes rather than
                                      * requires                            */
        if (*p == '-') {
            negated = TRUE;
            p++;
        }

        /* Collect the term.  A '"' toggles phrase mode and is itself
         * dropped, so quotes may open and close anywhere in the term;
         * while phrase mode is on, whitespace is part of the needle.  An
         * unterminated quote simply runs to the end of the query, which is
         * what makes a phrase filter sensibly as it is being typed.        */
        GString *raw = g_string_new(NULL);
        gboolean quoted = FALSE;     /* inside a "…" phrase                 */
        while (*p != '\0') {
            if (*p == '"') {
                quoted = !quoted;
                p++;
                continue;
            }
            if (!quoted && g_unichar_isspace(g_utf8_get_char(p)))
                break;
            const gchar *next = g_utf8_next_char(p);
            g_string_append_len(raw, p, next - p);
            p = next;
        }

        /* A term that came out empty — a bare '-', an empty "" — is not a
         * filter anybody could have meant, so it is dropped rather than
         * kept as a needle that matches every task.                        */
        if (raw->len > 0) {
            SearchTerm term;
            term.text    = g_utf8_casefold(raw->str, raw->len);
            term.negated = negated;
            g_array_append_val(terms, term);
        }
        g_string_free(raw, TRUE);
    }

    /* No usable term means no filter at all — see search.h.                */
    if (terms->len == 0) {
        g_array_free(terms, TRUE);
        return NULL;
    }

    TaskSearch *q = g_new0(TaskSearch, 1);
    q->terms = terms;
    return q;
}

void
task_search_free(TaskSearch *q)
{
    if (q == NULL)
        return;
    g_array_free(q->terms, TRUE);
    g_free(q);
}

gboolean
task_search_matches(const TaskSearch *q, const Task *t, GPtrArray *subs)
{
    if (q == NULL)
        return TRUE;                 /* no filter: everything matches       */

    /* Build the searched text as ONE string, fields separated by newlines
     * so a quoted phrase cannot match across the seam between a title and
     * the notes below it.  Only subtask TITLES join it (search.h).         */
    GString *hay = g_string_new(t->title != NULL ? t->title : "");
    if (t->notes != NULL && *t->notes != '\0') {
        g_string_append_c(hay, '\n');
        g_string_append(hay, t->notes);
    }
    for (guint i = 0; subs != NULL && i < subs->len; i++) {
        const Task *s = g_ptr_array_index(subs, i);
        if (s->title == NULL || *s->title == '\0')
            continue;
        g_string_append_c(hay, '\n');
        g_string_append(hay, s->title);
    }

    /* Folded ONCE per task, against needles the parse already folded.      */
    gchar *folded = g_utf8_casefold(hay->str, hay->len);
    g_string_free(hay, TRUE);

    gboolean ok = TRUE;              /* every term satisfied so far         */
    for (guint i = 0; ok && i < q->terms->len; i++) {
        const SearchTerm *term = &g_array_index(q->terms, SearchTerm, i);
        gboolean hit = strstr(folded, term->text) != NULL;
        ok = term->negated ? !hit : hit;
    }
    g_free(folded);
    return ok;
}
