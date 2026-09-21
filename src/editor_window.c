/* ===========================================================================
 * editor_window.c — the per-task editor window (see editor_window.h)
 *
 * GTK4 port: GtkComboBoxText → GtkDropDown; GtkListStore/GtkTreeView for
 * subtasks and attachments → GtkListBox of rows; button-press-event →
 * GtkGestureClick; focus-out-event → GtkEventControllerFocus; all
 * deprecated container / show_all / no_show_all APIs replaced; calendar
 * picker and file picker are async via task_app_dialog_new /
 * task_app_pick_path.  theme_field_height() and combo_match_fields() are
 * deleted — GTK4 aligns combos and entries without the workaround.
 * =========================================================================== */

#include "editor_window.h"
#include "recur.h"
#include <string.h>
#include <time.h>

/* The Advanced disclosure link's two faces.  Arrow names the action.       */
#define ADV_LABEL_TO_SHOW "<u>Advanced \xe2\x96\xbe</u>"
#define ADV_LABEL_TO_FOLD "<u>Advanced \xe2\x96\xb4</u>"

/* ---------------------------------------------------------------------------
 * TaskEditor — one open editor window's state.
 * ------------------------------------------------------------------------- */
typedef struct {
    TaskApp        *app;
    gint64          task_id;
    gint64          parent_id;        /* 0 when the task is top-level        */
    GtkWidget      *window;
    GtkWidget      *title_entry;
    GtkWidget      *status_combo;     /* GtkDropDown: index IS TaskStatus    */
    GtkWidget      *pinned_check;
    GtkWidget      *priority_check;
    GtkWidget      *completed_label;
    GtkWidget      *due_entry;
    GtkWidget      *due_time_entry;
    GtkTextBuffer  *notes_buf;
    GtkWidget      *sub_box;          /* GtkListBox of subtask rows          */
    GtkWidget      *att_box;          /* GtkListBox of attachment rows       */

    /* Recurrence block.                                                     */
    GtkWidget      *recur_enable;     /* GtkCheckButton master switch        */
    GtkWidget      *recur_body;       /* everything below the switch         */
    GtkWidget      *recur_start_entry;
    GtkWidget      *recur_time_entry;
    GtkWidget      *recur_every_spin;
    GtkWidget      *recur_unit_combo; /* GtkDropDown                        */
    GtkWidget      *recur_lead_spin;
    GtkWidget      *recur_lead_unit;  /* GtkDropDown                        */
    GtkWidget      *recur_summary;
    gint64          recur_next;
    gint            recur_seen_every;
    gint            recur_seen_unit;
    gboolean        recur_body_shown;

    GtkWidget      *adv_box;
    GtkWidget      *adv_label;
    gboolean        adv_shown;
    gint            adv_height;
    guint           save_source;
    TaskStatus      status_saved;
    gboolean        loading;
} TaskEditor;

/* The lead unit combo covers the first FOUR TaskRecurUnit values.          */
#define RECUR_LEAD_N_UNITS 4
static const gint recur_lead_minutes[RECUR_LEAD_N_UNITS] = {
    1, 60, 1440, 10080
};

/* ---------------------------------------------------------------------------
 * Forward declarations for callbacks that reference each other.
 * ------------------------------------------------------------------------- */
static void editor_save_now(TaskEditor *ed);
static void on_recur_changed(GtkWidget *w, gpointer data);
static void editor_status_resync(TaskEditor *ed);

/* ---------------------------------------------------------------------------
 * Helpers.
 * ------------------------------------------------------------------------- */

/*
 * editor_notify — tell the library the task changed (light hook: pane only).
 */
static void
editor_notify(TaskEditor *ed)
{
    task_app_notify_tasks(ed->app);
}

/*
 * editor_due_entry_parse — due entry text as a timestamp, mid-typing safe.
 *
 * Blank clears (0); a valid date parses; partial/invalid keeps `current`.
 */
static gint64
editor_due_entry_parse(TaskEditor *ed, gint64 current)
{
    gchar *trim = g_strstrip(
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ed->due_entry))));
    gint64 due;
    if (*trim == '\0')
        due = 0;
    else {
        gint64 parsed = task_due_parse(trim);
        due = parsed != 0 ? parsed : current;
    }
    g_free(trim);
    return due;
}

/*
 * editor_time_entry_parse — "HH:MM" entry as minutes past midnight.
 *
 * Mid-typing guard: partial or invalid text keeps `current`.
 */
static gint
editor_time_entry_parse(GtkWidget *entry, gint current)
{
    const gchar *txt   = gtk_editable_get_text(GTK_EDITABLE(entry));
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

/*
 * editor_time_entry_set — write `minutes` to "HH:MM" entry unless focused.
 */
static void
editor_time_entry_set(GtkWidget *entry, gint minutes, gint fallback)
{
    if (gtk_widget_has_focus(entry))
        return;
    if (minutes < 0 || minutes > 23 * 60 + 59)
        minutes = fallback;
    gchar *txt = g_strdup_printf("%02d:%02d", minutes / 60, minutes % 60);
    if (strcmp(gtk_editable_get_text(GTK_EDITABLE(entry)), txt) != 0)
        gtk_editable_set_text(GTK_EDITABLE(entry), txt);
    g_free(txt);
}

/*
 * editor_recur_start_parse — start entry text as a timestamp, mid-typing safe.
 */
static gint64
editor_recur_start_parse(TaskEditor *ed, gint64 current)
{
    gchar *trim = g_strstrip(
        g_strdup(gtk_editable_get_text(GTK_EDITABLE(ed->recur_start_entry))));
    gint64 start;
    if (*trim == '\0')
        start = 0;
    else {
        gint64 parsed = task_due_parse(trim);
        start = parsed != 0 ? parsed : current;
    }
    g_free(trim);
    return start;
}

/*
 * editor_recur_start_set — show anchor date unless the entry has focus.
 */
static void
editor_recur_start_set(TaskEditor *ed, gint64 start)
{
    if (gtk_widget_has_focus(ed->recur_start_entry))
        return;
    gchar *text = task_due_format_iso(start);
    if (strcmp(gtk_editable_get_text(GTK_EDITABLE(ed->recur_start_entry)),
               text) != 0)
        gtk_editable_set_text(GTK_EDITABLE(ed->recur_start_entry), text);
    g_free(text);
}

/*
 * editor_recur_lead_minutes — lead spin + unit combo → stored minutes.
 */
static gint
editor_recur_lead_minutes(TaskEditor *ed)
{
    gint  n = gtk_spin_button_get_value_as_int(
                  GTK_SPIN_BUTTON(ed->recur_lead_spin));
    guint u = gtk_drop_down_get_selected(GTK_DROP_DOWN(ed->recur_lead_unit));
    if (u == GTK_INVALID_LIST_POSITION || u >= RECUR_LEAD_N_UNITS)
        u = (guint)TASK_RECUR_DAY;
    return n * recur_lead_minutes[(gint)u];
}

/*
 * editor_recur_lead_set — show `minutes` in the largest exact-dividing unit.
 */
static void
editor_recur_lead_set(TaskEditor *ed, gint minutes)
{
    if (minutes <= 0) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->recur_lead_unit),
                                   (guint)TASK_RECUR_MINUTE);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ed->recur_lead_spin), 0);
        return;
    }
    gint u = 0;
    for (gint i = RECUR_LEAD_N_UNITS - 1; i >= 0; i--)
        if (minutes % recur_lead_minutes[i] == 0) {
            u = i;
            break;
        }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->recur_lead_unit), (guint)u);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ed->recur_lead_spin),
                              minutes / recur_lead_minutes[u]);
}

/*
 * editor_recur_every — interval the widgets describe; 0 when switch is off.
 */
