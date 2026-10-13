#include "task_rows.h"
#include "task_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most lines one row can have: title, "in <list>", notes, attachments,
 * the subtask lines and "+N more". */
#define ROW_MAX_LINES (5 + TASK_ROW_MAX_SUBTASKS)

/* What stands in for an empty title. */
#define UNTITLED_TASK    "Untitled Task"
#define UNTITLED_SUBTASK "Untitled"

/* Declared ahead of its definition only to carry the format attribute, so
 * the compiler checks every call's arguments against its format. */
static char *text_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * text_printf — printf into a new malloc'd string.  Out of memory aborts,
 * as it does everywhere else in the core (GLib's allocator does the same).
 *
 * Inputs:
 *   fmt — printf format, then its arguments
 *
 * Output:
 *   new string (free it); never NULL.
 */
static char *
text_printf(const char *fmt, ...)
{
    char   *out = NULL;
    va_list ap;

    va_start(ap, fmt);
    int n = vasprintf(&out, fmt, ap);
    va_end(ap);
    if (n < 0) abort();
    return out;
}

/*
 * text_valid — a copy of a string that came out of the DATABASE, made valid
 * UTF-8.  We do not control what a hand-edited database holds, and a text
 * system handed one bad byte may reject the whole string and draw the row
 * blank.  Invalid bytes become U+FFFD, which shows a replacement glyph in
 * the one bad spot instead.
 *
 * Inputs:
 *   text — NUL-terminated bytes, not NULL
 *
 * Output:
 *   new valid UTF-8 string (free it); never NULL.
 */
static char *
text_valid(const char *text)
{
    if (g_utf8_validate(text, -1, NULL)) return text_printf("%s", text);

    gchar *valid = g_utf8_make_valid(text, -1);
    char  *out   = text_printf("%s", valid);
    g_free(valid);
    return out;
}

/*
 * line_is_blank — whether [start, end) holds nothing but whitespace.
 * Unicode-aware on purpose: a stray U+00A0 pasted into a note is just as
 * invisible as a space and must not earn a preview line either.
 *
 * Inputs:
 *   start — first byte of the line
 *   end   — one past its last byte
 *
 * Output:
 *   TRUE when the line has no visible content.
 */
static gboolean
line_is_blank(const gchar *start, const gchar *end)
{
    for (const gchar *p = start; p < end; p = g_utf8_next_char(p)) {
        if (!g_unichar_isspace(g_utf8_get_char(p))) return FALSE;
    }
    return TRUE;
}

/*
 * line_add — append one line to a row being built.  The row's line array
 * has room for ROW_MAX_LINES, which is every line a row can have.
 *
 * Inputs:
 *   row       — the row
 *   kind      — what the line is
 *   prefix    — glyph prefix; ownership is taken (pass a new string)
 *   text      — the text; ownership is taken
 *   alpha_pct — the text's opacity
 *
 * Output:
 *   the new line, for the caller to set bold / italic / strike on.  Every
 *   line but the title is `small`.
 */
static task_row_line_t *
line_add(task_row_t *row, task_row_line_kind_t kind, char *prefix, char *text,
         int alpha_pct)
{
    task_row_line_t *line = &row->lines[row->line_count++];

    line->kind      = kind;
    line->prefix    = prefix;
    line->text      = text;
    line->small     = kind != task_row_line_title;
    line->alpha_pct = alpha_pct;
    return line;
}

/*
 * notes_preview — the notes line of a row: the first line of the notes that
 * actually HAS content, capped at TASK_ROW_NOTES_BYTES, trimmed, with an
 * ellipsis when there is more.
 *
 * Testing only for a non-empty note was not enough: a note holding just a
 * space (or starting with a blank line) previewed as an empty line, which
 * reads as nothing at all while still making that one row a line taller
 * than its neighbours.
 *
 * Inputs:
 *   notes — the task's notes, not NULL
 *
 * Output:
 *   new string (free it), or NULL when there is nothing to preview.
 */
static char *
notes_preview(const gchar *notes)
{
    const gchar *nline = notes;     /* candidate line, start ...          */
    const gchar *nend  = nline;     /* ... and one past its last byte     */

    while (*nline != '\0') {
        const gchar *eol = strchr(nline, '\n');
        nend = eol != NULL ? eol : nline + strlen(nline);
        if (!line_is_blank(nline, nend)) break;
        if (eol == NULL) {          /* every line was blank               */
            nline = nend;
            break;
        }
        nline = eol + 1;
    }
    if (nline >= nend) return NULL;

    gsize len   = (gsize)(nend - nline);
    gsize shown = MIN(len, (gsize)TASK_ROW_NOTES_BYTES);

    /* The cap is a BYTE cap, so walk it back to a character boundary: a
     * multi-byte character straddling the cut would leave a partial
     * sequence.  g_utf8_find_prev_char from the cut gives the last
     * character that STARTS before it; keep it only when it also ends at
     * or before the cut. */
    if (shown < len) {
        const gchar *cut  = nline + shown;
        const gchar *prev = g_utf8_find_prev_char(nline, cut);
        if (prev == NULL) {
            shown = 0;              /* no boundary found: drop it all     */
        } else if (g_utf8_next_char(prev) > cut) {
            shown = (gsize)(prev - nline);   /* the cut character goes    */
        }
    }

    /* Trim both ends: leading indentation reads as a stray gap in a
     * one-line preview, trailing space would sit before the ellipsis.
     * g_strstrip works in place, so `preview` stays the pointer to free. */
    gchar *preview = g_strndup(nline, shown);
    g_strstrip(preview);

    /* Nothing survived the cap: emit no line at all rather than an empty
     * one, which is the bug the content gate above exists to prevent. */
    char *out = NULL;
    if (*preview != '\0') {
        char *valid = text_valid(preview);
        /* An ellipsis when there is more of THIS line, or any line after. */
        out = text_printf("%s%s", valid,
                          shown < len || *nend != '\0' ? "\xe2\x80\xa6" : "");
        free(valid);
    }
    g_free(preview);
    return out;
}

/*
 * lines_build — fill a row's lines from its task.
 *
 * The prefix glyphs stack outwards from the title: a star for a favourite,
 * then a siren for high priority, then an arrow for a subtask shown in a
 * virtual view.
 *
 * Inputs:
 *   row       — the row being built (lines allocated, line_count 0)
 *   t         — the task
 *   list_name — the owning list's name, or NULL when the view IS that list
 *   att_count — the task's attachment count
 *   subs      — the task's visible subtasks (GPtrArray of Task*), or NULL
 *   ctx       — the refresh's context: the bold_task_titles setting, and
 *               the note_context_titles one for the subtask lines
 */
static void
lines_build(task_row_t *row, const Task *t, const gchar *list_name,
            gint att_count, GPtrArray *subs, const TaskRowCtx *ctx)
{
    task_row_line_t *title = line_add(row, task_row_line_title,
        text_printf("%s%s%s",
                    t->parent_id != 0 ? "\xe2\x86\xb3 " : "",
                    t->priority ? "\xf0\x9f\x9a\xa8  " : "",
                    t->pinned ? "\xe2\xad\x90\xef\xb8\x8f  " : ""),
        text_printf("%s", row->title), TASK_ROW_ALPHA_FULL);
    title->bold   = ctx->bold;
    title->strike = t->status == TASK_STATUS_DONE;

    if (list_name != NULL) {
        char *name = text_valid(list_name);
        line_add(row, task_row_line_list, text_printf("%s", ""),
                 text_printf("in %s", name), TASK_ROW_ALPHA_LIST)->italic = true;
        free(name);
    }

    char *preview = notes_preview(t->notes);
    if (preview != NULL) {
        line_add(row, task_row_line_notes, text_printf("%s", ""), preview,
                 TASK_ROW_ALPHA_DETAIL);
    }

    if (att_count > 0) {
        line_add(row, task_row_line_attachments, text_printf("%s", ""),
                 text_printf("\xf0\x9f\x93\x8e %d attachment%s", att_count,
                             att_count == 1 ? "" : "s"),
                 TASK_ROW_ALPHA_DETAIL);
    }

    guint nsubs = subs != NULL ? subs->len : 0;
    for (guint i = 0; i < MIN(nsubs, (guint)TASK_ROW_MAX_SUBTASKS); i++) {
        const Task *sub   = g_ptr_array_index(subs, i);
        gboolean    done  = sub->status == TASK_STATUS_DONE;
        gchar      *shown = task_display_title(sub, ctx->note_context);
        line_add(row, task_row_line_subtask,
                 text_printf("%s", done ? "\xe2\x98\x91 " : "\xe2\x98\x90 "),
                 text_valid(*shown != '\0' ? shown : UNTITLED_SUBTASK),
                 done ? TASK_ROW_ALPHA_SUB_DONE : TASK_ROW_ALPHA_FULL)
            ->strike = done;
        g_free(shown);
    }
    if (nsubs > TASK_ROW_MAX_SUBTASKS) {
        guint more = nsubs - TASK_ROW_MAX_SUBTASKS;
        line_add(row, task_row_line_more, text_printf("%s", ""),
                 text_printf("\xe2\x80\xa6 +%u more subtask%s", more,
                             more == 1 ? "" : "s"),
                 TASK_ROW_ALPHA_DETAIL);
    }
}

/*
 * task_row_ctx_init — gather the shared lookups of one refresh.  Subtasks
 * come as ONE query grouped in memory, not one query per top-level row;
 * list names are loaded only for the virtual views.
 * Inputs are documented in task_rows.h.
 */
void
task_row_ctx_init(TaskApp *app, TaskRowCtx *ctx, gboolean virtual_view)
{
    ctx->att_counts = task_db_attachment_counts(app->db);
    ctx->all_subs   = task_db_subtasks_all_visible(app->db);
    ctx->subs_by_parent =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < ctx->all_subs->len; i++) {
        Task *s = g_ptr_array_index(ctx->all_subs, i);
        GPtrArray *bucket = g_hash_table_lookup(ctx->subs_by_parent,
            GINT_TO_POINTER(s->parent_id));
        if (bucket == NULL) {
            bucket = g_ptr_array_new();
            g_hash_table_insert(ctx->subs_by_parent,
                GINT_TO_POINTER(s->parent_id), bucket);
        }
        g_ptr_array_add(bucket, s);
    }
    ctx->list_names = NULL;
    if (virtual_view) {
        ctx->list_names = g_hash_table_new_full(g_direct_hash,
                                                g_direct_equal,
                                                NULL, g_free);
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        for (guint i = 0; i < lists->len; i++) {
            TaskList *l = g_ptr_array_index(lists, i);
            g_hash_table_insert(ctx->list_names,
                                GINT_TO_POINTER(l->id),
                                g_strdup(l->name));
        }
        task_ptr_array_free_lists(lists);
    }
    ctx->bold      = task_config_get_bool(TASK_CFG_BOLD_TITLES);
    ctx->show_done = task_config_get_bool(TASK_CFG_SHOW_COMPLETED);
    ctx->note_context = task_config_get_bool(TASK_CFG_NOTE_CONTEXT);
}

