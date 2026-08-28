/* ===========================================================================
 * editor_window.c — the per-task editor window (see editor_window.h)
 * =========================================================================== */

#include "editor_window.h"
#include "task_ui.h"
#include "recur.h"
#include <string.h>
#include <time.h>

/* The Advanced disclosure link's two faces.  The ARROW NAMES THE ACTION,
 * like the View menu's items: ▾ offers to unfold, ▴ to fold away.  The
 * FOLDED face is written where the label is BUILT as well as by the
 * applier, because adv_box is constructed folded and the open path calls
 * an applier only when it reveals — a link whose label was never written
 * is a relief-less button with no text in it, i.e. invisible until the
 * first blind click (reported 2026-08-27 against new tasks, which never
 * have advanced content and so never reveal on open).                      */
#define ADV_LABEL_TO_SHOW "<u>Advanced \xe2\x96\xbe</u>"
#define ADV_LABEL_TO_FOLD "<u>Advanced \xe2\x96\xb4</u>"

/* Columns of the subtasks list store.                                      */
enum {
    SUB_ID = 0,                      /* gint64: subtask id                  */
    SUB_DONE,                        /* gboolean                            */
    SUB_TITLE,                       /* gchar*                              */
    SUB_N_COLS
};

/* Columns of the attachments list store.                                   */
enum {
    ATT_ID = 0,                      /* gint64: attachment id               */
    ATT_PATH,                        /* gchar*: full path                   */
    ATT_NAME,                        /* gchar*: basename shown              */
    ATT_N_COLS
};

/* ---------------------------------------------------------------------------
 * TaskEditor — one open editor window's state.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp        *app;
    gint64        task_id;
    gint64        parent_id;         /* 0 when the task is top-level        */
    GtkWidget    *window;
    GtkWidget    *title_entry;
    GtkWidget    *status_combo;      /* New / In Progress / Done — index
                                      * IS the TaskStatus value           */
    GtkWidget    *pinned_check;
    GtkWidget    *priority_check;
    GtkWidget    *completed_label;   /* "Completed <date>" while the task
                                      * is Done, else empty               */
    GtkWidget    *due_entry;
    GtkTextBuffer *notes_buf;
    GtkListStore *sub_store;         /* NULL for subtask editors            */
    GtkWidget    *sub_view;
    GtkCellEditable *sub_edit;       /* in-place subtask edit in flight,
                                      * else NULL (weak: cleared if the
                                      * editable dies under us)             */
    GtkListStore *att_store;
    GtkWidget    *att_view;
    GtkWidget    *ext_box;           /* contributed sections (task_ui.h)    */

    /* The Recurrence block (recur.h).  The preset combo's active index IS
     * the TaskRecurPreset value, and the unit combo's IS the
     * TaskRecurUnit — both are built by appending their labels in enum
     * order, the same trick the Status combo uses.                        */
    GtkWidget    *recur_combo;       /* Never / Hourly / … / Custom…       */
    GtkWidget    *recur_custom_row;  /* the "Every N …" row, present only
                                      * while Custom… is the preset        */
    GtkWidget    *recur_every_spin;  /* custom: repeat every N …            */
    GtkWidget    *recur_unit_combo;  /* … of THIS unit                      */
    GtkWidget    *recur_time_entry;  /* "HH:MM" — the dated units' time     */
    GtkWidget    *recur_lead_spin;   /* reset this long before it …         */
    GtkWidget    *recur_lead_unit;   /* … in these units                    */
    GtkWidget    *recur_summary;     /* "Next … — resets to New …"          */
    gint64        recur_next;        /* the next occurrence, reseeded on
                                      * every edit to the schedule         */
    gboolean      recur_custom_shown; /* is that row on screen?             */
    gint          recur_custom_h;    /* px the window grew to show it,
                                      * given back when it goes away       */

    GtkWidget    *adv_box;           /* Recurrence + Subtasks + Attachments,
                                      * folded away behind the Advanced
                                      * disclosure                          */
    GtkWidget    *adv_label;         /* the "Advanced ▾" link's label       */
    gboolean      adv_shown;         /* disclosure state                    */
    gint          adv_height;        /* px the window grew when expanding,
                                      * given back on collapse             */
    guint         save_source;       /* pending debounce save, or 0         */
    TaskStatus    status_saved;      /* the status last read or written, so
                                      * a save can tell whether the
                                      * completion stamp can have moved    */
    gboolean      loading;           /* suppress change handlers            */
} TaskEditor;

/* editor_notify() — tell the library something changed.  Editor saves
 * use the LIGHT hook (task pane only): they can never change the
 * sidebar, and the saving editor is itself the source of truth — the
 * full notify would reload every open editor (and re-run the Notes
 * CLI) per autosave.                                                       */
static void
editor_notify(TaskEditor *ed)
{
    task_app_notify_tasks(ed->app);  /* falls back to the full event       */
}

/* editor_due_entry_parse() — the due entry's text as a timestamp, with
 * the mid-typing guard: blank clears (0), a valid date parses, and
 * PARTIAL/invalid text keeps `current` — a debounced save firing while
 * the user is still typing must not wipe the stored date.                  */
static gint64
editor_due_entry_parse(TaskEditor *ed, gint64 current)
{
    gchar *trim = g_strstrip(
        g_strdup(gtk_entry_get_text(GTK_ENTRY(ed->due_entry))));
    gint64 due;                      /* the value to store                  */
    if (*trim == '\0')
        due = 0;
    else {
        gint64 parsed = task_due_parse(trim);
        due = parsed != 0 ? parsed : current;
    }
    g_free(trim);
    return due;
}

/* ===========================================================================
 * The Recurrence block (recur.h).
 *
 * Everything the user can set lives in widgets; recur_next does not — it
 * is bookkeeping, RESEEDED from scratch whenever the schedule is edited
 * (editor_recur_reseed) and otherwise carried through from the row.  That
 * split is why an edit here never has to guess what the pass would do:
 * both sides call task_recur_seed.
 * =========================================================================== */

/* The lead's unit menu, in the order the combo appends them — its active
 * index is an index INTO THIS TABLE.  It is not TaskRecurUnit: a lead of
 * "3 months" is meaningless (the clamp would cut it to under one period
 * anyway), so the menu stops at weeks and the value is stored in the
 * MINUTES the column holds.                                               */
static const struct { const gchar *label; gint minutes; } recur_lead_units[] = {
    { "minutes",     1 },
    { "hours",      60 },
    { "days",     1440 },
    { "weeks",   10080 },
};
#define RECUR_LEAD_UNIT_DAYS 2       /* the fallback, matching the default  */

/* editor_recur_time_parse() — the "HH:MM" entry as minutes past midnight,
 * with the same mid-typing guard the due entry has (editor_due_entry_parse):
 * partial or invalid text keeps `current`, because a debounced save firing
 * while the user is still typing "8:3" must not store 8:03 and move the
 * caret's meaning underneath them.                                         */
static gint
editor_recur_time_parse(TaskEditor *ed, gint current)
{
    const gchar *txt   = gtk_entry_get_text(GTK_ENTRY(ed->recur_time_entry));
    const gchar *colon = strchr(txt, ':');
    if (colon == NULL)
        return current;
    gchar *end = NULL;
    gint64 h = g_ascii_strtoll(txt, &end, 10);
    if (end != colon)
        return current;
    gint64 m = g_ascii_strtoll(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' ||
        h < 0 || h > 23 || m < 0 || m > 59)
        return current;
    return (gint)(h * 60 + m);
}

/* editor_recur_time_set() — write `minutes` into that entry as "HH:MM".    */
static void
editor_recur_time_set(TaskEditor *ed, gint minutes)
{
    if (minutes < 0 || minutes > 23 * 60 + 59)
        minutes = TASK_RECUR_TIME_DEFAULT;
    gchar *txt = g_strdup_printf("%02d:%02d", minutes / 60, minutes % 60);
    gtk_entry_set_text(GTK_ENTRY(ed->recur_time_entry), txt);
    g_free(txt);
}

/* editor_recur_lead_minutes() — the lead spin and its unit combo, folded
 * into the minutes the column stores.                                      */
static gint
editor_recur_lead_minutes(TaskEditor *ed)
{
    gint n = gtk_spin_button_get_value_as_int(
                 GTK_SPIN_BUTTON(ed->recur_lead_spin));
    gint u = gtk_combo_box_get_active(GTK_COMBO_BOX(ed->recur_lead_unit));
    if (u < 0 || u >= (gint)G_N_ELEMENTS(recur_lead_units))
        u = RECUR_LEAD_UNIT_DAYS;
    return n * recur_lead_units[u].minutes;
}

/* editor_recur_lead_set() — the inverse: show `minutes` in the LARGEST
 * unit that divides it evenly, so the stored 7200 comes back as "5 days"
 * rather than "7200 minutes".                                              */
static void
editor_recur_lead_set(TaskEditor *ed, gint minutes)
{
    if (minutes <= 0)
        minutes = TASK_RECUR_LEAD_DEFAULT;
    gint u = 0;
    for (gint i = (gint)G_N_ELEMENTS(recur_lead_units) - 1; i >= 0; i--)
        if (minutes % recur_lead_units[i].minutes == 0) {
            u = i;
            break;
        }
    gtk_combo_box_set_active(GTK_COMBO_BOX(ed->recur_lead_unit), u);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ed->recur_lead_spin),
                              minutes / recur_lead_units[u].minutes);
}