static gint
editor_recur_every(TaskEditor *ed)
{
    if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(ed->recur_enable)))
        return 0;
    return gtk_spin_button_get_value_as_int(
               GTK_SPIN_BUTTON(ed->recur_every_spin));
}

/*
 * editor_recur_read — fill `t`'s schedule fields from the widgets.
 */
static void
editor_recur_read(TaskEditor *ed, Task *t)
{
    t->recur_interval = editor_recur_every(ed);
    guint u = gtk_drop_down_get_selected(GTK_DROP_DOWN(ed->recur_unit_combo));
    t->recur_unit = (u != GTK_INVALID_LIST_POSITION && u < TASK_RECUR_N_UNITS)
                    ? (TaskRecurUnit)u : TASK_RECUR_DAY;
    t->recur_time  = editor_time_entry_parse(ed->recur_time_entry,
                                             TASK_RECUR_TIME_DEFAULT);
    t->recur_start = editor_recur_start_parse(ed, t->recur_start);
    t->recur_lead  = editor_recur_lead_minutes(ed);
}

/*
 * editor_recur_task — schedule as a bare Task for recur.h helpers.
 */
static Task
editor_recur_task(TaskEditor *ed)
{
    Task t = { 0 };
    t.due = editor_due_entry_parse(ed, 0);
    editor_recur_read(ed, &t);
    t.recur_next = ed->recur_next;
    return t;
}

/*
 * editor_recur_reseed — schedule changed; recompute the next occurrence.
 */
static void
editor_recur_reseed(TaskEditor *ed)
{
    Task t = editor_recur_task(ed);
    t.recur_next = 0;
    ed->recur_next = task_recur_seed(&t, (gint64)time(NULL));
}

/* ---------------------------------------------------------------------------
 * Status / title refresh.
 * ------------------------------------------------------------------------- */

/*
 * editor_title_refresh — window title "Tasks - <task title>".
 */
static void
editor_title_refresh(TaskEditor *ed)
{
    const gchar *t = gtk_editable_get_text(GTK_EDITABLE(ed->title_entry));
    gchar *title = g_strdup_printf("Tasks - %s",
                                   *t != '\0' ? t : "Untitled Task");
    gtk_window_set_title(GTK_WINDOW(ed->window), title);
    g_free(title);
}

/*
 * editor_status_get — current status from the dropdown (clamped to New).
 */
static TaskStatus
editor_status_get(TaskEditor *ed)
{
    guint active = gtk_drop_down_get_selected(GTK_DROP_DOWN(ed->status_combo));
    if (active == GTK_INVALID_LIST_POSITION || active >= TASK_STATUS_N_VALUES)
        return TASK_STATUS_NEW;
    return (TaskStatus)active;
}

/* ---------------------------------------------------------------------------
 * Completed-at label.
 * ------------------------------------------------------------------------- */
#define COMPLETED_LABEL_DONE "Completed"
#define COMPLETED_LABEL_PAST "Last completed"

/*
 * editor_completed_refresh — show when `t` was last completed (read-only).
 *
 * Empty when t is NULL or has no stamp.  Never shown as "Jan 1, 1970".
 */
static void
editor_completed_refresh(TaskEditor *ed, const Task *t)
{
    gchar *when = t != NULL ? task_due_format(t->completed_at)
                            : g_strdup("");
    gchar *markup = (t != NULL && *when != '\0')
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
 * Write-through save.
 * ------------------------------------------------------------------------- */

/*
 * editor_save_now — flush every editable field to the row; clear debounce.
 */
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
    t->title = g_strdup(gtk_editable_get_text(GTK_EDITABLE(ed->title_entry)));
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(ed->notes_buf, &a, &b);
    t->notes = gtk_text_buffer_get_text(ed->notes_buf, &a, &b, FALSE);
    t->status   = editor_status_get(ed);
    t->pinned   = gtk_check_button_get_active(
                      GTK_CHECK_BUTTON(ed->pinned_check));
    t->priority = gtk_check_button_get_active(
                      GTK_CHECK_BUTTON(ed->priority_check));
    t->due      = editor_due_entry_parse(ed, t->due);
    t->due_time = editor_time_entry_parse(ed->due_time_entry, t->due_time);
    editor_recur_read(ed, t);
    t->recur_next = ed->recur_next;
    task_db_task_update(ed->app->db, t);
    task_recur_wake_by(ed->app, t->recur_next > 0
                       ? t->recur_next - task_recur_lead_seconds(t) : 0);
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

/*
 * save_timeout — debounce timer body.
 */
static gboolean
save_timeout(gpointer data)
{
    TaskEditor *ed = data;
    ed->save_source = 0;
    editor_save_now(ed);
    return G_SOURCE_REMOVE;
}

/*
 * editor_queue_save — (re)arm the ~600 ms debounce.
 */
static void
editor_queue_save(TaskEditor *ed)
{
    if (ed->loading)
        return;
    if (ed->save_source != 0)
        g_source_remove(ed->save_source);
    ed->save_source = g_timeout_add(600, save_timeout, ed);
}

/* ---------------------------------------------------------------------------
 * Change signal callbacks.
 * ------------------------------------------------------------------------- */

/*
 * on_field_changed — any text/check edit → debounce a save.
 */
static void
on_field_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    editor_queue_save(data);
}

/*
 * on_toggle_changed — status/pin change → immediate save.
 */
static void
on_toggle_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    if (!ed->loading)
        editor_save_now(ed);
}

/*
 * on_status_notify — status GtkDropDown "notify::selected" → immediate save.
 */
static void
on_status_notify(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj; (void)pspec;
    TaskEditor *ed = data;
    if (!ed->loading)
        editor_save_now(ed);
}

/* ---------------------------------------------------------------------------
 * Recurrence helpers.
 * ------------------------------------------------------------------------- */

/*
 * editor_recur_lead_follow — keep the lead on its default while the user
 * has not chosen one, as the repeat period moves.
 */
static void
editor_recur_lead_follow(TaskEditor *ed)
{
    gint  every = editor_recur_every(ed);
    guint u_raw = gtk_drop_down_get_selected(
                      GTK_DROP_DOWN(ed->recur_unit_combo));
    gint  unit  = (u_raw != GTK_INVALID_LIST_POSITION &&
                   u_raw < TASK_RECUR_N_UNITS)
                  ? (gint)u_raw : (gint)TASK_RECUR_DAY;

    if (every == ed->recur_seen_every && unit == ed->recur_seen_unit)
        return;

    gint was = task_recur_lead_default((TaskRecurUnit)ed->recur_seen_unit,
                                       ed->recur_seen_every);
    if (ed->recur_seen_every <= 0 || editor_recur_lead_minutes(ed) == was) {
        gboolean loading = ed->loading;
        ed->loading = TRUE;
        editor_recur_lead_set(ed,
            task_recur_lead_default((TaskRecurUnit)unit, every));
        ed->loading = loading;
    }
    ed->recur_seen_every = every;
    ed->recur_seen_unit  = unit;
}

/*
 * editor_recur_body_set — show or hide the recurrence body, resizing the
 * window by the change in adv_box height.
 *
 * adv_height is RE-MEASURED off adv_box after every change rather than
 * adjusted by the body's own height, because the summary inside it is a
 * wrapped label whose height changes with the width it is given in place.
 */
static void
editor_recur_body_set(TaskEditor *ed, gboolean shown)
{
    if (shown == ed->recur_body_shown)
        return;
    ed->recur_body_shown = shown;

    gboolean live = gtk_widget_get_visible(ed->window) && ed->adv_shown;
    gint h = 0;
    if (live)
        h = gtk_widget_get_height(GTK_WIDGET(ed->window));

    gtk_widget_set_visible(ed->recur_body, shown);

    if (!live)
        return;

    gint min, nat;
    gtk_widget_measure(ed->adv_box, GTK_ORIENTATION_VERTICAL, 490,
                       &min, &nat, NULL, NULL);
    gint was = ed->adv_height;
    ed->adv_height = nat + 8;
    gint delta = ed->adv_height - was;
    if (delta != 0)
        gtk_window_set_default_size(GTK_WINDOW(ed->window), 490,
                                    MAX(h + delta, 100));
}