/*
 * task_row_ctx_clear — release the shared lookups.
 * Inputs are documented in task_rows.h.
 */
void
task_row_ctx_clear(TaskRowCtx *ctx)
{
    g_hash_table_destroy(ctx->att_counts);
    g_hash_table_destroy(ctx->subs_by_parent);
    task_ptr_array_free_tasks(ctx->all_subs);
    if (ctx->list_names != NULL) g_hash_table_destroy(ctx->list_names);
}

/*
 * task_row_ctx_subtasks — a top-level task's grouped subtasks.
 * Inputs/output are documented in task_rows.h.
 */
GPtrArray *
task_row_ctx_subtasks(const TaskRowCtx *ctx, const Task *t)
{
    if (t->parent_id != 0) return NULL;
    return g_hash_table_lookup(ctx->subs_by_parent, GINT_TO_POINTER(t->id));
}

/*
 * task_row_describe — the description of one task.
 * Inputs/output are documented in task_rows.h.
 */
task_row_t *
task_row_describe(const Task *t, const TaskRowCtx *ctx)
{
    task_row_t *row = calloc(1, sizeof *row);
    if (row == NULL) abort();
    row->lines = calloc(ROW_MAX_LINES, sizeof *row->lines);
    if (row->lines == NULL) abort();

    row->id     = t->id;
    row->status = t->status;
    gchar *shown = task_display_title(t, ctx->note_context);
    row->title  = text_valid(*shown != '\0' ? shown : UNTITLED_TASK);
    g_free(shown);

    const gchar *list_name = ctx->list_names != NULL
        ? g_hash_table_lookup(ctx->list_names, GINT_TO_POINTER(t->list_id))
        : NULL;
    gint att_count = GPOINTER_TO_INT(
        g_hash_table_lookup(ctx->att_counts, GINT_TO_POINTER(t->id)));
    lines_build(row, t, list_name, att_count, task_row_ctx_subtasks(ctx, t),
                ctx);

    /* The due cell carries its time of day only when that is not the 17:00
     * default (task_due_format_at).  due_instant is the sort key, since
     * `due` alone is local midnight for every row of the same day. */
    gchar *due_text       = task_due_format_at(t->due, t->due_time);
    gchar *completed_text = task_due_format(t->completed_at);

    row->due            = t->due;
    row->due_time       = t->due_time;
    row->due_instant    = task_due_instant(t->due, t->due_time);
    row->due_text       = text_printf("%s", due_text);
    row->due_urgency    = task_due_urgency(row->due_instant);
    row->completed_at   = t->completed_at;
    row->completed_text = text_printf("%s", completed_text);
    row->status_text    = task_status_label(t->status);
    g_free(due_text);
    g_free(completed_text);
    return row;
}

