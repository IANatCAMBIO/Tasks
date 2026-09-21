/* ===========================================================================
 * task_rows.c — the task-row renderer (see task_rows.h)
 * =========================================================================== */

#include "task_rows.h"
#include <string.h>

/* line_is_blank() — TRUE when [start, end) holds nothing but whitespace.
 * Unicode-aware on purpose: a stray U+00A0 pasted into a note is just as
 * invisible as a space and must not earn a preview line either.            */
static gboolean
line_is_blank(const gchar *start, const gchar *end)
{
    for (const gchar *p = start; p < end; p = g_utf8_next_char(p))
        if (!g_unichar_isspace(g_utf8_get_char(p)))
            return FALSE;
    return TRUE;
}

/* append_line() — add a `\n`-separated markup line.                        */
static void
append_line(GString *s, const gchar *markup)
{
    if (s->len > 0)
        g_string_append_c(s, '\n');
    g_string_append(s, markup);
}

/* ---------------------------------------------------------------------------
 * markup_escape_db() — g_markup_escape_text() for a string that came out of
 * the DATABASE, i.e. one whose UTF-8 validity we do not control.
 *
 * A whole task cell is ONE Pango markup string, so a single bad byte
 * anywhere in it makes pango_parse_markup reject the lot and the row draws
 * completely blank — title, list, notes and all.  g_markup_escape_text does
 * not validate (it escapes the markup metacharacters and copies the rest),
 * so invalid bytes pass straight through to Pango.  g_utf8_make_valid
 * substitutes U+FFFD for them, which shows the user a replacement glyph in
 * the one bad spot instead of silently losing the entire row.
 *
 * Text the app itself produced (GtkTextBuffer contents, our own literals)
 * is always valid; this is for anything a sync payload or a hand-edited
 * database could have put there.  New string (g_free).
 * ------------------------------------------------------------------------- */
static gchar *
markup_escape_db(const gchar *text)
{
    if (g_utf8_validate(text, -1, NULL))
        return g_markup_escape_text(text, -1);
    gchar *valid = g_utf8_make_valid(text, -1);
    gchar *esc   = g_markup_escape_text(valid, -1);
    g_free(valid);
    return esc;
}

/* ---------------------------------------------------------------------------
 * task_rows_desc_markup() — build the Task cell: bold title (struck when
 * done), an "in <list>" line in the virtual views, a dimmed notes
 * preview, an attachment count, and up to four subtask lines.  This is
 * what makes the rows "extra tall".
 *
 * Prefix glyphs stack outwards from the title: ❗ marks a mirrored
 * Notes action item, then ⭐️ a favorite, then 🚨 high priority, then
 * ↳ a subtask shown in a virtual view.  The ❗ sits INNERMOST (nearest
 * the title) because it describes what the row IS, not how it is
 * flagged — and unlike the pre-mirror tag it shows in every view,
 * including the item's own list.
 *   list_name  — the owning list's name, or NULL when the view IS that
 *                list (no need to repeat it).
 *   att_count  — the task's attachment count.
 *   subs       — the task's visible subtasks (may be NULL).
 *   bold       — render the title in bold (the "bold_task_titles"
 *                setting, read once per refresh by the caller).
 * ------------------------------------------------------------------------- */