/*
 * editor_recur_refresh — the Recurrence block's single applier.
 *
 * Sets the summary label and shows/hides the body according to the master
 * switch.  NOTHING IS EVER GREYED OUT — if it is off, the body is gone.
 */
static void
editor_recur_refresh(TaskEditor *ed)
{
    Task t = editor_recur_task(ed);

    gchar *text = task_recur_describe(&t, (gint64)time(NULL));
    gchar *markup = *text != '\0'
        ? g_markup_printf_escaped(
              "<small><span alpha=\"65%%\">%s</span></small>", text)
        : g_strdup("");
    gtk_label_set_markup(GTK_LABEL(ed->recur_summary), markup);
    g_free(markup);
    g_free(text);

    /* LAST — body measurement must see the summary's final text.           */
    editor_recur_body_set(ed,
        gtk_check_button_get_active(GTK_CHECK_BUTTON(ed->recur_enable)));
}

/*
 * recur_changed_impl — common body for all recurrence-control change handlers.
 */
static void
recur_changed_impl(TaskEditor *ed)
{
    if (ed->loading)
        return;
    editor_recur_lead_follow(ed);
    editor_recur_reseed(ed);
    editor_recur_refresh(ed);
    editor_queue_save(ed);
}

/*
 * on_recur_changed — GtkWidget "changed"/"value-changed"/"toggled" for
 * recurrence entries, spinners and the master check button.
 */
static void
on_recur_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    recur_changed_impl(data);
}

/*
 * on_recur_notify — GtkDropDown "notify::selected" for recurrence combos.
 */
static void
on_recur_notify(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)obj; (void)pspec;
    recur_changed_impl(data);
}

/* ---------------------------------------------------------------------------
 * Subtask section.
 *
 * In GTK4 we use a GtkListBox of rows.  Each row is a GtkBox with a
 * GtkCheckButton (for done) and a GtkEntry (for the title, always
 * editable).  Sub_box replaces sub_store + sub_view + sub_edit.
 * ------------------------------------------------------------------------- */

/*
 * sub_row_id — retrieve the task id stored on a subtask row's check button.
 *
 * Each row widget is a GtkBox; its first child is the GtkCheckButton with
 * "task-id" object data.
 */
static gint64
sub_row_id(GtkListBoxRow *lrow)
{
    GtkWidget *box   = gtk_list_box_row_get_child(lrow);
    GtkWidget *check = gtk_widget_get_first_child(box);
    gint64    *id_p  = g_object_get_data(G_OBJECT(check), "task-id");
    return id_p != NULL ? *id_p : 0;
}

/*
 * sub_selected_id — id of the selected subtask row, or 0.
 */
static gint64
sub_selected_id(TaskEditor *ed)
{
    if (ed->sub_box == NULL)
        return 0;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(
                             GTK_LIST_BOX(ed->sub_box));
    return row != NULL ? sub_row_id(row) : 0;
}

/*
 * on_sub_done_toggled — the done checkbox on a subtask row was clicked.
 */
static void
on_sub_done_toggled(GtkCheckButton *btn, gpointer data)
{
    TaskEditor *ed     = data;
    gint64     *id_ptr = g_object_get_data(G_OBJECT(btn), "task-id");
    if (id_ptr == NULL)
        return;
    gboolean now_done = gtk_check_button_get_active(btn);
    task_db_task_set_status(ed->app->db, *id_ptr,
        now_done ? TASK_STATUS_DONE : TASK_STATUS_IN_PROGRESS);
    /* Completing a subtask may promote the parent New → In Progress.       */
    if (now_done)
        editor_status_resync(ed);
    editor_notify(ed);
}

/*
 * sub_save_entry — save a subtask title entry to the database.
 */
static void
sub_save_entry(GtkWidget *entry, TaskEditor *ed)
{
    gint64 *id_ptr = g_object_get_data(G_OBJECT(entry), "task-id");
    if (id_ptr == NULL)
        return;
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    Task *t = task_db_task_get(ed->app->db, *id_ptr);
    if (t == NULL)
        return;
    if (strcmp(t->title, text) != 0) {
        g_free(t->title);
        t->title = g_strdup(text);
        task_db_task_update(ed->app->db, t);
        editor_notify(ed);
    }
    task_free(t);
}

/*
 * on_sub_entry_activate — Enter in a subtask entry saves the title.
 */
static void
on_sub_entry_activate(GtkEntry *entry, gpointer data)
{
    sub_save_entry(GTK_WIDGET(entry), data);
}

/*
 * on_sub_entry_focus_leave — focus lost saves the subtask title.
 */
static void
on_sub_entry_focus_leave(GtkEventControllerFocus *ctl, gpointer data)
{
    (void)ctl;
    GtkWidget *entry = gtk_event_controller_get_widget(
                           GTK_EVENT_CONTROLLER(ctl));
    sub_save_entry(entry, data);
}

/*
 * sub_refresh — repopulate the subtask GtkListBox from the database.
 */
static void
sub_refresh(TaskEditor *ed)
{
    if (ed->sub_box == NULL)
        return;

    /* Remove all existing rows.                                            */
    GtkListBoxRow *row;
    while ((row = gtk_list_box_get_row_at_index(
                      GTK_LIST_BOX(ed->sub_box), 0)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(ed->sub_box), GTK_WIDGET(row));

    GPtrArray *subs = task_db_subtasks(ed->app->db, ed->task_id);
    for (guint i = 0; i < subs->len; i++) {
        Task *s = g_ptr_array_index(subs, i);

        /* Row: [CheckButton] [Entry(title)]                               */
        GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check),
                                    s->status == TASK_STATUS_DONE);
        /* Store the id on the check button (boxed, safe for 64-bit ids).  */
        gint64 *id_ptr = g_new(gint64, 1);
        *id_ptr = s->id;
        g_object_set_data_full(G_OBJECT(check), "task-id", id_ptr, g_free);
        g_signal_connect(check, "toggled",
                         G_CALLBACK(on_sub_done_toggled), ed);
        gtk_box_append(GTK_BOX(r), check);

        GtkWidget *entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), s->title);
        gtk_widget_set_hexpand(entry, TRUE);
        /* Store the id on the entry too so focus-leave can find it.       */
        gint64 *eid = g_new(gint64, 1);
        *eid = s->id;
        g_object_set_data_full(G_OBJECT(entry), "task-id", eid, g_free);
        g_signal_connect(entry, "activate",
                         G_CALLBACK(on_sub_entry_activate), ed);
        GtkEventController *fc = gtk_event_controller_focus_new();
        g_signal_connect(fc, "leave",
                         G_CALLBACK(on_sub_entry_focus_leave), ed);
        gtk_widget_add_controller(entry, fc);
        gtk_box_append(GTK_BOX(r), entry);

        gtk_list_box_append(GTK_LIST_BOX(ed->sub_box), r);
    }
    task_ptr_array_free_tasks(subs);
}

/*
 * on_sub_add — create a subtask and focus its title entry.
 */
static void
on_sub_add(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    Task *t = task_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL)
        return;
    gint64 id = task_db_task_create(ed->app->db, t->list_id, ed->task_id,
                                    "New subtask");
    task_free(t);
    if (id == 0) {
        task_app_status(ed->app, "Could not create the subtask");
        return;
    }
    sub_refresh(ed);
    editor_notify(ed);

    /* Focus the new row's entry.                                           */
    GtkListBoxRow *row;
    for (gint i = 0;
         (row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ed->sub_box),
                                              i)) != NULL;
         i++) {
        if (sub_row_id(row) == id) {
            GtkWidget *box   = gtk_list_box_row_get_child(row);
            GtkWidget *check = gtk_widget_get_first_child(box);
            GtkWidget *entry = gtk_widget_get_next_sibling(check);
            if (entry != NULL)
                gtk_widget_grab_focus(entry);
            break;
        }
    }
}