/* ---------------------------------------------------------------------------
 * editor_recur_read() — fill `t`'s schedule fields from the widgets.
 *
 * The preset decides whether the custom spin and unit are consulted at
 * all: task_recur_preset_spec expands a named preset and leaves the
 * outputs ALONE for Custom, which is exactly the branch needed here.
 * recur_next is NOT written — see the block comment above.
 * ------------------------------------------------------------------------- */
static void
editor_recur_read(TaskEditor *ed, Task *t)
{
    gint p = gtk_combo_box_get_active(GTK_COMBO_BOX(ed->recur_combo));
    if (p < 0 || p >= TASK_RECUR_N_PRESETS)
        p = TASK_RECUR_PRESET_NEVER;
    if (!task_recur_preset_spec((TaskRecurPreset)p, &t->recur_interval,
                                &t->recur_unit)) {
        t->recur_interval = gtk_spin_button_get_value_as_int(
                                GTK_SPIN_BUTTON(ed->recur_every_spin));
        gint u = gtk_combo_box_get_active(
                     GTK_COMBO_BOX(ed->recur_unit_combo));
        t->recur_unit = (u >= 0 && u < TASK_RECUR_N_UNITS)
                        ? (TaskRecurUnit)u : TASK_RECUR_DAY;
    }
    t->recur_time = editor_recur_time_parse(ed, TASK_RECUR_TIME_DEFAULT);
    t->recur_lead = editor_recur_lead_minutes(ed);
}

/* editor_recur_task() — the schedule the widgets currently describe, as a
 * bare Task for the recur.h helpers to read.  Its due date comes from the
 * due ENTRY rather than the row, so the summary answers for what is on
 * screen; nothing here owns memory, so it is never task_free'd.            */
static Task
editor_recur_task(TaskEditor *ed)
{
    Task t = { 0 };
    t.due = editor_due_entry_parse(ed, 0);
    editor_recur_read(ed, &t);
    t.recur_next = ed->recur_next;
    return t;
}

/* editor_recur_reseed() — the schedule changed, so the next occurrence is
 * computed afresh.  Through task_recur_seed, the same function the pass
 * uses when recur_next is 0, so the date shown here is the date the pass
 * will act on.                                                             */
static void
editor_recur_reseed(TaskEditor *ed)
{
    Task t = editor_recur_task(ed);
    t.recur_next = 0;                /* a changed schedule starts over      */
    ed->recur_next = task_recur_seed(&t, (gint64)time(NULL));
}

/* ---------------------------------------------------------------------------
 * editor_title_refresh() — window title "Tasks - <task title>".
 * ------------------------------------------------------------------------- */
static void
editor_title_refresh(TaskEditor *ed)
{
    const gchar *t = gtk_entry_get_text(GTK_ENTRY(ed->title_entry));
    gchar *title = g_strdup_printf("Tasks - %s",
                                   *t != '\0' ? t : "Untitled Task");
    gtk_window_set_title(GTK_WINDOW(ed->window), title);
    g_free(title);
}

/* editor_status_get() — the status dropdown's current value.  The combo
 * is built with one row per TaskStatus IN ORDER, so the active index
 * IS the enum value; -1 (nothing active, which only happens if the combo
 * has not been loaded yet) reads as New rather than as a negative
 * status.                                                                  */
static TaskStatus
editor_status_get(TaskEditor *ed)
{
    gint active =
        gtk_combo_box_get_active(GTK_COMBO_BOX(ed->status_combo));
    if (active < 0 || active >= TASK_STATUS_N_VALUES)
        return TASK_STATUS_NEW;
    return (TaskStatus)active;
}

/* ---------------------------------------------------------------------------
 * editor_completed_refresh() — show when `t` was last completed.
 *
 * READ-ONLY, and shown whenever there is a STAMP — not only while the task
 * is Done.  completed_at is stamped on entering Done and nothing clears it
 * (db.h), so it survives someone reopening the task, and that is exactly
 * when it is worth reading: "this was finished on the 27th and is being
 * worked on again" is a fact about the task, not a leftover.  A task whose
 * stamp is 0 — never completed, or a row completed by a writer that
 * predates the column — shows nothing rather than "Jan 1, 1970".
 *
 * The two faces are one spelling each, and the ARROW-NAMES-THE-ACTION rule
 * the View menu follows applied to a statement: say what is true of the
 * CURRENT state.  "Completed 27 Aug" beside a Status reading In Progress
 * is a flat contradiction; "Last completed" is the same fact, told
 * straight.
 *
 * It lives on the flags row and is EMPTY rather than hidden when there is
 * nothing to say: an empty label takes no width, the row's height comes
 * from the checkboxes beside it either way, and there is no show_all to
 * fight (gotcha 15) and no window height to keep honest.
 *
 * The date only, not the time.  The row has the two checkboxes on it and
 * the editor takes its natural width from 490 px, so a string that grows
 * with the locale is one that can silently widen every editor.
 * ------------------------------------------------------------------------- */
#define COMPLETED_LABEL_DONE "Completed"
#define COMPLETED_LABEL_PAST "Last completed"

static void
editor_completed_refresh(TaskEditor *ed, const Task *t)
{
    gchar *when = t != NULL ? task_due_format(t->completed_at)
                            : g_strdup("");
    gchar *markup = *when != '\0'
        ? g_markup_printf_escaped(
              "<small><span alpha=\"65%%\">%s %s</span></small>",
              t->status == TASK_STATUS_DONE ? COMPLETED_LABEL_DONE
                                            : COMPLETED_LABEL_PAST, when)
        : g_strdup("");
    gtk_label_set_markup(GTK_LABEL(ed->completed_label), markup);
    g_free(markup);
    g_free(when);
}

/* ---------------------------------------------------------------------------
 * editor_save_now() — write every editable field through to the row and
 * notify the library.  The debounce timer funnels here.
 *
 * A mirrored Notes item saves exactly like any other task: its status
 * and due land in the database, and the next mirror pass carries them
 * to Notes in bulk (the Notes plugin) — flattened there to the done flag Notes
 * understands.  The editor no longer shells the CLI per
 * keystroke-debounce, which is what made every autosave wait on a
 * process spawn.
 * ------------------------------------------------------------------------- */
static void
editor_save_now(TaskEditor *ed)
{
    if (ed->save_source != 0) {
        g_source_remove(ed->save_source);
        ed->save_source = 0;
    }
    Task *t = task_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL)
        return;
    g_free(t->title);
    g_free(t->notes);
    t->title = g_strdup(gtk_entry_get_text(GTK_ENTRY(ed->title_entry)));
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(ed->notes_buf, &a, &b);
    t->notes = gtk_text_buffer_get_text(ed->notes_buf, &a, &b, FALSE);
    t->status = editor_status_get(ed);
    t->pinned = gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(ed->pinned_check));
    t->priority = gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(ed->priority_check));
    t->due    = editor_due_entry_parse(ed, t->due);
    /* The recurrence schedule rides the same write-through save.
     * recur_next comes off `ed` rather than out of a widget: it is not a
     * setting, and it is reseeded by editor_recur_reseed whenever the
     * schedule the widgets describe actually changes.                    */
    editor_recur_read(ed, t);
    t->recur_next = ed->recur_next;
    task_db_task_update(ed->app->db, t);
    /* Only a STATUS change can move completed_at, and the row is the one
     * that knows where it landed — the stamping rule is an SQL CASE over
     * the old row (db.c), and spelling it a second time here is how the
     * two come to disagree.  So re-read, but only on the change that can
     * matter: every other save is a keystroke on the 600 ms debounce and
     * must not buy a query.                                              */
    gboolean status_moved = t->status != ed->status_saved;
    ed->status_saved = t->status;
    task_free(t);
    if (status_moved) {
        Task *fresh = task_db_task_get(ed->app->db, ed->task_id);
        editor_completed_refresh(ed, fresh);
        task_free(fresh);
    }
    editor_title_refresh(ed);
    editor_notify(ed);
}

/* save_timeout() — the debounce timer body.                                */
static gboolean
save_timeout(gpointer data)
{
    TaskEditor *ed = data;
    ed->save_source = 0;
    editor_save_now(ed);
    return G_SOURCE_REMOVE;
}

/* editor_queue_save() — (re)arm the ~600 ms debounce.                      */
static void
editor_queue_save(TaskEditor *ed)
{
    if (ed->loading)
        return;
    if (ed->save_source != 0)
        g_source_remove(ed->save_source);
    ed->save_source = g_timeout_add(600, save_timeout, ed);
}

/* on_field_changed() — any text/toggle edit → debounce a save.             */
static void
on_field_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    editor_queue_save(data);
}

/* on_toggle_changed() — status/pinned change → save immediately (these
 * drive the library's meta lists).  The status combo shares it: a
 * dropdown pick is a deliberate act like a tick, not something the
 * 600 ms debounce should sit on.                                           */
static void
on_toggle_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    if (!ed->loading)
        editor_save_now(ed);
}