gchar *
task_rows_desc_markup(const Task *t, const gchar *list_name, gint att_count,
                      GPtrArray *subs, const TaskRowCtx *ctx)
{
    gboolean bold = ctx != NULL && ctx->bold;
    GString *s = g_string_new(NULL);
    gchar *title = markup_escape_db(
        *t->title != '\0' ? t->title : "Untitled Task");
    const gchar *open  = bold ? "<b>" : "";
    const gchar *close = bold ? "</b>" : "";
    gchar *line = t->status == TASK_STATUS_DONE
        ? g_strdup_printf("%s<s>%s</s>%s", open, title, close)
        : g_strdup_printf("%s%s%s", open, title, close);
    if (t->pinned) {                  /* favorite task wears a star         */
        gchar *p = g_strdup_printf("\xe2\xad\x90\xef\xb8\x8f  %s", line);
        g_free(line);
        line = p;
    }
    if (t->priority) {               /* high priority wears a siren         */
        gchar *p = g_strdup_printf("\xf0\x9f\x9a\xa8  %s", line);
        g_free(line);
        line = p;
    }
    if (t->parent_id != 0) {         /* a subtask row in a virtual view     */
        gchar *sub = g_strdup_printf("\xe2\x86\xb3 %s", line);
        g_free(line);
        line = sub;
    }
    append_line(s, line);
    g_free(line);
    g_free(title);

    /* Dimmed lines use Pango ALPHA, never a fixed gray: a hardcoded
     * foreground stays gray on the selection's blue background and is
     * unreadable — alpha dims whatever color the theme picks, so the
     * text follows the row's selected/unselected state.                    */
    if (list_name != NULL) {
        gchar *esc = markup_escape_db(list_name);
        gchar *l = g_strdup_printf(
            "<small><i><span alpha=\"60%%\">in %s</span></i>"
            "</small>", esc);
        append_line(s, l);
        g_free(l);
        g_free(esc);
    }

    /* Notes preview: the first line that actually HAS content, capped,
     * dimmed.  Testing `*notes != '\0'` was not enough — a note holding
     * just a space (or a leading blank line) previewed as an empty line,
     * which reads as nothing at all while still making that one row a
     * whole line taller than every other row in the list.                  */
    const gchar *nline = t->notes;   /* candidate line, start …             */
    const gchar *nend  = nline;      /* … and one past its last byte        */
    while (*nline != '\0') {
        const gchar *eol = strchr(nline, '\n');
        nend = eol != NULL ? eol : nline + strlen(nline);
        if (!line_is_blank(nline, nend))
            break;
        if (eol == NULL) {           /* every line was blank                */
            nline = nend;
            break;
        }
        nline = eol + 1;
    }
    if (nline < nend) {
        gsize len   = (gsize)(nend - nline);
        gsize shown = MIN(len, (gsize)120);
        /* The cap is a BYTE cap, so walk it back to a character boundary:
         * a multi-byte character straddling byte 120 would leave a partial
         * sequence, and the whole cell is ONE Pango markup string — so
         * pango_parse_markup rejects it and the row renders completely
         * blank, title and all (not just the preview).  g_utf8_find_prev_char
         * from the cut point gives the last character that STARTS before it;
         * keep it only when it also ends at or before the cut.             */
        if (shown < len) {
            const gchar *cut  = nline + shown;
            const gchar *prev = g_utf8_find_prev_char(nline, cut);
            if (prev == NULL)            /* no boundary found: drop it all  */
                shown = 0;
            else if (g_utf8_next_char(prev) > cut)
                shown = (gsize)(prev - nline);   /* char is cut: exclude it */
        }
        gchar *preview = g_strndup(nline, shown);
        /* Trim both ends: leading indentation reads as a stray gap in a
         * one-line preview, trailing space would sit before the ellipsis.
         * g_strstrip chugs in place, so `preview` stays the pointer to
         * free.                                                            */
        g_strstrip(preview);
        /* Nothing survived the cap (a single over-long character, or bytes
         * that were not valid UTF-8 to begin with): emit no line at all
         * rather than an empty one — an empty preview reads as nothing
         * while still making this row a line taller than its neighbours,
         * which is the bug the content-gating above exists to prevent.     */
        if (*preview != '\0') {
            gchar *esc = markup_escape_db(preview);
            gchar *l = g_strdup_printf(
                "<small><span alpha=\"65%%\">%s%s</span></small>", esc,
                /* more of THIS line, or any line after it                  */
                shown < len || *nend != '\0' ? "\xe2\x80\xa6" : "");
            append_line(s, l);
            g_free(l);
            g_free(esc);
        }
        g_free(preview);
    }

    if (att_count > 0) {
        gchar *l = g_strdup_printf(
            "<small><span alpha=\"65%%\">\xf0\x9f\x93\x8e "
            "%d attachment%s</span></small>",
            att_count, att_count == 1 ? "" : "s");
        append_line(s, l);
        g_free(l);
    }

    guint nsubs = subs != NULL ? subs->len : 0;
    for (guint i = 0; i < MIN(nsubs, 4u); i++) {
        Task *sub = g_ptr_array_index(subs, i);
        gchar *esc = markup_escape_db(
            *sub->title != '\0' ? sub->title : "Untitled");
        gchar *l = sub->status == TASK_STATUS_DONE
            ? g_strdup_printf("<small>\xe2\x98\x91 <span "
                              "alpha=\"55%%\"><s>%s</s></span>"
                              "</small>", esc)
            : g_strdup_printf("<small>\xe2\x98\x90 %s</small>", esc);
        append_line(s, l);
        g_free(l);
        g_free(esc);
    }
    if (nsubs > 4) {
        gchar *l = g_strdup_printf(
            "<small><span alpha=\"65%%\">\xe2\x80\xa6 +%u more "
            "subtask%s</span></small>", nsubs - 4,
            nsubs - 4 == 1 ? "" : "s");
        append_line(s, l);
        g_free(l);
    }
    return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------------------
 * TaskRowCtx — the shared lookups behind the task rows of one refresh
 * (avoid per-row queries).  Subtasks come as ONE query grouped in
 * memory, not one query per top-level row; list names are loaded only
 * for the virtual views (the "in <list>" line).
 * ------------------------------------------------------------------------- */

/*
 * task_row_ctx_init — initialise a TaskRowCtx for one refresh.
 * Inputs:
 *   app          — the application context (for db access and config)
 *   ctx          — the context to fill (caller puts it on the stack)
 *   virtual_view — TRUE if rows should carry "in <list>" lines
 * Output: none (ctx is modified in place; call task_row_ctx_clear when done).
 */
void
task_row_ctx_init(TaskApp *app, TaskRowCtx *ctx, gboolean virtual_view)
{
    ctx->att_counts = task_db_attachment_counts(app->db);
    ctx->all_subs = task_db_subtasks_all_visible(app->db);
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
    ctx->bold = task_app_config_get_bool("bold_task_titles", FALSE);
    ctx->show_done = task_app_config_get_bool("show_completed", TRUE);
}

/*
 * task_row_ctx_clear — release resources held by a TaskRowCtx.
 * Inputs: ctx — the context built by task_row_ctx_init
 * Output: none
 */
void
task_row_ctx_clear(TaskRowCtx *ctx)
{
    g_hash_table_destroy(ctx->att_counts);
    g_hash_table_destroy(ctx->subs_by_parent);
    task_ptr_array_free_tasks(ctx->all_subs);
    if (ctx->list_names != NULL)
        g_hash_table_destroy(ctx->list_names);
}

/*
 * task_rows_append — replace the contents of `store` with TaskRow objects
 * built from `tasks` through the shared-lookup context, honoring the
 * completed-visibility toggle, as ONE atomic splice.
 *
 * ONE splice avoids the visual flicker of adding rows one by one and
 * prevents the sort model from resorting after each insert.
 *
 * Inputs:
 *   store — the GListStore to fill (emptied and replaced)
 *   tasks — the tasks to show; order is preserved
 *   ctx   — the shared lookups built by task_row_ctx_init
 * Output: count of rows actually added (< tasks->len when done are hidden).
 */
guint
task_rows_append(GListStore *store, GPtrArray *tasks, const TaskRowCtx *ctx)
{
    /* Build the new items into a GPtrArray first so we can do ONE splice. */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        gboolean done = t->status == TASK_STATUS_DONE;
        if (!ctx->show_done && done)
            continue;                /* toolbar completed-visibility toggle */

        GPtrArray *subs = t->parent_id == 0
            ? g_hash_table_lookup(ctx->subs_by_parent,
                                  GINT_TO_POINTER(t->id))
            : NULL;
        const gchar *list_name = ctx->list_names != NULL
            ? g_hash_table_lookup(ctx->list_names,
                                  GINT_TO_POINTER(t->list_id))
            : NULL;
        gint att_count = GPOINTER_TO_INT(
            g_hash_table_lookup(ctx->att_counts, GINT_TO_POINTER(t->id)));

        TaskRow *row = task_row_new();
        row->id          = t->id;
        row->status      = t->status;
        row->title       = g_strdup(*t->title != '\0' ? t->title : "Untitled Task");
        row->markup      = task_rows_desc_markup(t, list_name, att_count,
                                                  subs, ctx);
        row->due         = t->due;
        row->due_time    = t->due_time;
        row->due_instant = task_due_instant(t->due, t->due_time);
        /* The due cell carries its time of day only when that time is not
         * the 08:00 default (task_due_format_at) — every task has one now,
         * so printing it always would put a clock on every row and
         * distinguish nothing.  due_instant is the sort key since `due`
         * alone is local midnight for every row of the same calendar day.  */
        row->due_text       = task_due_format_at(t->due, t->due_time);
        row->completed_at   = t->completed_at;
        row->completed_text = task_due_format(t->completed_at);
        row->status_text    = g_strdup(task_status_label(t->status));

        g_ptr_array_add(items, row);  /* GPtrArray takes ownership          */
    }

    guint added = items->len;

    /* ONE splice replaces whatever was in the store with the new items.   */
    g_list_store_splice(store, 0,
                        g_list_model_get_n_items(G_LIST_MODEL(store)),
                        items->pdata, items->len);
    g_ptr_array_unref(items);
    return added;
}