/*
 * on_sub_remove — delete the selected subtask.
 */
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

/*
 * on_sub_move — move the selected subtask up (-1) or down (+1).
 */
static void
on_sub_move(GtkWidget *w, gpointer data)
{
    TaskEditor *ed    = data;
    gint direction    = GPOINTER_TO_INT(
                            g_object_get_data(G_OBJECT(w), "task-direction"));
    gint64 id = sub_selected_id(ed);
    if (id == 0)
        return;
    task_db_subtask_move(ed->app->db, id, direction);
    sub_refresh(ed);
    editor_notify(ed);

    /* Reselect the moved row.                                              */
    GtkListBoxRow *row;
    for (gint i = 0;
         (row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(ed->sub_box),
                                              i)) != NULL;
         i++) {
        if (sub_row_id(row) == id) {
            gtk_list_box_select_row(GTK_LIST_BOX(ed->sub_box), row);
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Attachments section.
 *
 * GtkListBox of rows; each row is a GtkLabel (name).  Add / Remove / Open
 * are separate buttons.
 * ------------------------------------------------------------------------- */

/*
 * att_row_at — retrieve the id and optionally the path from a list box row.
 */
static gint64
att_row_id_path(GtkListBoxRow *lrow, gchar **path_out)
{
    gint64 *id_ptr = g_object_get_data(G_OBJECT(lrow), "task-id");
    if (path_out != NULL) {
        gchar *p = g_object_get_data(G_OBJECT(lrow), "task-path");
        *path_out = p != NULL ? g_strdup(p) : NULL;
    }
    return id_ptr != NULL ? *id_ptr : 0;
}

/*
 * att_selected — id and optionally path of the selected attachment row.
 */
static gint64
att_selected(TaskEditor *ed, gchar **path_out)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(
                             GTK_LIST_BOX(ed->att_box));
    if (row == NULL) {
        if (path_out != NULL)
            *path_out = NULL;
        return 0;
    }
    return att_row_id_path(row, path_out);
}

/*
 * att_refresh — repopulate the attachments GtkListBox from the database.
 */
static void
att_refresh(TaskEditor *ed)
{
    GtkListBoxRow *row;
    while ((row = gtk_list_box_get_row_at_index(
                      GTK_LIST_BOX(ed->att_box), 0)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(ed->att_box), GTK_WIDGET(row));

    GPtrArray *atts = task_db_attachments(ed->app->db, ed->task_id);
    for (guint i = 0; i < atts->len; i++) {
        TaskAttachment *a = g_ptr_array_index(atts, i);
        gchar *name  = g_path_get_basename(a->path);
        GtkWidget *label = gtk_label_new(name);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        g_free(name);

        gtk_list_box_append(GTK_LIST_BOX(ed->att_box), label);
        /* gtk_list_box_append wraps label in a GtkListBoxRow automatically.*/
        GtkListBoxRow *lrow = gtk_list_box_get_row_at_index(
                                  GTK_LIST_BOX(ed->att_box), (gint)i);
        gint64 *id_ptr = g_new(gint64, 1);
        *id_ptr = a->id;
        g_object_set_data_full(G_OBJECT(lrow), "task-id", id_ptr, g_free);
        g_object_set_data_full(G_OBJECT(lrow), "task-path",
                               g_strdup(a->path), g_free);
    }
    task_ptr_array_free_attachments(atts);
}

/*
 * att_open_path — hand a filesystem path to the platform's default opener.
 */
static void
att_open_path(TaskEditor *ed, const gchar *path)
{
    gchar *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri == NULL)
        return;
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(ed->window),
                            NULL, NULL, NULL);
    g_object_unref(launcher);
    g_free(uri);
}

/*
 * on_att_row_activated — double-click on an attachment row opens it.
 */
static void
on_att_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    (void)box;
    TaskEditor *ed = data;
    gchar *p = NULL;
    att_row_id_path(row, &p);
    if (p != NULL) {
        att_open_path(ed, p);
        g_free(p);
    }
}

/* Async context for task_app_pick_path (attachment add).                   */
typedef struct {
    TaskEditor *ed;
} AttAddCtx;

/*
 * on_att_add_done — completion callback for the attachment file picker.
 */
static void
on_att_add_done(gchar *path, gpointer user_data)
{
    AttAddCtx *ctx = user_data;
    if (path != NULL) {
        task_db_attachment_add(ctx->ed->app->db, ctx->ed->task_id, path);
        g_free(path);
        att_refresh(ctx->ed);
        editor_notify(ctx->ed);
    }
    g_free(ctx);
}

/*
 * on_att_add — open an async file picker and add the chosen file.
 */
static void
on_att_add(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed  = data;
    AttAddCtx  *ctx = g_new0(AttAddCtx, 1);
    ctx->ed = ed;
    task_app_pick_path(GTK_WINDOW(ed->window), "Attach File",
                       TASK_PICK_OPEN, "Attach",
                       NULL, NULL, NULL,
                       on_att_add_done, ctx);
}

/*
 * on_att_remove — drop the selected attachment record (file untouched).
 */
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

/*
 * on_att_open — open the selected attachment with the platform opener.
 */
static void
on_att_open(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed   = data;
    gchar      *path = NULL;
    if (att_selected(ed, &path) != 0 && path != NULL)
        att_open_path(ed, path);
    g_free(path);
}

/* ---------------------------------------------------------------------------
 * Date picker (async, via task_app_dialog_new + GtkCalendar).
 * ------------------------------------------------------------------------- */

/* Context passed to the async date-picker completion.                      */
typedef struct {
    TaskEditor *ed;
    GtkWidget  *entry;        /* the date entry to update                   */
    GtkWidget  *calendar;     /* GtkCalendar inside the dialog              */
    GtkWidget  *clear_check;  /* "Clear date" checkbox                      */
    gboolean    is_recur;     /* TRUE → fire on_recur_changed, else save_now */
} PickDateCtx;

/*
 * on_pick_date_done — completion callback for the calendar dialog.
 *
 * Accepted with clear_check → empty the entry; accepted without → write ISO.
 */
static void
on_pick_date_done(gboolean accepted, GtkWindow *dialog, gpointer user_data)
{
    (void)dialog;
    PickDateCtx *ctx = user_data;
    if (accepted) {
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->clear_check))) {
            gtk_editable_set_text(GTK_EDITABLE(ctx->entry), "");
        } else {
            GDateTime *dt = gtk_calendar_get_date(
                                GTK_CALENDAR(ctx->calendar));
            gchar *iso = g_strdup_printf("%04d-%02d-%02d",
                g_date_time_get_year(dt),
                g_date_time_get_month(dt),
                g_date_time_get_day_of_month(dt));
            g_date_time_unref(dt);
            gtk_editable_set_text(GTK_EDITABLE(ctx->entry), iso);
            g_free(iso);
        }
        if (ctx->is_recur)
            on_recur_changed(NULL, ctx->ed);
        else
            editor_save_now(ctx->ed);
    }
    g_free(ctx);
}

/*
 * editor_pick_date — open an async calendar dialog for `entry`.
 *
 * is_recur TRUE → on_recur_changed fires on OK, else editor_save_now.
 */