/* ---------------------------------------------------------------------------
 * editor_recur_custom_set() — show or hide the "Every N …" row, growing or
 * shrinking the window by exactly its height.
 *
 * The row is HIDDEN rather than greyed, so the block only ever shows
 * controls that do something.  That costs the bookkeeping below, and the
 * bookkeeping is the whole point: adv_height is what the Advanced fold
 * gives back on the way in, so a row that appears afterwards has to be
 * added to it or a later collapse leaves the window taller than it opened.
 *
 * The RESIZE half only runs for a window already on screen — the same
 * split editor_advanced_reveal and editor_advanced_set make, and for the
 * same reason: on the open path the row's visibility is settled before the
 * window is ever presented, so it is already in the natural height and
 * resizing would be the visible two-step that path exists to avoid.
 *
 * The row carries no_show_all, so neither show_all on adv_box nor the
 * construction-time one on the window can reveal it behind this
 * function's back; the flag is lifted across its own show_all, without
 * which that call would return early and nothing would appear (gotcha 15).
 * ------------------------------------------------------------------------- */
static void
editor_recur_custom_set(TaskEditor *ed, gboolean shown)
{
    if (shown == ed->recur_custom_shown)
        return;                      /* no change, and so no resize         */
    ed->recur_custom_shown = shown;

    gboolean live = gtk_widget_get_visible(ed->window) && ed->adv_shown;
    gint w = 0, h = 0;               /* live client size                    */
    if (live)
        gtk_window_get_size(GTK_WINDOW(ed->window), &w, &h);

    if (shown) {
        gtk_widget_set_no_show_all(ed->recur_custom_row, FALSE);
        gtk_widget_show_all(ed->recur_custom_row);
        gtk_widget_set_no_show_all(ed->recur_custom_row, TRUE);
        gint min, nat;               /* measured AFTER the show             */
        gtk_widget_get_preferred_height(ed->recur_custom_row, &min, &nat);
        ed->recur_custom_h = nat + 4;   /* + the section box's spacing      */
        if (live) {
            ed->adv_height += ed->recur_custom_h;
            gtk_window_resize(GTK_WINDOW(ed->window), w,
                              h + ed->recur_custom_h);
        }
    } else {
        gtk_widget_hide(ed->recur_custom_row);
        if (live && ed->recur_custom_h > 0) {
            ed->adv_height = MAX(ed->adv_height - ed->recur_custom_h, 0);
            gtk_window_resize(GTK_WINDOW(ed->window), w,
                              MAX(h - ed->recur_custom_h, 1));
        }
        ed->recur_custom_h = 0;
    }
}

/* ---------------------------------------------------------------------------
 * editor_recur_refresh() — the Recurrence block's single applier: what is
 * sensitive, and what the summary line says.
 *
 * The custom row comes and goes (editor_recur_custom_set, which keeps the
 * window's height honest); everything else is greyed in place, so a
 * control that does not currently apply still shows what it holds.
 * ------------------------------------------------------------------------- */
static void
editor_recur_refresh(TaskEditor *ed)
{
    gint     p      = gtk_combo_box_get_active(GTK_COMBO_BOX(ed->recur_combo));
    gboolean on     = p > TASK_RECUR_PRESET_NEVER;
    gboolean custom = p == TASK_RECUR_PRESET_CUSTOM;
    Task     t      = editor_recur_task(ed);

    /* The custom row is HIDDEN when it does not apply (it is a whole
     * control that would otherwise sit there doing nothing); the two
     * below are GREYED, because they keep showing the value in force and
     * only stop being editable.                                          */
    editor_recur_custom_set(ed, custom);
    /* The minute and hour units have no time of day to land on — "every
     * 3 hours at 8am" is not a thing anyone can mean.                     */
    gtk_widget_set_sensitive(ed->recur_time_entry,
                             on && t.recur_unit >= TASK_RECUR_DAY);
    gtk_widget_set_sensitive(ed->recur_lead_spin, on);
    gtk_widget_set_sensitive(ed->recur_lead_unit, on);

    gchar *text = task_recur_describe(&t, (gint64)time(NULL));
    /* Dimmed with Pango alpha, never a fixed gray (a gray is unreadable
     * on a dark theme).  Escaped because the sentence carries formatted
     * dates from the C library, not a literal of ours.                   */
    gchar *markup = *text != '\0'
        ? g_markup_printf_escaped(
              "<small><span alpha=\"65%%\">%s</span></small>", text)
        : g_strdup("");
    gtk_label_set_markup(GTK_LABEL(ed->recur_summary), markup);
    g_free(markup);
    g_free(text);
}

/* on_recur_changed() — any Recurrence control moved: reseed the next
 * occurrence, re-apply the block, and debounce a save.
 *
 * The DEBOUNCE rather than an immediate write, unlike the status combo:
 * the time and lead controls are typed into, and a save per keystroke
 * would write a half-entered "8:" through the parse guard on every one.
 * Nothing is lost by waiting — the save is write-through, and
 * on_editor_destroy flushes a pending one.                                 */
static void
on_recur_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    if (ed->loading)
        return;
    editor_recur_reseed(ed);
    editor_recur_refresh(ed);
    editor_queue_save(ed);
}

/* ---------------------------------------------------------------------------
 * The in-flight in-place subtask edit.
 *
 * GTK hands the renderer's editable (a GtkEntry) to "editing-started"; we
 * hold it so Add and Save can COMMIT half-typed text rather than throw the
 * typing out.  The pointer is weak — every path that ends an edit
 * ("edited", "editing-canceled", the widget simply dying) clears it, and
 * on_editor_destroy drops the weak reference before `ed` is freed, since
 * the editable can outlive it (the window's "destroy" handlers run before
 * its children are destroyed).
 *
 * Holding the editable is NOT on its own enough, and the reason is worth
 * writing down: GTK3 treats losing focus as CANCELLING an in-place cell
 * edit, not as finishing it (GtkCellRendererText's own focus-out handler
 * sets the entry's "editing-canceled" and tears the editable down).  A
 * mouse click on Add moves focus on BUTTON-PRESS, so that cancel has
 * already run — and already emitted "editing-canceled", which clears this
 * pointer — by the time "clicked" reaches on_sub_add.  Committing from the
 * button handler therefore found nothing left to commit and the typing was
 * lost anyway.  So on_sub_edit_focus_out below commits FIRST, from the
 * entry's own focus-out (a plain g_signal_connect, which runs ahead of the
 * renderer's g_signal_connect_after one); committing disconnects the
 * renderer's handler, so the cancel never happens.  Escape is unaffected —
 * it cancels through the key-press path with no focus change at all
 * (verified both ways against GTK 3.24).
 * ------------------------------------------------------------------------- */
static gboolean on_sub_edit_focus_out(GtkWidget *entry, GdkEventFocus *event,
                                      gpointer data);

static void
editor_sub_edit_forget(TaskEditor *ed)
{
    if (ed->sub_edit == NULL)
        return;
    /* Disconnect BEFORE dropping the pointer: the handler captures `ed`,
     * and the editable outlives it (a window being destroyed hands focus
     * away as it goes), so a handler left connected is a use-after-free.  */
    g_signal_handlers_disconnect_by_func(ed->sub_edit,
                                         (gpointer)on_sub_edit_focus_out, ed);
    g_object_remove_weak_pointer(G_OBJECT(ed->sub_edit),
                                 (gpointer *)&ed->sub_edit);
    ed->sub_edit = NULL;
}

/* editor_sub_edit_commit() — finish an in-flight edit as if the user had
 * pressed Enter: "editing-done" makes the renderer emit "edited" with the
 * entry's current text (on_sub_title_edited saves it), and remove-widget
 * tears the editable down.  No-op when nothing is being edited.            */
static void
editor_sub_edit_commit(TaskEditor *ed)
{
    if (ed->sub_edit == NULL)
        return;
    GtkCellEditable *e = ed->sub_edit;
    g_object_ref(e);                 /* remove_widget may drop the last ref */
    editor_sub_edit_forget(ed);      /* the callbacks below re-enter here   */
    gtk_cell_editable_editing_done(e);
    gtk_cell_editable_remove_widget(e);
    g_object_unref(e);
}

/* ---------------------------------------------------------------------------
 * on_editor_save() — the Save button every editor carries: flush the
 * write-through save and close.  Saves are already write-through, so this
 * is a "commit now and get out of my way" button rather than the only way
 * to persist.
 * ------------------------------------------------------------------------- */
static void
on_editor_save(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    editor_sub_edit_commit(ed);      /* a subtask still being typed         */
    editor_save_now(ed);             /* also clears the pending debounce    */
    gtk_widget_destroy(ed->window);
}

/* ---------------------------------------------------------------------------
 * on_editor_cancel() — the New Task window's Cancel: close and delete the
 * task the New Task action created (tombstoned with its subtasks, the same
 * path as the library's Delete Task, so the delete syncs).
 *
 * Order matters: drop the pending debounce and destroy the window FIRST,
 * because on_editor_destroy frees `ed` and would otherwise flush a save
 * into the row we are about to tombstone.  The library is notified after
 * that through the app hook — a vanishing task is structural (list counts,
 * the Favorites row), so it takes the FULL refresh, not notify_tasks.
 *
 * A subtask still in its in-place editor is FORGOTTEN, not committed: the
 * whole task is about to be tombstoned, so on_editor_destroy must not go
 * writing that title into a row on its way out.
 * ------------------------------------------------------------------------- */
static void
on_editor_cancel(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    TaskApp  *app = ed->app;           /* `ed` dies with the window below   */
    gint64  id  = ed->task_id;
    if (ed->save_source != 0) {
        g_source_remove(ed->save_source);
        ed->save_source = 0;
    }
    editor_sub_edit_forget(ed);
    gtk_widget_destroy(ed->window);
    task_db_task_delete(app->db, id);
    task_app_notify_changed(app);
    task_app_status(app, "Discarded the new task");
}

