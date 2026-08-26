/* ===========================================================================
 * task_rows.c — the task-row renderer (see task_rows.h)
 * =========================================================================== */

#include "task_rows.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * task_rows_store_new() — a store of the TL_* shape (see task_rows.h).
 * ------------------------------------------------------------------------- */
GtkListStore *
task_rows_store_new(void)
{
    return gtk_list_store_new(TL_N_COLS,
                              G_TYPE_INT64,    /* TL_ID            */
                              G_TYPE_BOOLEAN,  /* TL_DONE          */
                              G_TYPE_STRING,   /* TL_DESC          */
                              G_TYPE_STRING,   /* TL_DUE           */
                              G_TYPE_INT64,    /* TL_DUE_RAW       */
                              G_TYPE_STRING,   /* TL_TITLE         */
                              G_TYPE_STRING,   /* TL_COMPLETED     */
                              G_TYPE_INT64,    /* TL_COMPLETED_RAW */
                              G_TYPE_INT,      /* TL_STATUS        */
                              G_TYPE_STRING);  /* TL_STATUS_TEXT   */
}

/* ---------------------------------------------------------------------------
 * task_rows_stripe_color() / task_rows_bg_func() — see task_rows.h.
 * ------------------------------------------------------------------------- */
const gchar *
task_rows_stripe_color(GtkTreeModel *model, GtkTreeIter *iter)
{
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even = (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    gtk_tree_path_free(path);
    return even ? NULL : ROW_TINT;
}

void
task_rows_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                  GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    (void)data;
    g_object_set(cell, "cell-background",
                 task_rows_stripe_color(model, iter), NULL);
}

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
 * task_desc_markup() — build the Task cell: bold title (struck when
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
                      GPtrArray *subs, gboolean bold)
{
    GString *s = g_string_new(NULL);
    gchar *title = markup_escape_db(
        *t->title != '\0' ? t->title : "Untitled Task");
    const gchar *open  = bold ? "<b>" : "";
    const gchar *close = bold ? "</b>" : "";
    gchar *line = t->status == TASK_STATUS_DONE
        ? g_strdup_printf("%s<s>%s</s>%s", open, title, close)
        : g_strdup_printf("%s%s%s", open, title, close);
    if (t->bn_uid != 0) {            /* mirrored Notes action item        */
        gchar *p = g_strdup_printf("\xe2\x9d\x97  %s", line);
        g_free(line);
        line = p;
    }
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

void
task_row_ctx_clear(TaskRowCtx *ctx)
{
    g_hash_table_destroy(ctx->att_counts);
    g_hash_table_destroy(ctx->subs_by_parent);
    task_ptr_array_free_tasks(ctx->all_subs);
    if (ctx->list_names != NULL)
        g_hash_table_destroy(ctx->list_names);
}

/* append_task_rows() — append `tasks` to `store` through the shared-
 * lookup context, honoring the completed-visibility toggle.  Returns
 * the number of rows actually appended.                                    */
guint
task_rows_append(GtkListStore *store, GPtrArray *tasks,
                 const TaskRowCtx *ctx)
{
    guint appended = 0;              /* rows actually in the pane           */
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
            g_hash_table_lookup(ctx->att_counts,
                                GINT_TO_POINTER(t->id)));
        gchar *desc      = task_rows_desc_markup(t, list_name, att_count, subs,
                                            ctx->bold);
        gchar *due       = task_due_format(t->due);
        gchar *completed = task_due_format(t->completed_at);
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                           TL_ID,            t->id,
                           TL_DONE,          done,
                           TL_DESC,          desc,
                           TL_DUE,           due,
                           TL_DUE_RAW,       t->due,
                           TL_TITLE,         t->title,
                           TL_COMPLETED,     completed,
                           TL_COMPLETED_RAW, t->completed_at,
                           TL_STATUS,        (gint)t->status,
                           TL_STATUS_TEXT,   task_status_label(t->status),
                           -1);
        g_free(desc);
        g_free(due);
        g_free(completed);
        appended++;
    }
    return appended;
}

/* ---------------------------------------------------------------------------
 * Fade-out animation for tasks marked done while completeds are hidden.
 *
 * 20 steps × 50 ms = 1 s.  Each step wraps TL_DESC in a <span alpha="N%">
 * that decrements from 95 → 0.  At step 20 the row is removed and a
 * refresh fires.
 * The context holds a reference to the store and a row reference to the
 * row, so neither a window closing nor another refresh mid-flight can
 * leave this timer writing into freed memory.
 * ------------------------------------------------------------------------- */
#define FADE_STEPS    20
#define FADE_INTERVAL 50   /* ms — 20 × 50 ms = 1 s                         */

typedef struct {
    TaskApp              *app;
    GtkListStore       *store;
    GtkTreeRowReference *row_ref;
    gchar              *orig_desc;  /* TL_DESC value at fade-start          */
    gint                step;
} FadeCtx;

static void
fade_ctx_free(FadeCtx *ctx)
{
    gtk_tree_row_reference_free(ctx->row_ref);
    g_clear_object(&ctx->store);     /* the ref taken in start_fade        */
    g_free(ctx->orig_desc);
    g_free(ctx);
}