static void
editor_pick_date(TaskEditor *ed, GtkWidget *entry, const gchar *title,
                 gboolean is_recur)
{
    PickDateCtx *ctx = g_new0(PickDateCtx, 1);
    ctx->ed       = ed;
    ctx->entry    = entry;
    ctx->is_recur = is_recur;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    ctx->calendar = gtk_calendar_new();

    /* Preselect what the entry currently shows.                            */
    const gchar *cur_text =
        gtk_editable_get_text(GTK_EDITABLE(entry));
    gint64 cur = task_due_parse(cur_text);
    if (cur != 0) {
        GDateTime *dt = task_local_dt(cur);
        gtk_calendar_set_date(GTK_CALENDAR(ctx->calendar), dt);
        g_date_time_unref(dt);
    }
    gtk_box_append(GTK_BOX(vbox), ctx->calendar);

    ctx->clear_check = gtk_check_button_new_with_label("Clear date");
    gtk_box_append(GTK_BOX(vbox), ctx->clear_check);

    task_app_dialog_new(GTK_WINDOW(ed->window), title, vbox, "OK",
                        on_pick_date_done, ctx);
}

/* ---------------------------------------------------------------------------
 * Due-entry and recur-start-entry click → open calendar picker.
 *
 * GtkGestureClick replaces button-press-event.  Only primary button.
 * ------------------------------------------------------------------------- */

/*
 * on_due_entry_press — primary click on the due entry opens the calendar.
 */
static void
on_due_entry_press(GtkGestureClick *gesture, gint n_press,
                   gdouble x, gdouble y, gpointer data)
{
    (void)n_press; (void)x; (void)y;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    TaskEditor *ed = data;
    editor_pick_date(ed, ed->due_entry, "Due Date", FALSE);
}

/*
 * on_recur_start_press — primary click on the start entry opens the calendar.
 */
static void
on_recur_start_press(GtkGestureClick *gesture, gint n_press,
                     gdouble x, gdouble y, gpointer data)
{
    (void)n_press; (void)x; (void)y;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    TaskEditor *ed = data;
    editor_pick_date(ed, ed->recur_start_entry, "Start Date", TRUE);
}

/* ---------------------------------------------------------------------------
 * Load / lifetime.
 * ------------------------------------------------------------------------- */

/*
 * set_entry_if_differs — update an entry only when the text actually changed.
 */
static void
set_entry_if_differs(GtkWidget *entry, const gchar *text)
{
    if (strcmp(gtk_editable_get_text(GTK_EDITABLE(entry)), text) != 0)
        gtk_editable_set_text(GTK_EDITABLE(entry), text);
}

/*
 * due_entry_refresh — show a stored due date; skip if the entry has focus.
 */
static void
due_entry_refresh(TaskEditor *ed, gint64 due)
{
    if (gtk_widget_has_focus(ed->due_entry))
        return;
    gchar *text = task_due_format_iso(due);
    set_entry_if_differs(ed->due_entry, text);
    g_free(text);
}

/*
 * editor_status_resync — re-read the task's status and update the dropdown.
 *
 * Called when something OTHER than the combo moved the status (e.g. a
 * subtask completion promoting the parent New → In Progress).  Guarded
 * with loading so the "notify::selected" handler does not fire a save.
 */
static void
editor_status_resync(TaskEditor *ed)
{
    Task *t = task_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL)
        return;
    gboolean was = ed->loading;
    ed->loading = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->status_combo),
        t->status >= 0 && t->status < TASK_STATUS_N_VALUES
            ? (guint)t->status : (guint)TASK_STATUS_NEW);
    ed->loading = was;
    ed->status_saved = t->status;
    task_free(t);
}

/*
 * editor_load — (re)load every widget from the database row.
 *
 * Returns FALSE when the row vanished and the window was destroyed; `ed`
 * must not be touched afterwards.
 */
static gboolean
editor_load(TaskEditor *ed)
{
    Task *t = task_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL || t->deleted) {
        task_free(t);
        gtk_window_destroy(GTK_WINDOW(ed->window));
        return FALSE;
    }
    ed->loading = TRUE;
    set_entry_if_differs(ed->title_entry, t->title);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->status_combo),
        t->status >= 0 && t->status < TASK_STATUS_N_VALUES
            ? (guint)t->status : (guint)TASK_STATUS_NEW);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ed->pinned_check),
                                t->pinned);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(ed->priority_check),
                                t->priority);
    due_entry_refresh(ed, t->due);
    editor_time_entry_set(ed->due_time_entry, t->due_time,
                          TASK_DUE_TIME_DEFAULT);
    ed->status_saved = t->status;
    editor_completed_refresh(ed, t);

    gtk_check_button_set_active(GTK_CHECK_BUTTON(ed->recur_enable),
                                t->recur_interval > 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ed->recur_every_spin),
                              t->recur_interval > 0 ? t->recur_interval : 1);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->recur_unit_combo),
                               t->recur_interval > 0
                                   ? (guint)t->recur_unit
                                   : (guint)TASK_RECUR_DAY);
    editor_time_entry_set(ed->recur_time_entry, t->recur_time,
                          TASK_RECUR_TIME_DEFAULT);
    editor_recur_start_set(ed, t->recur_start);
    editor_recur_lead_set(ed, t->recur_lead);
    ed->recur_seen_every = t->recur_interval > 0 ? t->recur_interval : 0;
    ed->recur_seen_unit  = t->recur_interval > 0 ? (gint)t->recur_unit
                                                  : (gint)TASK_RECUR_DAY;
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
    editor_title_refresh(ed);
    ed->loading = FALSE;
    task_free(t);
    return TRUE;
}

/*
 * on_editor_destroy — flush a pending save and unregister.
 */
static void
on_editor_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    if (ed->save_source != 0)
        editor_save_now(ed);
    g_hash_table_remove(ed->app->editors, &ed->task_id);
    g_free(ed);
}

/* ---------------------------------------------------------------------------
 * Button / action callbacks.
 * ------------------------------------------------------------------------- */

/*
 * on_editor_save — flush the write-through save and close.
 */
static void
on_editor_save(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    editor_save_now(ed);
    gtk_window_destroy(GTK_WINDOW(ed->window));
}

/*
 * on_editor_cancel — New Task "Cancel": close and tombstone the new task.
 *
 * Destroy the window FIRST so on_editor_destroy does not flush a save into
 * the row about to be tombstoned.
 */
static void
on_editor_cancel(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed  = data;
    TaskApp    *app = ed->app;
    gint64      id  = ed->task_id;
    if (ed->save_source != 0) {
        g_source_remove(ed->save_source);
        ed->save_source = 0;
    }
    gtk_window_destroy(GTK_WINDOW(ed->window));
    task_db_task_delete(app->db, id);
    task_app_notify_changed(app);
    task_app_status(app, "Discarded the new task");
}

/*
 * on_editor_advanced — flip the Advanced disclosure block.
 */
static void
on_editor_advanced(GtkWidget *w, gpointer data)
{
    (void)w;
    TaskEditor *ed = data;
    /* Forward declaration needed; implemented below.                       */
    extern void editor_advanced_set(TaskEditor *ed, gboolean shown);
    editor_advanced_set(ed, !ed->adv_shown);
}

/* ---------------------------------------------------------------------------
 * Advanced disclosure.
 * ------------------------------------------------------------------------- */

/*
 * editor_has_advanced_content — does the task already carry recurrence,
 * subtasks or attachments?  Read off the loaded widgets; run after
 * editor_load.
 */
static gboolean
editor_has_advanced_content(TaskEditor *ed)
{
    return gtk_check_button_get_active(GTK_CHECK_BUTTON(ed->recur_enable)) ||
           (ed->sub_box != NULL &&
            gtk_list_box_get_row_at_index(
                GTK_LIST_BOX(ed->sub_box), 0) != NULL) ||
           (ed->att_box != NULL &&
            gtk_list_box_get_row_at_index(
                GTK_LIST_BOX(ed->att_box), 0) != NULL);
}

/*
 * editor_advanced_reveal — show adv_box and record its height.
 *
 * Does NOT resize the window.  Called on the open path before the window
 * is presented (so the window appears at its final size in one step) and
 * from editor_advanced_set for a window already on screen.
 */