/* ---------------------------------------------------------------------------
 * task_rows_toggle_done() — the ✓ column's click (see task_rows.h).
 *
 * ONE implementation for every pane that shows a checkbox.  Separate
 * copies differing only in where the row came from are how two of them
 * would eventually disagree about what a tick means.
 * ------------------------------------------------------------------------- */

/*
 * task_rows_toggle_done — apply the done/undone rule for one row and refresh.
 *
 * Ticking means Done; unticking means In Progress (a task that was ticked
 * has plainly been worked on, so dropping it back to New would lose that).
 * New is reachable only from the editor's dropdown.
 *
 * There is no fade-out any more: a completed row that the visibility setting
 * hides disappears with the notify_changed refresh.
 *
 * Inputs:
 *   app — the application context
 *   row — the task row the user clicked
 * Output: none
 */
void
task_rows_toggle_done(TaskApp *app, TaskRow *row)
{
    if (row->id == 0)
        return;                      /* placeholder row, not a task         */

    gboolean was_done = task_row_done(row);

    /* Ticking means Done; unticking means In Progress (not New, because a
     * task that reached Done was worked on and should not silently lose
     * that state).  New is only reachable from the editor's dropdown.     */
    task_db_task_set_status(app->db, row->id,
                            was_done ? TASK_STATUS_IN_PROGRESS
                                     : TASK_STATUS_DONE);

    if (!was_done)
        task_app_status(app,
                        "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 Completed",
                        *row->title != '\0' ? row->title : "Untitled Task");

    task_app_notify_changed(app);
}
