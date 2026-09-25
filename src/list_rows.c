/* ===========================================================================
 * list_rows.c — the item objects behind the GTK4 list widgets (see list_rows.h)
 *
 * Three GObject classes with public fields and a finalizer each; task_row_touch
 * is the one in-place update path that keeps the sorted model and the bound
 * widgets consistent without a full store rebuild.
 * =========================================================================== */

#include "list_rows.h"

/* ---------------------------------------------------------------------------
 * TaskListRow — the abstract base: nothing but the "changed" signal.
 * ------------------------------------------------------------------------- */
G_DEFINE_ABSTRACT_TYPE(TaskListRow, task_list_row, G_TYPE_OBJECT)

static guint row_changed_signal;     /* TaskListRow::changed                */

static void
task_list_row_class_init(TaskListRowClass *klass)
{
    row_changed_signal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                      G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                      NULL, G_TYPE_NONE, 0);
}

static void
task_list_row_init(TaskListRow *r)
{
    (void)r;
}

/* ---------------------------------------------------------------------------
 * TaskRow — one task in the task pane.
 * ------------------------------------------------------------------------- */
G_DEFINE_TYPE(TaskRow, task_row, TASK_TYPE_LIST_ROW)

static void
task_row_finalize(GObject *object)
{
    TaskRow *r = TASK_ROW(object);
    g_free(r->title);
    g_free(r->markup);
    g_free(r->due_text);
    g_free(r->completed_text);
    g_free(r->status_text);
    G_OBJECT_CLASS(task_row_parent_class)->finalize(object);
}

static void
task_row_class_init(TaskRowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = task_row_finalize;
}

static void
task_row_init(TaskRow *r)
{
    (void)r;
}

/*
 * task_row_new — an empty task row; all fields are zero/NULL.
 * Inputs: none
 * Output: new TaskRow (g_object_unref to free).
 */
TaskRow *
task_row_new(void)
{
    return g_object_new(TASK_TYPE_ROW, NULL);
}

/* ---------------------------------------------------------------------------
 * TaskSbRow — one sidebar row.
 * ------------------------------------------------------------------------- */
G_DEFINE_TYPE(TaskSbRow, task_sb_row, TASK_TYPE_LIST_ROW)

static void
task_sb_row_finalize(GObject *object)
{
    TaskSbRow *r = TASK_SB_ROW(object);
    g_free(r->label);
    g_clear_object(&r->children);
    G_OBJECT_CLASS(task_sb_row_parent_class)->finalize(object);
}

static void
task_sb_row_class_init(TaskSbRowClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = task_sb_row_finalize;
}

static void
task_sb_row_init(TaskSbRow *r)
{
    (void)r;
}

/*
 * task_sb_row_new — a sidebar row; `expandable` gives it an empty children
 * store.  The label string is copied.
 * Inputs:
 *   kind       — SB_KIND_* constant
 *   id         — view index, list id or group id
 *   label      — display text (copied)
 *   bold       — render in bold
 *   expandable — give the row a children GListStore
 * Output: new TaskSbRow (g_object_unref to free).
 */
TaskSbRow *
task_sb_row_new(gint kind, gint64 id, const gchar *label,
                gboolean bold, gboolean expandable)
{
    TaskSbRow *r = g_object_new(TASK_TYPE_SB_ROW, NULL);
    r->kind  = kind;
    r->id    = id;
    r->label = g_strdup(label);
    r->bold  = bold;
    if (expandable)
        r->children = g_list_store_new(TASK_TYPE_SB_ROW);
    return r;
}

/* ---------------------------------------------------------------------------
 * task_row_touch() — a row's fields changed: every bound widget rebinds
 * ("changed"), and the store re-announces the item so a sorted model re-sorts.
 * ------------------------------------------------------------------------- */

/*
 * task_row_touch — signal a row update and notify the store it lives in.
 * Inputs:
 *   store — the GListStore holding `row`
 *   row   — the TaskListRow (or derived type) that changed
 * Output: TRUE when the row was found in the store, FALSE otherwise.
 *         The "changed" signal is emitted either way.
 */
gboolean
task_row_touch(GListStore *store, gpointer row)
{
    g_signal_emit(row, row_changed_signal, 0);
    guint pos;
    if (!g_list_store_find(store, row, &pos))
        return FALSE;
    g_list_model_items_changed(G_LIST_MODEL(store), pos, 1, 1);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * task_row_factory_new() and its trampolines.  The factory carries the
 * caller's bind and data; each list item carries the handler id of its
 * "changed" connection while bound.
 * ------------------------------------------------------------------------- */
typedef struct {
    GCallback bind;                  /* the caller's bind handler           */
    gpointer  user_data;
} RowFactory;

static void row_factory_bind_again(GtkListItem *item);

/*
 * row_factory_bind — the caller's bind, then listen for "changed".
 * Inputs: standard GtkSignalListItemFactory "bind" arguments + RowFactory *data
 * Output: none
 */
static void
row_factory_bind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    RowFactory *rf = data;
    ((void (*)(GtkListItemFactory *, GtkListItem *, gpointer))rf->bind)(
        f, item, rf->user_data);
    gpointer row = gtk_list_item_get_item(item);
    if (TASK_IS_LIST_ROW(row)) {
        gulong id = g_signal_connect_swapped(row, "changed",
                                             G_CALLBACK(row_factory_bind_again),
                                             item);
        g_object_set_data(G_OBJECT(item), "task-changed-id",
                          GSIZE_TO_POINTER(id));
        g_object_set_data(G_OBJECT(item), "task-factory", f);
    }
}

/*
 * row_factory_bind_again — the bound row changed: rebind.
 * Inputs: item — the GtkListItem whose row emitted "changed"
 * Output: none
 */
static void
row_factory_bind_again(GtkListItem *item)
{
    GtkListItemFactory *f = g_object_get_data(G_OBJECT(item), "task-factory");
    RowFactory *rf = g_object_get_data(G_OBJECT(f), "task-row-factory");
    ((void (*)(GtkListItemFactory *, GtkListItem *, gpointer))rf->bind)(
        f, item, rf->user_data);
}

/*
 * row_factory_unbind — stop listening; the item is about to be recycled.
 * Inputs: standard GtkSignalListItemFactory "unbind" arguments
 * Output: none
 */
static void
row_factory_unbind(GtkListItemFactory *f, GtkListItem *item, gpointer data)
{
    (void)f; (void)data;
    gulong id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(item),
                                                    "task-changed-id"));
    if (id != 0) {
        g_signal_handler_disconnect(gtk_list_item_get_item(item), id);
        g_object_set_data(G_OBJECT(item), "task-changed-id", NULL);
    }
}

/*
 * task_row_factory_new — a GtkSignalListItemFactory whose bind runs again
 * whenever the bound row emits "changed".
 * Inputs:
 *   setup     — the "setup" handler
 *   bind      — the "bind" handler, also the rebind
 *   user_data — passed to both
 * Output: new factory (caller unref's it after handing to a column/view).
 */
GtkListItemFactory *
task_row_factory_new(GCallback setup, GCallback bind, gpointer user_data)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    RowFactory *rf = g_new0(RowFactory, 1);
    rf->bind      = bind;
    rf->user_data = user_data;
    g_object_set_data_full(G_OBJECT(f), "task-row-factory", rf, g_free);
    g_signal_connect(f, "setup",  setup, user_data);
    g_signal_connect(f, "bind",   G_CALLBACK(row_factory_bind),   rf);
    g_signal_connect(f, "unbind", G_CALLBACK(row_factory_unbind), rf);
    return f;
}