static void
editor_advanced_reveal(TaskEditor *ed)
{
    ed->adv_shown = TRUE;
    gtk_label_set_markup(GTK_LABEL(ed->adv_label), ADV_LABEL_TO_FOLD);
    gtk_widget_set_visible(ed->adv_box, TRUE);
    gint min, nat;
    gtk_widget_measure(ed->adv_box, GTK_ORIENTATION_VERTICAL, 490,
                       &min, &nat, NULL, NULL);
    ed->adv_height = nat + 8;
}

/*
 * editor_advanced_set — fold or unfold the block for a window on screen,
 * resizing the window by the block's height.
 */
void
editor_advanced_set(TaskEditor *ed, gboolean shown)
{
    gint h = gtk_widget_get_height(GTK_WIDGET(ed->window));
    if (shown) {
        editor_advanced_reveal(ed);
        gtk_window_set_default_size(GTK_WINDOW(ed->window), 490,
                                    h + ed->adv_height);
    } else {
        ed->adv_shown = FALSE;
        gtk_label_set_markup(GTK_LABEL(ed->adv_label), ADV_LABEL_TO_SHOW);
        gtk_widget_set_visible(ed->adv_box, FALSE);
        if (ed->adv_height > 0)
            gtk_window_set_default_size(GTK_WINDOW(ed->window), 490,
                                        MAX(h - ed->adv_height, 100));
        ed->adv_height = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Layout helpers.
 * ------------------------------------------------------------------------- */

/*
 * make_list_section — GtkListBox + button column under a bold heading label.
 *
 * Returns the outer GtkBox.
 */
static GtkWidget *
make_list_section(const gchar *heading, GtkWidget *listbox,
                  GtkWidget *btn_box)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", heading);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(outer), label);

    GtkWidget *hbox   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), listbox);
    gtk_box_append(GTK_BOX(hbox), scroll);
    gtk_box_append(GTK_BOX(hbox), btn_box);
    gtk_box_append(GTK_BOX(outer), hbox);
    return outer;
}

/*
 * small_button — compact labelled button wired to `cb`.
 */