/* ---------------------------------------------------------------------------
 * on_due_calendar() — the 📅 button: modal GtkCalendar dialog writing an
 * ISO date into the due entry (Clear empties it).
 * ------------------------------------------------------------------------- */
static void
on_due_calendar(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Due Date",
        GTK_WINDOW(ed->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Clear", GTK_RESPONSE_REJECT, "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *cal = gtk_calendar_new();

    /* Preselect the current due date, if any.                              */
    gint64 due = task_due_parse(gtk_entry_get_text(GTK_ENTRY(ed->due_entry)));
    if (due != 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        gtk_calendar_select_month(GTK_CALENDAR(cal),
                                  (guint)g_date_time_get_month(dt) - 1,
                                  (guint)g_date_time_get_year(dt));
        gtk_calendar_select_day(GTK_CALENDAR(cal),
                                (guint)g_date_time_get_day_of_month(dt));
        g_date_time_unref(dt);
    }
    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
        cal, TRUE, TRUE, 6);
    gtk_widget_show_all(dlg);

    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_OK) {
        guint y, m, d;               /* the picked date                     */
        gtk_calendar_get_date(GTK_CALENDAR(cal), &y, &m, &d);
        gchar *iso = g_strdup_printf("%04u-%02u-%02u", y, m + 1, d);
        gtk_entry_set_text(GTK_ENTRY(ed->due_entry), iso);
        g_free(iso);
        editor_save_now(ed);
    } else if (resp == GTK_RESPONSE_REJECT) {
        gtk_entry_set_text(GTK_ENTRY(ed->due_entry), "");
        editor_save_now(ed);
    }
    gtk_widget_destroy(dlg);
}

/* ===========================================================================
 * Subtasks section (top-level tasks only).
 * =========================================================================== */

/* sub_selected_id() — id of the selected subtask row, or 0.                */
static gint64
sub_selected_id(TaskEditor *ed)
{
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ed->sub_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return 0;
    gint64 id;
    gtk_tree_model_get(model, &iter, SUB_ID, &id, -1);
    return id;
}

/* sub_refresh() — repopulate the subtasks store from the database.         */
static void
sub_refresh(TaskEditor *ed)
{
    if (ed->sub_store == NULL)
        return;
    gtk_list_store_clear(ed->sub_store);
    GPtrArray *subs = task_db_subtasks(ed->app->db, ed->task_id);
    for (guint i = 0; i < subs->len; i++) {
        Task *s = g_ptr_array_index(subs, i);
        GtkTreeIter iter;
        gtk_list_store_append(ed->sub_store, &iter);
        gtk_list_store_set(ed->sub_store, &iter,
                           SUB_ID, s->id,
                           SUB_DONE, s->status == TASK_STATUS_DONE,
                           SUB_TITLE, s->title,
                           -1);
    }
    task_ptr_array_free_tasks(subs);
}

/* on_sub_edit_focus_out() — the entry lost focus (the user clicked Add,
 * Save, another field, another window): SAVE the half-typed title instead
 * of letting GTK's own focus-out handler cancel it.  Returns FALSE so the
 * entry still gets its ordinary focus-out handling.                        */
static gboolean
on_sub_edit_focus_out(GtkWidget *entry, GdkEventFocus *event, gpointer data)
{
    (void)event;
    TaskEditor *ed = data;
    if (ed->sub_edit == (GtkCellEditable *)entry)
        editor_sub_edit_commit(ed);
    return FALSE;
}

/* on_sub_editing_started() — remember the editable GTK just created, and
 * take over its focus-out (see the block comment above).                   */
static void
on_sub_editing_started(GtkCellRenderer *cell, GtkCellEditable *editable,
                       gchar *path_str, gpointer data)
{
    (void)cell;
    (void)path_str;
    TaskEditor *ed = data;
    editor_sub_edit_forget(ed);
    ed->sub_edit = editable;
    g_object_add_weak_pointer(G_OBJECT(editable), (gpointer *)&ed->sub_edit);
    if (GTK_IS_WIDGET(editable))
        g_signal_connect(editable, "focus-out-event",
                         G_CALLBACK(on_sub_edit_focus_out), ed);
}

/* on_sub_editing_canceled() — Escape (or GTK cancelling for us).           */
static void
on_sub_editing_canceled(GtkCellRenderer *cell, gpointer data)
{
    (void)cell;
    editor_sub_edit_forget(data);
}

/* on_sub_add() — create a subtask and start editing its title in place.
 * A title still being typed in another row is committed first, so Add
 * never costs the user what they had just written.  A mouse click has
 * normally already committed it through the focus-out path (see the block
 * comment above), which leaves this call a no-op; it still matters for a
 * keyboard/mnemonic activation that never moves focus.                     */
static void
on_sub_add(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    editor_sub_edit_commit(ed);
    Task *t = task_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL)
        return;
    gint64 id = task_db_task_create(ed->app->db, t->list_id, ed->task_id,
                                    "New subtask");
    task_free(t);
    if (id == 0) {                   /* refused (nesting) or write failed   */
        task_app_status(ed->app, "Could not create the subtask");
        return;
    }
    sub_refresh(ed);
    editor_notify(ed);

    /* Put the fresh row's title straight into edit mode.                   */
    GtkTreeModel *model = GTK_TREE_MODEL(ed->sub_store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gint64 rid;
        gtk_tree_model_get(model, &iter, SUB_ID, &rid, -1);
        if (rid == id) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(ed->sub_view), path,
                gtk_tree_view_get_column(GTK_TREE_VIEW(ed->sub_view), 1),
                TRUE);
            gtk_tree_path_free(path);
            break;
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

/* on_sub_remove() — delete the selected subtask (no confirm — it is one
 * line of text; the delete propagates to Google on the next sync).         */
static void
on_sub_remove(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    gint64 id = sub_selected_id(ed);
    if (id == 0)
        return;
    task_db_task_delete(ed->app->db, id);
    sub_refresh(ed);
    editor_notify(ed);
}

/* on_sub_move() — move the selected subtask up (-1) or down (+1).          */
static void
on_sub_move(GtkWidget *w, gpointer data)
{
    TaskEditor *ed = data;
    gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w),
                                                       "task-direction"));
    gint64 id = sub_selected_id(ed);
    if (id == 0)
        return;
    task_db_subtask_move(ed->app->db, id, direction);
    sub_refresh(ed);
    editor_notify(ed);

    /* Restore selection to the moved row. */
    GtkTreeModel *model = GTK_TREE_MODEL(ed->sub_store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gint64 rid;
        gtk_tree_model_get(model, &iter, SUB_ID, &rid, -1);
        if (rid == id) {
            GtkTreeSelection *sel =
                gtk_tree_view_get_selection(GTK_TREE_VIEW(ed->sub_view));
            gtk_tree_selection_select_iter(sel, &iter);
            break;
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

/*
 * editor_status_resync() — re-read this task's status off the row and put
 * it in the combo, without saving.
 *
 * Inputs:  ed — the editor
 * Output:  none.
 *
 * For the case where something OTHER than this combo moved the status:
 * ticking a subtask promotes its parent New → In Progress in the
 * database, and this editor may BE that parent.  Leaving the combo stale
 * would not merely look wrong — editor_save_now reads the combo and
 * writes it back, so the next debounced save would quietly undo the
 * promotion.
 *
 * `loading` is raised around the set_active for the same reason
 * editor_load raises it: "changed" fires on a programmatic set, and
 * on_toggle_changed would answer it with a save.                          */
static void
editor_status_resync(TaskEditor *ed)
{
    Task *t = task_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL)
        return;
    gboolean was = ed->loading;
    ed->loading = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(ed->status_combo),
        t->status >= 0 && t->status < TASK_STATUS_N_VALUES
            ? (gint)t->status : (gint)TASK_STATUS_NEW);
    ed->loading = was;
    task_free(t);
}

/* on_sub_toggled() — the subtask done checkbox in the list.  Subtasks
 * have a status like any other task and get the same checkbox rule as
 * the task pane's: ticking means Done, unticking means In Progress (a
 * subtask that was ticked HAD been worked on).  There is no per-subtask
 * status dropdown — the row is one line in a compact list — but opening
 * the subtask in its own editor offers the full choice.
 *
 * Completing a subtask also moves the PARENT off New (parent_started in
 * db.c, which every write path folds through), so the combo above is
 * resynced: it is the parent's own status, and a stale one would be
 * written back by the next debounced save.  Unticking is not the mirror
 * of that — see parent_started.                                           */
static void
on_sub_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    TaskEditor *ed = data;
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(ed->sub_store);
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gboolean done;
    gtk_tree_model_get(model, &iter, SUB_ID, &id, SUB_DONE, &done, -1);
    task_db_task_set_status(ed->app->db, id,
                            done ? TASK_STATUS_IN_PROGRESS : TASK_STATUS_DONE);
    gtk_list_store_set(ed->sub_store, &iter, SUB_DONE, !done, -1);
    if (!done)                       /* the tick just COMPLETED it         */
        editor_status_resync(ed);
    editor_notify(ed);
}