/* fade_done() — shared terminal path: decrement the in-flight count and
 * refresh only when the LAST fade finishes.  Refreshing per fade would
 * yank the other fading rows out from under themselves.                   */
static void
fade_done(TaskApp *app)
{
    if (--app->pending_fades <= 0) {
        app->pending_fades = 0;
        task_app_notify_changed(app);
    }
}

static gboolean
fade_step_cb(gpointer data)
{
    FadeCtx *ctx = data;
    ctx->step++;

    /* The context holds a REFERENCE to the store, so the store cannot be
     * freed under this timer.  That replaces the old guard, which asked
     * the library window whether it still owned this store — a question
     * only that window could answer, and one a panel or a plugin has no
     * way to.  A row reference that has gone stale (the pane refreshed
     * beneath us) still reports itself below.                             */
    GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->row_ref);
    if (path == NULL) {              /* row already gone (external refresh) */
        fade_done(ctx->app);
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &iter, path)) {
        gtk_tree_path_free(path);
        fade_done(ctx->app);
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    if (ctx->step >= FADE_STEPS) {   /* fade complete — remove this row     */
        gtk_list_store_remove(ctx->store, &iter);
        gtk_tree_path_free(path);
        fade_done(ctx->app);         /* the refresh fires on the last one   */
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    gtk_tree_path_free(path);

    /* alpha: 95 → 5 across FADE_STEPS steps (step 1 = 95%, step 19 = 5%)  */
    gint alpha = 100 - (ctx->step * 100 / FADE_STEPS);
    gchar *faded = g_strdup_printf("<span alpha=\"%d%%\">%s</span>",
                                   alpha, ctx->orig_desc);
    gtk_list_store_set(ctx->store, &iter, TL_DESC, faded, -1);
    g_free(faded);

    return G_SOURCE_CONTINUE;
}

/* start_fade() — kick off a fade-out for iter in store.  Reads orig_desc
 * from the store, marks the row done (checkbox AND status cell, which
 * the row wears until the refresh removes it), posts a status message,
 * and fires the repeating timer.                                           */
static void
start_fade(TaskApp *app, GtkListStore *store, GtkTreeIter *iter,
           const gchar *title)
{
    gchar *orig_desc = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, TL_DESC, &orig_desc, -1);

    gtk_list_store_set(store, iter,
                       TL_DONE,        TRUE,
                       TL_STATUS,      (gint)TASK_STATUS_DONE,
                       TL_STATUS_TEXT, task_status_label(TASK_STATUS_DONE),
                       -1);

    /* The RAW title: the status bar is a plain-text label (set_text, no
     * markup), so escaping here put a literal "&amp;" on screen for any
     * task with an ampersand in its name.  The fade animation is the only
     * thing that needs markup, and it escapes what it reads back off the
     * label itself.                                                        */
    task_app_status(app,
                    "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 Completed",
                    title != NULL && *title != '\0' ? title : "Untitled Task");

    GtkTreePath *path =
        gtk_tree_model_get_path(GTK_TREE_MODEL(store), iter);
    FadeCtx *ctx   = g_new0(FadeCtx, 1);
    ctx->app       = app;
    ctx->store     = g_object_ref(store);
    ctx->row_ref   = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), path);
    ctx->orig_desc = orig_desc;          /* ownership transferred           */
    ctx->step      = 0;
    gtk_tree_path_free(path);

    app->pending_fades++;
    g_timeout_add(FADE_INTERVAL, fade_step_cb, ctx);
}

/* ---------------------------------------------------------------------------
 * task_rows_toggle_done() — the ✓ column's click (see task_rows.h).
 *
 * ONE implementation for every pane that shows a checkbox: the task
 * pane, the Weekly Forecast's seven day views, and any plugin's.  They
 * were separate copies differing only in where the model came from,
 * which is how two of them would eventually disagree about what a tick
 * means.
 * ------------------------------------------------------------------------- */
void
task_rows_toggle_done(TaskApp *app, GtkListStore *store, GtkTreeIter *iter)
{
    gint64 id;
    gboolean done;
    gchar *title = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter,
                       TL_ID, &id, TL_DONE, &done, TL_TITLE, &title, -1);
    if (id == 0) {                   /* a placeholder row, not a task      */
        g_free(title);
        return;
    }

    /* The checkbox is a VIEW of the status, not a field of its own:
     * ticking means Done, unticking means In Progress — a task that was
     * ticked has plainly been worked on, so dropping it back to New would
     * lose that.  New is reachable only from the editor's dropdown.
     *
     * A mirrored Notes item is written like any other task: the tick
     * lands in the database now and rides out with the next mirror pass,
     * which is what makes that write-back bulk rather than one subprocess
     * per click.                                                          */
    task_db_task_set_status(app->db, id,
                            done ? TASK_STATUS_IN_PROGRESS
                                 : TASK_STATUS_DONE);

    /* Ticking a task while completed ones are hidden would make the row
     * vanish under the pointer; fade it out over a second instead.        */
    if (!done && !task_app_config_get_bool("show_completed", TRUE)) {
        start_fade(app, store, iter, title);
        g_free(title);
        return;
    }
    g_free(title);
    task_app_notify_changed(app);
}