/*
 * task_row_free — free one row.
 * Inputs are documented in task_rows.h.
 */
void
task_row_free(task_row_t *row)
{
    if (row == NULL) return;

    for (size_t i = 0; i < row->line_count; i++) {
        free(row->lines[i].prefix);
        free(row->lines[i].text);
    }
    free(row->lines);
    free(row->title);
    free(row->due_text);
    free(row->completed_text);
    free(row);
}

/*
 * task_rows_build — the rows of a refresh, completed ones left out when the
 * setting hides them.
 * Inputs/output are documented in task_rows.h.
 */
task_row_list_t *
task_rows_build(GPtrArray *tasks, const TaskRowCtx *ctx)
{
    task_row_list_t *list = calloc(1, sizeof *list);
    if (list == NULL) abort();
    list->rows = calloc(tasks->len > 0 ? tasks->len : 1, sizeof *list->rows);
    if (list->rows == NULL) abort();

    for (guint i = 0; i < tasks->len; i++) {
        const Task *t = g_ptr_array_index(tasks, i);
        if (!ctx->show_done && t->status == TASK_STATUS_DONE) continue;
        list->rows[list->count++] = task_row_describe(t, ctx);
    }
    return list;
}

/*
 * task_row_list_free — free a list and its rows.
 * Inputs are documented in task_rows.h.
 */
void
task_row_list_free(task_row_list_t *list)
{
    if (list == NULL) return;

    for (size_t i = 0; i < list->count; i++) task_row_free(list->rows[i]);
    free(list->rows);
    free(list);
}