/* on_sub_title_edited() — in-place subtask rename.                         */
static void
on_sub_title_edited(GtkCellRendererText *cell, gchar *path_str,
                    gchar *new_text, gpointer data)
{
    (void)cell;
    TaskEditor *ed = data;
    editor_sub_edit_forget(ed);      /* this edit is over                   */
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(ed->sub_store);
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gtk_tree_model_get(model, &iter, SUB_ID, &id, -1);
    Task *t = task_db_task_get(ed->app->db, id);
    if (t == NULL)
        return;
    g_free(t->title);
    t->title = g_strdup(new_text);
    task_db_task_update(ed->app->db, t);
    task_free(t);
    gtk_list_store_set(ed->sub_store, &iter, SUB_TITLE, new_text, -1);
    editor_notify(ed);
}

/* ===========================================================================
 * Attachments section.
 * =========================================================================== */

/* att_refresh() — repopulate the attachments store.                        */
static void
att_refresh(TaskEditor *ed)
{
    gtk_list_store_clear(ed->att_store);
    GPtrArray *atts = task_db_attachments(ed->app->db, ed->task_id);
    for (guint i = 0; i < atts->len; i++) {
        TaskAttachment *a = g_ptr_array_index(atts, i);
        gchar *name = g_path_get_basename(a->path);
        GtkTreeIter iter;
        gtk_list_store_append(ed->att_store, &iter);
        gtk_list_store_set(ed->att_store, &iter,
                           ATT_ID, a->id,
                           ATT_PATH, a->path,
                           ATT_NAME, name,
                           -1);
        g_free(name);
    }
    task_ptr_array_free_attachments(atts);
}

/* att_selected() — id and (optionally) path of the selected row.           */
static gint64
att_selected(TaskEditor *ed, gchar **path_out)
{
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ed->att_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return 0;
    gint64 id;
    gtk_tree_model_get(model, &iter, ATT_ID, &id, -1);
    if (path_out != NULL)
        gtk_tree_model_get(model, &iter, ATT_PATH, path_out, -1);
    return id;
}

/* on_att_add() — file chooser → new attachment row.                        */
static void
on_att_add(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Attach File",
        GTK_WINDOW(ed->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Attach", GTK_RESPONSE_ACCEPT,
        NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path =
            gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path != NULL) {
            task_db_attachment_add(ed->app->db, ed->task_id, path);
            g_free(path);
            att_refresh(ed);
            editor_notify(ed);
        }
    }
    gtk_widget_destroy(dlg);
}

/* on_att_remove() — drop the selected attachment (the file itself is
 * never touched — attachments are references).                             */
static void
on_att_remove(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    gint64 id = att_selected(ed, NULL);
    if (id == 0)
        return;
    task_db_attachment_remove(ed->app->db, id);
    att_refresh(ed);
    editor_notify(ed);
}

/* att_open_path() — hand a path to the platform's default opener.          */
static void
att_open_path(TaskEditor *ed, const gchar *path)
{
    gchar *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri == NULL)
        return;
    GError *gerr = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(ed->window), uri,
                                GDK_CURRENT_TIME, &gerr)) {
        task_app_notice(GTK_WINDOW(ed->window), GTK_MESSAGE_ERROR, NULL,
                        "Cannot open %s: %s", path,
                        gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
    }
    g_free(uri);
}

/* on_att_open() — the Open button.                                         */
static void
on_att_open(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    gchar *path = NULL;
    if (att_selected(ed, &path) != 0 && path != NULL)
        att_open_path(ed, path);
    g_free(path);
}

/* on_att_activated() — double-click a row = open it.                       */
static void
on_att_activated(GtkTreeView *view, GtkTreePath *tp,
                 GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    TaskEditor *ed = data;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, tp))
        return;
    gchar *path = NULL;
    gtk_tree_model_get(model, &iter, ATT_PATH, &path, -1);
    if (path != NULL)
        att_open_path(ed, path);
    g_free(path);
}

/* ===========================================================================
 * Load / lifetime.
 * =========================================================================== */

/* set_entry_if_differs() — rewrite an entry only when the text really
 * changed, so refreshes never move a cursor needlessly.                    */
static void
set_entry_if_differs(GtkWidget *entry, const gchar *text)
{
    if (strcmp(gtk_entry_get_text(GTK_ENTRY(entry)), text) != 0)
        gtk_entry_set_text(GTK_ENTRY(entry), text);
}

/* due_entry_refresh() — show a stored due date in the entry — unless the
 * user is mid-edit: never rewrite the entry while it has focus (the
 * canonical form would replace their half-typed text).                     */
static void
due_entry_refresh(TaskEditor *ed, gint64 due)
{
    if (gtk_widget_has_focus(ed->due_entry))
        return;
    gchar *text = task_due_format_iso(due);
    set_entry_if_differs(ed->due_entry, text);
    g_free(text);
}

/* clear_children() — empty a container.                                    */
static void
clear_children(GtkWidget *box)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
}

/* ---------------------------------------------------------------------------
 * ext_sections_load() — rebuild the contributed sections for `t`.
 *
 * Rebuilt per load rather than built once and refilled: a section is
 * whatever its owner returns for THIS task, and most tasks get nothing.
 * Asking each contributor and packing what comes back is simpler than
 * keeping a widget per contributor alive and hiding it, and it means a
 * contributor cannot leak state between the tasks it is shown for.
 *
 * A contributor returning NULL is the normal case, not an error.
 * ------------------------------------------------------------------------- */
static void
ext_sections_load(TaskEditor *ed, const Task *t)
{
    clear_children(ed->ext_box);
    gboolean any = FALSE;
    for (guint i = 0; i < task_ui_editor_count(); i++) {
        const TaskUiEditorDef *d = task_ui_editor_nth(i);
        GtkWidget *w = d->build != NULL
                     ? d->build(ed->app, t, d->user_data) : NULL;
        if (w == NULL)
            continue;
        gtk_box_pack_start(GTK_BOX(ed->ext_box), w, FALSE, FALSE, 0);
        any = TRUE;
    }
    if (any)
        gtk_widget_show_all(ed->ext_box);
    else
        gtk_widget_hide(ed->ext_box);
}

/* ---------------------------------------------------------------------------
 * editor_load() — (re)load every widget from the database row.  Returns
 * FALSE when the row/item vanished and the window was destroyed — `ed`
 * must not be touched afterwards.
 * ------------------------------------------------------------------------- */
static gboolean
editor_load(TaskEditor *ed)
{
    Task *t = task_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL || t->deleted) {
        task_free(t);
        gtk_widget_destroy(ed->window);
        return FALSE;
    }
    ed->loading = TRUE;
    set_entry_if_differs(ed->title_entry, t->title);
    /* The combo's rows are the TaskStatus values in order, so the enum
     * value doubles as the active index.  An out-of-range status off
     * disk would leave the combo blank, so it clamps to New.               */
    gtk_combo_box_set_active(GTK_COMBO_BOX(ed->status_combo),
        t->status >= 0 && t->status < TASK_STATUS_N_VALUES
            ? (gint)t->status : (gint)TASK_STATUS_NEW);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->pinned_check),
                                 t->pinned);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->priority_check),
                                 t->priority);
    due_entry_refresh(ed, t->due);
    ed->status_saved = t->status;
    editor_completed_refresh(ed, t);

    /* The recurrence schedule.  The preset combo is set from the (interval,
     * unit) pair rather than stored separately — task_recur_preset_of is
     * the inverse of the expansion editor_recur_read does, so a schedule
     * saved as Custom that happens to be "every 1 week" comes back reading
     * Weekly, which is what it IS.
     *
     * The custom spin and unit are loaded either way, so switching the
     * combo to Custom… shows the schedule already in force rather than an
     * arbitrary "every 1 minute".                                          */
    gtk_combo_box_set_active(GTK_COMBO_BOX(ed->recur_combo),
                             (gint)task_recur_preset_of(t));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ed->recur_every_spin),
                              t->recur_interval > 0 ? t->recur_interval : 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ed->recur_unit_combo),
                             t->recur_interval > 0 ? (gint)t->recur_unit
                                                   : (gint)TASK_RECUR_DAY);
    editor_recur_time_set(ed, t->recur_time);
    editor_recur_lead_set(ed, t->recur_lead);
    ed->recur_next = t->recur_next;
    editor_recur_refresh(ed);

    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(ed->notes_buf, &a, &b);
    gchar *cur = gtk_text_buffer_get_text(ed->notes_buf, &a, &b, FALSE);
    if (strcmp(cur, t->notes) != 0)
        gtk_text_buffer_set_text(ed->notes_buf, t->notes, -1);
    g_free(cur);

    sub_refresh(ed);
    att_refresh(ed);
    ext_sections_load(ed, t);
    editor_title_refresh(ed);
    ed->loading = FALSE;
    task_free(t);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * on_editor_destroy() — flush a pending save and unregister.
 *
 * A subtask title still in its in-place editor is committed here too, so
 * closing the window keeps it — the window's own "destroy" handlers run
 * BEFORE its children are destroyed, so the editable and its tree view are
 * both still alive at this point.  The commit also drops the weak pointer,
 * which must not outlive `ed`.  on_editor_cancel forgets the edit instead:
 * that path is tombstoning the task, so there is nothing to save into.
 * ------------------------------------------------------------------------- */
static void
on_editor_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    editor_sub_edit_commit(ed);
    if (ed->save_source != 0)
        editor_save_now(ed);         /* also clears the source              */
    g_hash_table_remove(ed->app->editors, &ed->task_id);
    g_free(ed);
}