static GtkWidget *
small_button(const gchar *label, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

/* ---------------------------------------------------------------------------
 * editor_open_common — build and present an editor window.
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

    ed->window = gtk_window_new();
    /* Height -1 = natural height: the notes box has a capped content height,
     * so the folded window opens at exactly the size of its fixed rows plus
     * the notes scroller.  editor_advanced_set adds the block height when
     * the user expands it.                                                  */
    gtk_window_set_default_size(GTK_WINDOW(ed->window), 490, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(ed->window), vbox);

    /* ── Title ──────────────────────────────────────────────────────────── */
    ed->title_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->title_entry), "Task title");
    g_signal_connect(ed->title_entry, "changed",
                     G_CALLBACK(on_field_changed), ed);
    g_signal_connect(ed->title_entry, "activate",
                     G_CALLBACK(on_editor_save), ed);
    gtk_box_append(GTK_BOX(vbox), ed->title_entry);

    /* ── Status / Due row ────────────────────────────────────────────────
     *
     * Left group: "Status:" label + status dropdown.
     * Right group: "Due:" label + date entry + "at" + time entry.
     * An expanding spacer pushes the right group to the end.              */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    gtk_box_append(GTK_BOX(row), gtk_label_new("Status:"));

    /* Build status dropdown from task_status_label, enum-order.            */
    {
        const gchar *labels[TASK_STATUS_N_VALUES + 1];
        for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
            labels[s] = task_status_label((TaskStatus)s);
        labels[TASK_STATUS_N_VALUES] = NULL;
        ed->status_combo = gtk_drop_down_new_from_strings(labels);
    }
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->status_combo),
                               (guint)TASK_STATUS_NEW);
    g_signal_connect(ed->status_combo, "notify::selected",
                     G_CALLBACK(on_status_notify), ed);
    gtk_box_append(GTK_BOX(row), ed->status_combo);

    /* Spacer.                                                               */
    GtkWidget *rspc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(rspc, TRUE);
    gtk_box_append(GTK_BOX(row), rspc);

    /* Due date (right side, left-to-right: "Due:" date "at" time).         */
    gtk_box_append(GTK_BOX(row), gtk_label_new("Due:"));

    ed->due_entry = gtk_entry_new();
    gtk_editable_set_width_chars(GTK_EDITABLE(ed->due_entry), 12);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->due_entry), "YYYY-MM-DD");
    gtk_widget_set_tooltip_text(ed->due_entry, "Click to pick a due date.");
    /* Primary click opens the calendar picker.                             */
    {
        GtkGesture *gc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc),
                                      GDK_BUTTON_PRIMARY);
        g_signal_connect(gc, "pressed",
                         G_CALLBACK(on_due_entry_press), ed);
        gtk_widget_add_controller(ed->due_entry, GTK_EVENT_CONTROLLER(gc));
    }
    g_signal_connect(ed->due_entry, "changed",
                     G_CALLBACK(on_field_changed), ed);
    gtk_box_append(GTK_BOX(row), ed->due_entry);

    gtk_box_append(GTK_BOX(row), gtk_label_new("at"));

    ed->due_time_entry = gtk_entry_new();
    /* FIVE chars: "HH:MM".  Six was 4 px past the 490 the window asks for.*/
    gtk_editable_set_width_chars(GTK_EDITABLE(ed->due_time_entry), 5);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(ed->due_time_entry), 5);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->due_time_entry), "HH:MM");
    gtk_widget_set_tooltip_text(ed->due_time_entry,
        "The time of day this task is due (24-hour), 08:00 unless you "
        "change it.  It is kept on this machine only \xe2\x80\x94 Google "
        "Tasks stores a due DATE and discards any time, so a sync will "
        "not carry it or overwrite it.");
    g_signal_connect(ed->due_time_entry, "changed",
                     G_CALLBACK(on_field_changed), ed);
    gtk_box_append(GTK_BOX(row), ed->due_time_entry);

    gtk_box_append(GTK_BOX(vbox), row);

    /* ── Favorite / High Priority / completion date row ─────────────────── */
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    ed->pinned_check = gtk_check_button_new_with_label("Favorite");
    g_signal_connect(ed->pinned_check, "toggled",
                     G_CALLBACK(on_toggle_changed), ed);
    gtk_box_append(GTK_BOX(flags), ed->pinned_check);

    ed->priority_check = gtk_check_button_new_with_label("High Priority");
    g_signal_connect(ed->priority_check, "toggled",
                     G_CALLBACK(on_toggle_changed), ed);
    gtk_box_append(GTK_BOX(flags), ed->priority_check);

    GtkWidget *fspc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(fspc, TRUE);
    gtk_box_append(GTK_BOX(flags), fspc);

    ed->completed_label = gtk_label_new(NULL);
    gtk_box_append(GTK_BOX(flags), ed->completed_label);

    gtk_box_append(GTK_BOX(vbox), flags);

    /* ── Notes ──────────────────────────────────────────────────────────── */
    GtkWidget *notes_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(notes_label), "<b>Notes</b>");
    gtk_widget_set_halign(notes_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), notes_label);

    GtkWidget *notes_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(notes_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(notes_scroll), TRUE);

    GtkWidget *notes_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(notes_view), GTK_WRAP_WORD);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(notes_view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(notes_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(notes_view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(notes_view), 4);
    ed->notes_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(notes_view));
    g_signal_connect(ed->notes_buf, "changed",
                     G_CALLBACK(on_field_changed), ed);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(notes_scroll),
                                  notes_view);
    /* Eight lines: measure by laying out eight "X\n" lines in the view's
     * own Pango context (+12 px: the view's 4 px top+bottom margins and
     * 4 px slack so the caret on line 8 is not flush against the frame).  */
    {
        PangoLayout *lay = gtk_widget_create_pango_layout(notes_view,
            "X\nX\nX\nX\nX\nX\nX\nX");
        gint lines_w, lines_h;
        pango_layout_get_pixel_size(lay, &lines_w, &lines_h);
        g_object_unref(lay);
        if (lines_h <= 0)
            lines_h = 8 * 17;
        gint content = lines_h + 12;
        gtk_scrolled_window_set_min_content_height(
            GTK_SCROLLED_WINDOW(notes_scroll), content);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(notes_scroll), content);
    }
    gtk_widget_set_hexpand(notes_scroll, TRUE);
    gtk_widget_set_vexpand(notes_scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), notes_scroll);

    /* ── adv_box: Subtasks + Attachments + Recurrence ────────────────────
     *
     * Hidden until the user clicks Advanced or the task already has content.
     * Hidden = not in the window's natural height.                         */
    ed->adv_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_visible(ed->adv_box, FALSE);

    /* ── Subtasks (top-level tasks only) ─────────────────────────────────  */
    if (ed->parent_id == 0) {
        ed->sub_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(ed->sub_box),
                                        GTK_SELECTION_SINGLE);

        GtkWidget *sub_btns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append(GTK_BOX(sub_btns),
            small_button("Add", G_CALLBACK(on_sub_add), ed));
        gtk_box_append(GTK_BOX(sub_btns),
            small_button("Remove", G_CALLBACK(on_sub_remove), ed));

        GtkWidget *move_box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        GtkWidget *up_btn     = gtk_button_new_with_label("\xe2\x96\xb2");
        GtkWidget *down_btn   = gtk_button_new_with_label("\xe2\x96\xbc");
        g_object_set_data(G_OBJECT(up_btn),   "task-direction",
                          GINT_TO_POINTER(-1));
        g_object_set_data(G_OBJECT(down_btn), "task-direction",
                          GINT_TO_POINTER(1));
        g_signal_connect(up_btn,   "clicked", G_CALLBACK(on_sub_move), ed);
        g_signal_connect(down_btn, "clicked", G_CALLBACK(on_sub_move), ed);
        gtk_widget_set_hexpand(up_btn,   TRUE);
        gtk_widget_set_hexpand(down_btn, TRUE);
        gtk_box_append(GTK_BOX(move_box), up_btn);
        gtk_box_append(GTK_BOX(move_box), down_btn);
        gtk_box_append(GTK_BOX(sub_btns), move_box);

        GtkWidget *sub_section =
            make_list_section("Subtasks", ed->sub_box, sub_btns);
        gtk_box_append(GTK_BOX(ed->adv_box), sub_section);
    } else {
        Task *parent = task_db_task_get(app->db, ed->parent_id);
        gchar *txt = g_strdup_printf(
            "This is a subtask of \xe2\x80\x9c%s\xe2\x80\x9d "
            "\xe2\x80\x94 subtasks cannot have their own subtasks.",
            parent != NULL ? parent->title : "?");
        GtkWidget *note = gtk_label_new(txt);
        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_widget_set_halign(note, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), note);
        g_free(txt);
        task_free(parent);
    }

    /* ── Attachments ─────────────────────────────────────────────────────  */
    ed->att_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ed->att_box),
                                    GTK_SELECTION_SINGLE);
    g_signal_connect(ed->att_box, "row-activated",
                     G_CALLBACK(on_att_row_activated), ed);

    GtkWidget *att_btns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(att_btns),
        small_button("Add\xe2\x80\xa6", G_CALLBACK(on_att_add), ed));
    gtk_box_append(GTK_BOX(att_btns),
        small_button("Remove", G_CALLBACK(on_att_remove), ed));
    gtk_box_append(GTK_BOX(att_btns),
        small_button("Open", G_CALLBACK(on_att_open), ed));

    GtkWidget *att_section =
        make_list_section("Attachments", ed->att_box, att_btns);
    gtk_box_append(GTK_BOX(ed->adv_box), att_section);

    /* ── Recurrence (LAST in adv_box — at the foot above the disclosure) ─  */
    {
        GtkWidget *rec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *heading = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(heading), "<b>Recurrence</b>");
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(rec), heading);

        GtkWidget *desc = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(desc),
            "<small><span alpha=\"65%\">"
            "Recurrence will change the Due Date of your task to the next "
            "specified iteration date and time.  Additionally, completed "
            "tasks will be set to New X (defaults to a week, or less on a "
            "shorter repeat) before that Due Date to give you lead time."
            "</span></small>");
        gtk_label_set_xalign(GTK_LABEL(desc), 0.0);
        gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(desc), 52);
        gtk_box_append(GTK_BOX(rec), desc);

        ed->recur_enable = gtk_check_button_new_with_label("Repeat this task");
        gtk_widget_set_tooltip_text(ed->recur_enable,
            "Give this task a repeating schedule.  Switching it off leaves "
            "the task exactly where it is and stops it coming back.");
        gtk_box_append(GTK_BOX(rec), ed->recur_enable);

        /* The body — hidden when the switch is off.                        */
        ed->recur_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_visible(ed->recur_body, FALSE);
        GtkWidget *body = ed->recur_body;
        gtk_box_append(GTK_BOX(rec), body);

        /* Size group aligns the three leading labels so their controls
         * start at the same x.                                              */
        GtkSizeGroup *lead_col =
            gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

        /* Row 1 — "Starting <date> at <time>".                             */
        GtkWidget *r_start = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *l_start = gtk_label_new("Starting");
        gtk_label_set_xalign(GTK_LABEL(l_start), 0.0);
        gtk_size_group_add_widget(lead_col, l_start);
        gtk_box_append(GTK_BOX(r_start), l_start);

        ed->recur_start_entry = gtk_entry_new();
        gtk_editable_set_width_chars(GTK_EDITABLE(ed->recur_start_entry), 12);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(ed->recur_start_entry), 12);
        gtk_entry_set_placeholder_text(GTK_ENTRY(ed->recur_start_entry),
                                       "YYYY-MM-DD");
        gtk_widget_set_tooltip_text(ed->recur_start_entry,
            "Click to pick the day this schedule is anchored on "
            "\xe2\x80\x94 the \"Monday\" of \"every Monday at 9:00 AM\".  "
            "A start still in the future is the FIRST repeat, not a week "
            "after it.  Leave it empty to anchor on the task's own due "
            "date.");
        {
            GtkGesture *gc = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc),
                                          GDK_BUTTON_PRIMARY);
            g_signal_connect(gc, "pressed",
                             G_CALLBACK(on_recur_start_press), ed);
            gtk_widget_add_controller(ed->recur_start_entry,
                                      GTK_EVENT_CONTROLLER(gc));
        }
        g_signal_connect(ed->recur_start_entry, "changed",
                         G_CALLBACK(on_recur_changed), ed);
        gtk_box_append(GTK_BOX(r_start), ed->recur_start_entry);
        gtk_box_append(GTK_BOX(r_start), gtk_label_new("at"));

        ed->recur_time_entry = gtk_entry_new();
        gtk_editable_set_width_chars(GTK_EDITABLE(ed->recur_time_entry), 5);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(ed->recur_time_entry), 5);
        gtk_entry_set_placeholder_text(GTK_ENTRY(ed->recur_time_entry),
                                       "HH:MM");
        gtk_widget_set_tooltip_text(ed->recur_time_entry,
            "The time of day the schedule is anchored at (24-hour).  A "
            "repeat measured in days, weeks, months or years lands on this "
            "time every time; one measured in minutes or hours starts from "
            "it and steps on from there.");
        g_signal_connect(ed->recur_time_entry, "changed",
                         G_CALLBACK(on_recur_changed), ed);
        gtk_box_append(GTK_BOX(r_start), ed->recur_time_entry);
        gtk_box_append(GTK_BOX(body), r_start);

        /* Row 2 — "Repeat every <N> <unit>".                               */
        GtkWidget *r_every = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *l_every = gtk_label_new("Repeat every");
        gtk_label_set_xalign(GTK_LABEL(l_every), 0.0);
        gtk_size_group_add_widget(lead_col, l_every);
        gtk_box_append(GTK_BOX(r_every), l_every);

        ed->recur_every_spin =
            gtk_spin_button_new_with_range(1, 999, 1);
        gtk_widget_set_tooltip_text(ed->recur_every_spin,
            "How often this task comes back.  A set time before each "
            "repeat, a COMPLETED task is put back to New and its due date "
            "moves to that repeat.");
        g_signal_connect(ed->recur_every_spin, "value-changed",
                         G_CALLBACK(on_recur_changed), ed);
        gtk_box_append(GTK_BOX(r_every), ed->recur_every_spin);

        {
            const gchar *unit_labels[TASK_RECUR_N_UNITS + 1];
            for (gint i = 0; i < TASK_RECUR_N_UNITS; i++)
                unit_labels[i] = task_recur_unit_label((TaskRecurUnit)i);
            unit_labels[TASK_RECUR_N_UNITS] = NULL;
            ed->recur_unit_combo =
                gtk_drop_down_new_from_strings(unit_labels);
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->recur_unit_combo),
                                   (guint)TASK_RECUR_DAY);
        g_signal_connect(ed->recur_unit_combo, "notify::selected",
                         G_CALLBACK(on_recur_notify), ed);
        gtk_box_append(GTK_BOX(r_every), ed->recur_unit_combo);
        gtk_box_append(GTK_BOX(body), r_every);

        /* Row 3 — "Reset to New <N> <unit> beforehand".
         * Indented from rows 1–2 by 8 px top margin — a different question
         * about the same schedule.                                          */
        GtkWidget *r3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(r3, 8);
        GtkWidget *l_reset = gtk_label_new("Reset to New");
        gtk_label_set_xalign(GTK_LABEL(l_reset), 0.0);
        gtk_size_group_add_widget(lead_col, l_reset);
        gtk_box_append(GTK_BOX(r3), l_reset);

        ed->recur_lead_spin =
            gtk_spin_button_new_with_range(0, 999, 1);
        gtk_widget_set_tooltip_text(ed->recur_lead_spin,
            "How far ahead of each repeat a completed task is reopened.  "
            "It is shortened automatically when it would not fit inside "
            "the repeat itself.");
        g_signal_connect(ed->recur_lead_spin, "value-changed",
                         G_CALLBACK(on_recur_changed), ed);
        gtk_box_append(GTK_BOX(r3), ed->recur_lead_spin);

        {
            const gchar *lead_labels[RECUR_LEAD_N_UNITS + 1];
            for (gint i = 0; i < RECUR_LEAD_N_UNITS; i++)
                lead_labels[i] = task_recur_unit_label((TaskRecurUnit)i);
            lead_labels[RECUR_LEAD_N_UNITS] = NULL;
            ed->recur_lead_unit =
                gtk_drop_down_new_from_strings(lead_labels);
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(ed->recur_lead_unit),
                                   (guint)TASK_RECUR_DAY);
        g_signal_connect(ed->recur_lead_unit, "notify::selected",
                         G_CALLBACK(on_recur_notify), ed);
        gtk_box_append(GTK_BOX(r3), ed->recur_lead_unit);
        gtk_box_append(GTK_BOX(r3), gtk_label_new("beforehand"));
        gtk_box_append(GTK_BOX(body), r3);

        g_object_unref(lead_col);

        /* Summary.  Wrapped so a long phrase cannot widen the editor.      */
        ed->recur_summary = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(ed->recur_summary), 0.0);
        gtk_label_set_wrap(GTK_LABEL(ed->recur_summary), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(ed->recur_summary), 52);
        gtk_box_append(GTK_BOX(body), ed->recur_summary);

        /* Wire the master switch LAST — construction set_active cannot fire
         * the handler before every widget it reads exists.                  */
        g_signal_connect(ed->recur_enable, "toggled",
                         G_CALLBACK(on_recur_changed), ed);

        gtk_box_append(GTK_BOX(ed->adv_box), rec);
    }

    gtk_box_append(GTK_BOX(vbox), ed->adv_box);

    /* ── Foot row: Advanced link | spacer | Save [Cancel] ────────────────
     *
     * Packed last so it stays at the window's bottom in both fold states.  */
    GtkWidget *foot    = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *adv_btn = gtk_button_new();
    gtk_widget_add_css_class(adv_btn, "flat");
    ed->adv_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ed->adv_label), ADV_LABEL_TO_SHOW);
    gtk_button_set_child(GTK_BUTTON(adv_btn), ed->adv_label);
    task_app_css_install(
        "button.flat { color: #1c71d8; padding: 2px 4px; }");
    gtk_widget_set_tooltip_text(adv_btn,
        "Show or hide the Recurrence, Subtasks and Attachments sections");
    g_signal_connect(adv_btn, "clicked",
                     G_CALLBACK(on_editor_advanced), ed);
    gtk_box_append(GTK_BOX(foot), adv_btn);

    GtkWidget *fspc2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(fspc2, TRUE);
    gtk_box_append(GTK_BOX(foot), fspc2);

    /* Save first → leftmost of the right group.                            */
    gtk_box_append(GTK_BOX(foot),
        small_button("Save", G_CALLBACK(on_editor_save), ed));
    if (is_new)
        gtk_box_append(GTK_BOX(foot),
            small_button("Cancel", G_CALLBACK(on_editor_cancel), ed));

    gtk_box_append(GTK_BOX(vbox), foot);

    g_signal_connect(ed->window, "destroy",
                     G_CALLBACK(on_editor_destroy), ed);

    /* Register + load.                                                     */
    gint64 *key = g_new(gint64, 1);
    *key = task_id;
    g_hash_table_insert(app->editors, key, ed->window);
    g_object_set_data(G_OBJECT(ed->window), "task-editor", ed);
    task_free(t);

    if (!editor_load(ed))
        return;                      /* row gone, window destroyed           */

    /* Reveal the Advanced block BEFORE presenting if the task already has
     * content, so the window appears at its final size in one step.        */
    if (!is_new && editor_has_advanced_content(ed))
        editor_advanced_reveal(ed);

    gtk_window_present(GTK_WINDOW(ed->window));
}