/* ---------------------------------------------------------------------------
 * make_list_section() — the shared "label + scrolled tree view + button
 * column" layout of the subtasks and attachments sections: wraps the
 * caller's `view` and `btn_box` under `heading`.  Returns the outer
 * widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
make_list_section(const gchar *heading, GtkWidget *view,
                  GtkWidget *btn_box)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", heading);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(outer), label, FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                        GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(hbox), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), btn_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), hbox, TRUE, TRUE, 0);
    return outer;
}

/* small_button() — a compact labelled button wired to `cb`.                */
static GtkWidget *
small_button(const gchar *label, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

/* ---------------------------------------------------------------------------
 * editor_advanced_set() — fold the Subtasks + Attachments block away (or
 * back) and grow/shrink the window by exactly that block's height, so the
 * rest of the layout never reflows.
 *
 * The measurement is taken AFTER the show, when the block's preferred
 * height is real, and remembered in adv_height so the collapse gives back
 * the same pixels it took (a GtkBox sizes itself from its VISIBLE children
 * only, so re-measuring a folded box would read 0).
 * ------------------------------------------------------------------------- */
static void
editor_advanced_reveal(TaskEditor *ed)
{
    ed->adv_shown = TRUE;
    gtk_label_set_markup(GTK_LABEL(ed->adv_label), ADV_LABEL_TO_FOLD);
    /* Lift no_show_all across the show — with it set, show_all on the box
     * itself returns early and nothing would appear (gotcha 15).           */
    gtk_widget_set_no_show_all(ed->adv_box, FALSE);
    gtk_widget_show_all(ed->adv_box);
    gtk_widget_set_no_show_all(ed->adv_box, TRUE);
    gint min, nat;
    gtk_widget_get_preferred_height(ed->adv_box, &min, &nat);
    ed->adv_height = nat + 8;        /* + the vbox's inter-child spacing    */
}

/* ---------------------------------------------------------------------------
 * editor_advanced_set() — the disclosure applier for a window that is
 * ALREADY ON SCREEN: reveal or fold the block and resize the window by its
 * height, so the collapse gives back exactly the pixels the expand took.
 *
 * The open path does NOT come through here when the block starts expanded
 * — see editor_open_common.  Growing a window that has already been
 * presented is a visible two-step, and that is precisely the stutter the
 * open path must not have.
 * ------------------------------------------------------------------------- */
static void
editor_advanced_set(TaskEditor *ed, gboolean shown)
{
    gint w, h;                       /* live client size                    */
    gtk_window_get_size(GTK_WINDOW(ed->window), &w, &h);
    if (shown) {
        editor_advanced_reveal(ed);
        gtk_window_resize(GTK_WINDOW(ed->window), w, h + ed->adv_height);
    } else {
        ed->adv_shown = FALSE;
        gtk_label_set_markup(GTK_LABEL(ed->adv_label), ADV_LABEL_TO_SHOW);
        gtk_widget_hide(ed->adv_box);
        if (ed->adv_height > 0)
            gtk_window_resize(GTK_WINDOW(ed->window), w,
                              MAX(h - ed->adv_height, 1));
        ed->adv_height = 0;
    }
}

/* on_editor_advanced() — the Advanced link: flip the disclosure.           */
static void
on_editor_advanced(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    editor_advanced_set(ed, !ed->adv_shown);
}

/* editor_has_advanced_content() — does this task already carry a
 * recurrence, subtasks or attachments?  Read off the loaded widgets and
 * stores, so it needs editor_load to have run.  Existing tasks with any of
 * the three open expanded (saving the user a click); new and empty ones
 * open folded.                                                             */
static gboolean
editor_has_advanced_content(TaskEditor *ed)
{
    return gtk_combo_box_get_active(GTK_COMBO_BOX(ed->recur_combo))
               > TASK_RECUR_PRESET_NEVER ||
           (ed->sub_store != NULL &&
            gtk_tree_model_iter_n_children(
                GTK_TREE_MODEL(ed->sub_store), NULL) > 0) ||
           (ed->att_store != NULL &&
            gtk_tree_model_iter_n_children(
                GTK_TREE_MODEL(ed->att_store), NULL) > 0);
}

/* ---------------------------------------------------------------------------
 * editor_open_common() — build an editor window for a task.  Mirrored
 * Notes items are ordinary tasks, so there is no longer a reduced
 * variant: they get notes, subtasks and attachments like anything else.
 *
 * Every editor gets a Save button under the notes box; `is_new` marks the
 * window as the one the New Task action just opened and adds Cancel beside
 * it, meaning "throw the row away again".
 * ------------------------------------------------------------------------- */
static void
editor_open_common(TaskApp *app, gint64 task_id, gboolean is_new)
{
    GtkWindow *existing = g_hash_table_lookup(app->editors, &task_id);
    if (existing != NULL) {
        gtk_window_present(existing);
        return;
    }
    Task *t = task_db_task_get(app->db, task_id);
    if (t == NULL || t->deleted) {
        task_free(t);
        return;
    }

    TaskEditor *ed = g_new0(TaskEditor, 1);
    ed->app       = app;
    ed->task_id   = task_id;
    ed->parent_id = t->parent_id;

    ed->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    /* Height -1 = the layout's NATURAL height, which with the Advanced
     * block folded away (no_show_all, see below) is the 8-line notes box
     * plus the fixed rows — so the notes area opens at the size it was
     * asked for instead of swallowing the slack of a fixed window height.
     * editor_advanced_set adds the block's own height when it opens.       */
    gtk_window_set_default_size(GTK_WINDOW(ed->window), 490, -1);
    gtk_window_set_position(GTK_WINDOW(ed->window), GTK_WIN_POS_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(ed->window), vbox);

    /* Title.                                                               */
    ed->title_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->title_entry),
                                   "Task title");
    g_signal_connect(ed->title_entry, "changed",
                     G_CALLBACK(on_field_changed), ed);
    /* Enter in the title is the Save button: the common case is typing a
     * new task's title and being done with it, and the notes box (a
     * GtkTextView) still takes Enter as a newline.  It is deliberately
     * NOT hooked to Cancel in the New Task variant — Enter must never
     * discard what was just typed.                                         */
    g_signal_connect(ed->title_entry, "activate",
                     G_CALLBACK(on_editor_save), ed);
    gtk_box_pack_start(GTK_BOX(vbox), ed->title_entry, FALSE, FALSE, 0);

    /* Status / Due row, then the flags row beneath it.
     *
     * The status dropdown is where the Done checkbox used to sit, and it
     * is a good deal wider than one.  Two rows, not one: the window asks
     * for 490 px and takes its NATURAL height, so an over-wide row would
     * silently widen every editor, while an extra row costs one row of
     * height and nothing else.                                             */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Status:"),
                       FALSE, FALSE, 0);
    /* One row per TaskStatus, appended IN ENUM ORDER — the active
     * index is read back as the status value (editor_status_get).          */
    ed->status_combo = gtk_combo_box_text_new();
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(ed->status_combo),
            task_status_label((TaskStatus)s));
    gtk_combo_box_set_active(GTK_COMBO_BOX(ed->status_combo),
                             (gint)TASK_STATUS_NEW);
    g_signal_connect(ed->status_combo, "changed",
                     G_CALLBACK(on_toggle_changed), ed);
    gtk_box_pack_start(GTK_BOX(row), ed->status_combo, FALSE, FALSE, 0);

    GtkWidget *due_btn = small_button("\xf0\x9f\x93\x85",
                                      G_CALLBACK(on_due_calendar), ed);
    gtk_widget_set_tooltip_text(due_btn, "Pick a due date");
    gtk_box_pack_end(GTK_BOX(row), due_btn, FALSE, FALSE, 0);
    ed->due_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(ed->due_entry), 12);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->due_entry),
                                   "YYYY-MM-DD");
    g_signal_connect(ed->due_entry, "changed",
                     G_CALLBACK(on_field_changed), ed);
    gtk_box_pack_end(GTK_BOX(row), ed->due_entry, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), gtk_label_new("Due:"),
                     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    /* Favorite / High Priority — the two local-only flags.                 */
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    ed->pinned_check = gtk_check_button_new_with_label("Favorite");
    g_signal_connect(ed->pinned_check, "toggled",
                     G_CALLBACK(on_toggle_changed), ed);
    gtk_box_pack_start(GTK_BOX(flags), ed->pinned_check, FALSE, FALSE, 0);
    ed->priority_check = gtk_check_button_new_with_label("High Priority");
    g_signal_connect(ed->priority_check, "toggled",
                     G_CALLBACK(on_toggle_changed), ed);
    gtk_box_pack_start(GTK_BOX(flags), ed->priority_check,
                       FALSE, FALSE, 0);
    /* The completion date, read-only, at the right of the same row — the
     * space beside two checkboxes was doing nothing, and it costs no
     * height at all.  editor_completed_refresh writes it (empty until the
     * task has been completed at least once).                             */
    ed->completed_label = gtk_label_new(NULL);
    gtk_box_pack_end(GTK_BOX(flags), ed->completed_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), flags, FALSE, FALSE, 0);

    /* Notes.                                                               */
    GtkWidget *notes_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(notes_label), "<b>Notes</b>");
    gtk_widget_set_halign(notes_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), notes_label, FALSE, FALSE, 0);
    GtkWidget *notes_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(notes_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(notes_scroll),
                                        GTK_SHADOW_IN);
    GtkWidget *notes_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(notes_view), GTK_WRAP_WORD);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(notes_view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(notes_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(notes_view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(notes_view), 4);
    ed->notes_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(notes_view));
    g_signal_connect(ed->notes_buf, "changed",
                     G_CALLBACK(on_field_changed), ed);
    gtk_container_add(GTK_CONTAINER(notes_scroll), notes_view);
    /* Eight lines tall, measured by LAYING OUT eight lines in the view's
     * own font and context — not from font metrics, whose ascent+descent
     * omits the line gap Pango adds between lines and would leave the box
     * about half a line short.  Measuring beats hardcoding pixels: the UI
     * font differs per platform and per HiDPI scale.
     *
     * min AND max content height both get the value: the min is the ask,
     * and the max keeps a task with 50 lines of notes from opening a
     * window the height of the screen — the box scrolls instead.  A
     * hand-resized window still stretches it (expand=TRUE below), since
     * max-content-height caps the size REQUEST, not the allocation.        */
    {
        PangoLayout *lay = gtk_widget_create_pango_layout(notes_view,
            "X\nX\nX\nX\nX\nX\nX\nX");
        gint lines_w, lines_h;
        pango_layout_get_pixel_size(lay, &lines_w, &lines_h);
        g_object_unref(lay);
        if (lines_h <= 0)            /* no font resolved yet: sane default  */
            lines_h = 8 * 17;
        /* + the view's 4 px top and bottom margins, + 4 px slack so the
         * caret on the 8th line is not flush against the frame.            */
        gint content = lines_h + 12;
        gtk_scrolled_window_set_min_content_height(
            GTK_SCROLLED_WINDOW(notes_scroll), content);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(notes_scroll), content);
    }
    gtk_box_pack_start(GTK_BOX(vbox), notes_scroll, TRUE, TRUE, 0);

    /* Subtasks and Attachments live inside the Advanced disclosure — both
     * are folded away until the user asks for them (or the task already
     * has some).  no_show_all keeps the construction-time show_all out of
     * the block, which is what makes the window's NATURAL height the
     * folded one; editor_advanced_set lifts the flag across its own
     * show_all (that call would otherwise return early — see gotcha 15).   */
    ed->adv_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_no_show_all(ed->adv_box, TRUE);

    /* Subtasks — only for top-level tasks (no nested subtasks).            */
    if (ed->parent_id == 0) {
        ed->sub_store = gtk_list_store_new(SUB_N_COLS, G_TYPE_INT64,
                                           G_TYPE_BOOLEAN, G_TYPE_STRING);
        ed->sub_view = gtk_tree_view_new_with_model(
            GTK_TREE_MODEL(ed->sub_store));
        g_object_unref(ed->sub_store);
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ed->sub_view),
                                          FALSE);
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(ed->sub_view),
                                        FALSE);

        GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
        g_signal_connect(toggle, "toggled",
                         G_CALLBACK(on_sub_toggled), ed);
        gtk_tree_view_append_column(GTK_TREE_VIEW(ed->sub_view),
            gtk_tree_view_column_new_with_attributes("", toggle,
                "active", SUB_DONE, NULL));
        GtkCellRenderer *text = gtk_cell_renderer_text_new();
        g_object_set(text, "editable", TRUE, NULL);
        g_signal_connect(text, "edited",
                         G_CALLBACK(on_sub_title_edited), ed);
        g_signal_connect(text, "editing-started",
                         G_CALLBACK(on_sub_editing_started), ed);
        g_signal_connect(text, "editing-canceled",
                         G_CALLBACK(on_sub_editing_canceled), ed);
        gtk_tree_view_append_column(GTK_TREE_VIEW(ed->sub_view),
            gtk_tree_view_column_new_with_attributes("Subtask", text,
                "text", SUB_TITLE, NULL));

        GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_pack_start(GTK_BOX(btns),
            small_button("Add", G_CALLBACK(on_sub_add), ed),
            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(btns),
            small_button("Remove", G_CALLBACK(on_sub_remove), ed),
            FALSE, FALSE, 0);
        GtkWidget *move_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        GtkWidget *up_btn   = gtk_button_new_with_label("\xe2\x96\xb2");
        GtkWidget *down_btn = gtk_button_new_with_label("\xe2\x96\xbc");
        g_object_set_data(G_OBJECT(up_btn),   "task-direction",
                          GINT_TO_POINTER(-1));
        g_object_set_data(G_OBJECT(down_btn),  "task-direction",
                          GINT_TO_POINTER(1));
        g_signal_connect(up_btn,   "clicked", G_CALLBACK(on_sub_move), ed);
        g_signal_connect(down_btn, "clicked", G_CALLBACK(on_sub_move), ed);
        gtk_box_pack_start(GTK_BOX(move_box), up_btn,   TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(move_box), down_btn, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(btns), move_box, FALSE, FALSE, 0);
        GtkWidget *sub_section =
            make_list_section("Subtasks", ed->sub_view, btns);
        gtk_box_pack_start(GTK_BOX(ed->adv_box), sub_section,
                           FALSE, FALSE, 0);
    } else {
        Task *parent = task_db_task_get(app->db, ed->parent_id);
        gchar *txt = g_strdup_printf(
            "This is a subtask of \xe2\x80\x9c%s\xe2\x80\x9d "
            "— subtasks cannot have their own subtasks.",
            parent != NULL ? parent->title : "?");
        GtkWidget *note = gtk_label_new(txt);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_widget_set_halign(note, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
        g_free(txt);
        task_free(parent);
    }

    /* Attachments.                                                         */
    ed->att_store = gtk_list_store_new(ATT_N_COLS, G_TYPE_INT64,
                                       G_TYPE_STRING, G_TYPE_STRING);
    ed->att_view = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ed->att_store));
    g_object_unref(ed->att_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ed->att_view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(ed->att_view), FALSE);
    GtkCellRenderer *att_text = gtk_cell_renderer_text_new();
    g_object_set(att_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ed->att_view),
        gtk_tree_view_column_new_with_attributes("File", att_text,
            "text", ATT_NAME, NULL));
    g_signal_connect(ed->att_view, "row-activated",
                     G_CALLBACK(on_att_activated), ed);

    GtkWidget *att_btns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(att_btns),
        small_button("Add…", G_CALLBACK(on_att_add), ed),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(att_btns),
        small_button("Remove", G_CALLBACK(on_att_remove), ed),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(att_btns),
        small_button("Open", G_CALLBACK(on_att_open), ed),
        FALSE, FALSE, 0);
    GtkWidget *att_section =
        make_list_section("Attachments", ed->att_view, att_btns);
    gtk_box_pack_start(GTK_BOX(ed->adv_box), att_section, FALSE, FALSE, 0);

    /* Recurrence, LAST in the block and so at the foot of the window's
     * content, just above the Advanced link that reveals it.  Subtasks and
     * Attachments are what the task CONTAINS and are what someone opening
     * Advanced is usually after; a schedule is set once and then left
     * alone, so it reads better out of their way than in front of them.  */
    {
        GtkWidget *rec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *heading = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(heading), "<b>Recurrence</b>");
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(rec), heading, FALSE, FALSE, 0);

        /* Row 1 — the preset.  One row per TaskRecurPreset, appended IN
         * ENUM ORDER, so the active index IS the preset value (the same
         * arrangement the Status combo has).                              */
        GtkWidget *r1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(r1), gtk_label_new("Repeat:"),
                           FALSE, FALSE, 0);
        ed->recur_combo = gtk_combo_box_text_new();
        for (gint i = 0; i < TASK_RECUR_N_PRESETS; i++)
            gtk_combo_box_text_append_text(
                GTK_COMBO_BOX_TEXT(ed->recur_combo),
                task_recur_preset_label((TaskRecurPreset)i));
        gtk_combo_box_set_active(GTK_COMBO_BOX(ed->recur_combo),
                                 (gint)TASK_RECUR_PRESET_NEVER);
        gtk_widget_set_tooltip_text(ed->recur_combo,
            "How often this task comes back.  A set time before each "
            "repeat, a COMPLETED task is put back to New and its due date "
            "moves to that repeat.");
        gtk_box_pack_start(GTK_BOX(r1), ed->recur_combo, FALSE, FALSE, 0);

        /* … and the time of day the dated repeats land on, on the same
         * row: it belongs with "how often", and the editor is 490 px of
         * natural height where a row of its own would cost real pixels.
         *
         * pack_START, immediately after the combo.  It was pack_end'd to
         * line the entry up with the Due entry two rows above, and that
         * put ~300 px of nothing in the middle of what is ONE SENTENCE —
         * "repeat weekly at 08:00".  A column that splits a phrase in
         * half is not worth the column.                                 */
        ed->recur_time_entry = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(ed->recur_time_entry), 6);
        gtk_entry_set_max_width_chars(GTK_ENTRY(ed->recur_time_entry), 6);
        gtk_entry_set_placeholder_text(GTK_ENTRY(ed->recur_time_entry),
                                       "HH:MM");
        gtk_widget_set_tooltip_text(ed->recur_time_entry,
            "The time of day a daily, weekly or monthly repeat lands on "
            "(24-hour).  Repeats measured in minutes or hours have no "
            "time of day and ignore it.");
        gtk_box_pack_start(GTK_BOX(r1), gtk_label_new("at"),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(r1), ed->recur_time_entry,
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rec), r1, FALSE, FALSE, 0);

        /* Row 2 — the custom schedule, PRESENT only while the preset
         * above is Custom….  no_show_all keeps it out of both show_all
         * passes (the window's and adv_box's), which is what makes it
         * absent from the folded natural height and leaves
         * editor_recur_custom_set the only thing that can reveal it.
         * The unit combo's rows are the TaskRecurUnit values in order, so
         * its active index is the enum value too.                        */
        ed->recur_custom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_no_show_all(ed->recur_custom_row, TRUE);
        GtkWidget *r2 = ed->recur_custom_row;
        gtk_box_pack_start(GTK_BOX(r2), gtk_label_new("Every"),
                           FALSE, FALSE, 0);
        ed->recur_every_spin = gtk_spin_button_new_with_range(1, 999, 1);
        gtk_box_pack_start(GTK_BOX(r2), ed->recur_every_spin,
                           FALSE, FALSE, 0);
        ed->recur_unit_combo = gtk_combo_box_text_new();
        for (gint i = 0; i < TASK_RECUR_N_UNITS; i++)
            gtk_combo_box_text_append_text(
                GTK_COMBO_BOX_TEXT(ed->recur_unit_combo),
                task_recur_unit_label((TaskRecurUnit)i));
        gtk_combo_box_set_active(GTK_COMBO_BOX(ed->recur_unit_combo),
                                 (gint)TASK_RECUR_DAY);
        gtk_box_pack_start(GTK_BOX(r2), ed->recur_unit_combo,
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rec), r2, FALSE, FALSE, 0);

        /* Row 3 — the lead: how long before each repeat a completed task
         * is reset to New.  Five days by default.                         */
        GtkWidget *r3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(r3), gtk_label_new("Reset to New"),
                           FALSE, FALSE, 0);
        ed->recur_lead_spin = gtk_spin_button_new_with_range(0, 999, 1);
        gtk_widget_set_tooltip_text(ed->recur_lead_spin,
            "How far ahead of each repeat a completed task is reopened.  "
            "It is shortened automatically when it would not fit inside "
            "the repeat itself.");
        gtk_box_pack_start(GTK_BOX(r3), ed->recur_lead_spin,
                           FALSE, FALSE, 0);
        ed->recur_lead_unit = gtk_combo_box_text_new();
        for (gsize i = 0; i < G_N_ELEMENTS(recur_lead_units); i++)
            gtk_combo_box_text_append_text(
                GTK_COMBO_BOX_TEXT(ed->recur_lead_unit),
                recur_lead_units[i].label);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ed->recur_lead_unit),
                                 RECUR_LEAD_UNIT_DAYS);
        gtk_box_pack_start(GTK_BOX(r3), ed->recur_lead_unit,
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(r3), gtk_label_new("beforehand"),
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(rec), r3, FALSE, FALSE, 0);

        /* The summary.  Wrapped rather than allowed to widen the window:
         * the editor asks for 490 px and takes its natural height, so a
         * long line here would silently stretch every editor.             */
        ed->recur_summary = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(ed->recur_summary), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(ed->recur_summary), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(ed->recur_summary), 52);
        gtk_box_pack_start(GTK_BOX(rec), ed->recur_summary,
                           FALSE, FALSE, 0);

        /* Wired LAST, so the construction-time set_active calls above
         * cannot fire the handler before every widget it reads exists.
         * (ed->loading also guards it, but only once editor_load runs.)   */
        g_signal_connect(ed->recur_combo, "changed",
                         G_CALLBACK(on_recur_changed), ed);
        g_signal_connect(ed->recur_every_spin, "value-changed",
                         G_CALLBACK(on_recur_changed), ed);
        g_signal_connect(ed->recur_unit_combo, "changed",
                         G_CALLBACK(on_recur_changed), ed);
        g_signal_connect(ed->recur_time_entry, "changed",
                         G_CALLBACK(on_recur_changed), ed);
        g_signal_connect(ed->recur_lead_spin, "value-changed",
                         G_CALLBACK(on_recur_changed), ed);
        g_signal_connect(ed->recur_lead_unit, "changed",
                         G_CALLBACK(on_recur_changed), ed);

        gtk_box_pack_start(GTK_BOX(ed->adv_box), rec, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(vbox), ed->adv_box, FALSE, FALSE, 0);

    /* Contributed sections (see task_ui.h) — an integration's read-only
     * view of this task, such as what a sync knows about it.  Empty and
     * zero-height for a task nothing contributes to, which is most of
     * them, so it costs the editor's natural height nothing.              */
    ed->ext_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), ed->ext_box, FALSE, FALSE, 0);

    /* Bottom row: the Advanced disclosure link at the left, Save at the
     * right (every editor) and Cancel to ITS right in the New Task variant
     * — vbox's 12 px border puts them flush with the notes box's right
     * edge.  The row is packed last so it stays at the foot of the window
     * in both fold states.                                                 */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *adv_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(adv_btn), GTK_RELIEF_NONE);
    ed->adv_label = gtk_label_new(NULL);
    /* The folded face, matching the state adv_box is built in.  The open
     * path calls an applier only when it REVEALS, so without this the link
     * carries no text at all in the folded case — which is every new task
     * and every empty one.                                                 */
    gtk_label_set_markup(GTK_LABEL(ed->adv_label), ADV_LABEL_TO_SHOW);
    gtk_container_add(GTK_CONTAINER(adv_btn), ed->adv_label);
    /* Link-blue + underlined (the markup above and in editor_advanced_set,
     * which owns the arrow direction from then on).                        */
    task_app_widget_add_css(adv_btn,
        "button { color: #1c71d8; padding: 2px 4px; }");
    gtk_widget_set_tooltip_text(adv_btn,
        "Show or hide the Recurrence, Subtasks and Attachments sections");
    g_signal_connect(adv_btn, "clicked",
                     G_CALLBACK(on_editor_advanced), ed);
    gtk_box_pack_start(GTK_BOX(foot), adv_btn, FALSE, FALSE, 0);
    /* pack_end puts the FIRST-packed child rightmost, so Cancel goes in
     * before Save to end up on Save's right.                               */
    if (is_new)
        gtk_box_pack_end(GTK_BOX(foot),
            small_button("Cancel", G_CALLBACK(on_editor_cancel), ed),
            FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot),
        small_button("Save", G_CALLBACK(on_editor_save), ed),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), foot, FALSE, FALSE, 0);

    g_signal_connect(ed->window, "destroy",
                     G_CALLBACK(on_editor_destroy), ed);

    /* Register + load.                                                     */
    gint64 *key = g_new(gint64, 1);
    *key = task_id;
    g_hash_table_insert(app->editors, key, ed->window);
    g_object_set_data(G_OBJECT(ed->window), "task-editor", ed);
    task_free(t);
    /* The Notes load can destroy the window (item gone / CLI
     * failure) — `ed` is freed then, so bail before touching it.           */
    if (!editor_load(ed))
        return;
    /* Fold state, decided once the stores are loaded (editor_load, just
     * above) and applied BEFORE the window is ever shown: a task that
     * already has subtasks or attachments opens expanded so they are on
     * screen without a click; a new (or empty) task opens folded, which is
     * the state adv_box is built in and so needs nothing done to it.
     *
     * Revealing FIRST is what makes the window appear at its final size in
     * ONE step.  It used to show_all and then grow, which asks the window
     * manager to present a folded window and resize it a moment later —
     * two frames, and the second one only lands once the main loop gets
     * back to it.  Off the Kanban board, where the click also restyles
     * cards, that gap was long enough to watch the window scale and then
     * unfold (reported 2026-08-26); from the list it usually beat the
     * first frame, which is why it looked like a board-only problem.  It
     * was neither view's fault — the sequence was wrong for both.
     *
     * The block's height still measures true here: a GtkBox counts only
     * VISIBLE children, and adv_box is visible by the time it is measured
     * (gotcha 15).  Realization is not required for a size request.        */
    if (!is_new && editor_has_advanced_content(ed))
        editor_advanced_reveal(ed);
    gtk_widget_show_all(ed->window);
}