/* ---------------------------------------------------------------------------
 * Public entry points.
 * ------------------------------------------------------------------------- */

/*
 * task_editor_open — open an editor for an existing task (see header).
 */
void
task_editor_open(TaskApp *app, gint64 task_id)
{
    editor_open_common(app, task_id, FALSE);
}

/*
 * task_editor_open_new — open an editor for a just-created task (see header).
 *
 * Adds a Cancel button that tombstones the task.
 */
void
task_editor_open_new(TaskApp *app, gint64 task_id)
{
    editor_open_common(app, task_id, TRUE);
}

/*
 * editor_windows — every open editor window (caller must g_list_free).
 */
static GList *
editor_windows(TaskApp *app)
{
    return g_hash_table_get_values(app->editors);
}

/*
 * task_editor_refresh_all — reload every open editor (see header).
 */
void
task_editor_refresh_all(TaskApp *app)
{
    GList *windows = editor_windows(app);
    for (GList *l = windows; l != NULL; l = l->next) {
        TaskEditor *ed = g_object_get_data(G_OBJECT(l->data), "task-editor");
        if (ed == NULL || ed->save_source != 0)
            continue;
        editor_load(ed);
    }
    g_list_free(windows);
}

/*
 * task_editor_close_all — destroy every open editor, flushing saves.
 */
void
task_editor_close_all(TaskApp *app)
{
    GList *windows = editor_windows(app);
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_window_destroy(GTK_WINDOW(l->data));
    g_list_free(windows);
}