/* ---------------------------------------------------------------------------
 * task_editor_open() / task_editor_open_new() — the public entry points
 * (see header).
 * ------------------------------------------------------------------------- */
void
task_editor_open(TaskApp *app, gint64 task_id)
{
    editor_open_common(app, task_id, FALSE);
}

void
task_editor_open_new(TaskApp *app, gint64 task_id)
{
    editor_open_common(app, task_id, TRUE);
}

/* editor_windows() — every open editor window (new list; g_list_free).     */
static GList *
editor_windows(TaskApp *app)
{
    return g_hash_table_get_values(app->editors);
}

/* ---------------------------------------------------------------------------
 * task_editor_refresh_all() — reload every open editor (see header).
 * ------------------------------------------------------------------------- */
void
task_editor_refresh_all(TaskApp *app)
{
    GList *windows = editor_windows(app);
    for (GList *l = windows; l != NULL; l = l->next) {
        TaskEditor *ed = g_object_get_data(G_OBJECT(l->data), "task-editor");
        if (ed == NULL || ed->save_source != 0)
            continue;                /* mid-edit: their version wins        */
        editor_load(ed);
    }
    g_list_free(windows);
}

/* task_editor_close_all() — destroy every open editor (flushing saves).    */
void
task_editor_close_all(TaskApp *app)
{
    GList *windows = editor_windows(app);
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(windows);
}
