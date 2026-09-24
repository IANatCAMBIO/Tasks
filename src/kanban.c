/* ===========================================================================
 * kanban.c — the library window's Kanban BOARD: three lanes of cards, the
 * hand-rolled card drag, the per-view card order and the board's CSS (see
 * library_priv.h).
 *
 * GTK4 port: GtkEventBox removed; cards are plain GtkBox widgets.
 * Drag is via GtkGestureDrag on the ⠿ grip; ghost is a GtkPicture on
 * ghost_layer (GtkFixed overlay child).  Hit-testing uses
 * gtk_widget_compute_bounds in overlay coords.  All GTK3 grab/window/
 * cairo-surface code is gone.
 * =========================================================================== */

#include "library_priv.h"
#include "editor_window.h"
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Kanban board — the third task-pane variant.
 *
 * Built from the Weekly Forecast's parts (a heading label over a framed
 * body, everything at natural height inside ONE outer scroller so the
 * whole board scrolls together), with the sections turned through 90°:
 * three side-by-side lanes, one per TaskStatus, holding CARDS rather
 * than list rows.  Lane index IS the status value.
 *
 * The drag is HAND-ROLLED via GtkGestureDrag on the ⠿ grip, with a
 * GtkPicture ghost on ghost_layer (a GtkFixed overlay child).  There is no
 * gdk_seat_grab, no GdkWindow manipulation and no cairo surface rendering.
 * Hit-testing uses gtk_widget_compute_bounds in overlay-widget coordinates.
 * =========================================================================== */

/* How far the pointer must travel before a press becomes a drag.            */
/* The ghost's opacity: enough to read the card, enough to see the lane.     */
#define CARD_GHOST_ALPHA 0.65

/* Thickness of the insertion marker, in logical px.                         */
#define CARD_MARK_H 3

/* Breathing room either side of the ⠿ grip glyph.                          */
#define CARD_GRIP_PAD 5

/* Inner insets, applied as WIDGET MARGINS on the card label child.          */
#define CARD_PAD 8

#define LANE_PAD 6

/* How many completed tasks the Done lane shows before its "Show All" link.  */
#define DONE_CAP 10

/* pad_widget() — inset a widget from its parent on all four sides.          */
static void
pad_widget(GtkWidget *w, gint pad)
{
    gtk_widget_set_margin_start(w, pad);
    gtk_widget_set_margin_end(w, pad);
    gtk_widget_set_margin_top(w, pad);
    gtk_widget_set_margin_bottom(w, pad);
}

/* kanban_css_install() — the board's look, installed ONCE for the whole
 * display via task_app_css_install.
 *
 * In GTK4, GtkBox is not a windowed widget in the GDK sense, but the GTK4
 * node-based renderer draws CSS backgrounds and box-shadows on any widget,
 * including a plain GtkBox.  So the shadow wrapper from the GTK3 port (which
 * existed because GtkEventBox clipped outset shadows at its GdkWindow
 * boundary — gotcha 30) is gone.  box-shadow on .task-card just works.
 * One caveat: the GTK-CSS alpha() extension does NOT parse inside box-shadow
 * values; use rgba() there instead.
 * ------------------------------------------------------------------------- */
static void
kanban_css_install(void)
{
    static gboolean done = FALSE;    /* one provider per process              */
    if (done)
        return;
    done = TRUE;
    /* SQUARE corners throughout, matching the Weekly Forecast's framed day
     * sections: a rounded tint inside a square frame reads as a mistake, and
     * rounded cards made the board the odd view out.  No border-radius here
     * is deliberate — don't add one back.                                    */
    task_app_css_install(
        ".task-lane {"
        "  background-color: alpha(@theme_fg_color, 0.05);"
        "  border: 1px solid alpha(@theme_fg_color, 0.20);"
        /* GTK4: the lane IS the content box (no GtkEventBox wrapper), so inner
         * padding must be CSS.  The old pad_widget(lane, LANE_PAD) only adds an
         * outer margin; cards would otherwise pack against the lane's border.  */
        "  padding: 6px;"
        "}"
        ".task-card {"
        "  background-color: @theme_base_color;"
        "  border: 1px solid alpha(@theme_fg_color, 0.22);"
        "}"
        ".task-card:hover {"
        "  border-color: alpha(@theme_fg_color, 0.45);"
        "}"
        /* Landing indicator: lane tint says COLUMN, marker bar says SLOT.
         * .task-lane-target is listed AFTER .task-lane so it wins at equal
         * specificity.                                                       */
        ".task-lane-target {"
        "  background-color: alpha(@theme_selected_bg_color, 0.22);"
        "}"
        ".task-card-mark {"
        "  background-color: @theme_selected_bg_color;"
        "}"
        /* The card's DROP SHADOW.  In GTK4, box-shadow on a GtkBox widget
         * works correctly — the render-node approach does not clip it at the
         * widget boundary.  The light is in the UPPER LEFT so the shadow
         * falls bottom and right only; the negative spread keeps it off the
         * top and left edges (gotcha 30 is GTK3-only).                      */
        /* rgba() rather than alpha(@theme_fg_color,...): the GTK-CSS alpha()
         * extension does not parse inside box-shadow values.               */
        ".task-card-shadow {"
        "  box-shadow: 2px 2px 3px -1px rgba(0,0,0,0.40);"
        "}"
        /* No shadow while in flight — a crisp shadow under a nearly
         * transparent card reads as the shadow having come loose.           */
        ".task-card-shadow-flat {"
        "  box-shadow: none;"
        "}"
        /* The ⠿ grip strip — only this area starts a drag.  No hover tint:
         * the glyph and the "grab" cursor already communicate draggability;
         * a narrow grey strip on a white card adds noise.                   */
        /* The original card stays in place, dimmed while its ghost moves.   */
        ".task-card-dragging {"
        "  opacity: 0.40;"
        "}"
        /* The board's selection indicator.                                  */
        ".task-card-selected {"
        "  border-color: @theme_selected_bg_color;"
        "  background-color: alpha(@theme_selected_bg_color, 0.16);"
        "}"
        /* Count badge on the ghost for a multi-card drag.                   */
        ".task-card-badge {"
        "  background-color: rgba(47,93,192,0.95);"
        "  color: white;"
        "  border-radius: 8px;"
        "  padding: 1px 5px;"
        "  font-weight: bold;"
        "  font-size: 0.8em;"
        "}");
}

/* lane_clear() — destroy a lane's cards (the GTK4 way: unparent each child
 * until none remain).                                                        */
static void
lane_clear(GtkWidget *lane)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(lane)) != NULL)
        gtk_widget_unparent(child);
}

/* Forward declarations.                                                      */
static void card_lane_highlight(TaskLibrary *lw, gint lane);
static void card_mark_clear(TaskLibrary *lw);
static GArray *lane_card_ids(TaskLibrary *lw, gint s);

/* card_task_id() — the task a card stands for (0 if unset).                */
static gint64
card_task_id(GtkWidget *card)
{
    return (gint64)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(card), "task-task-id"));
}

/* card_of() — in GTK4 there is no shadow wrapper, so this is the identity.
 * Kept so the occasional caller at the boundary of wrapper-aware code
 * compiles unchanged.                                                        */
static GtkWidget *
card_of(GtkWidget *child)
{
    return child;
}

/* card_sel_has() — is `id` selected?                                        */
static gboolean
card_sel_has(TaskLibrary *lw, gint64 id)
{
    return lw->board.kanban_sel != NULL &&
           g_hash_table_contains(lw->board.kanban_sel,
                                 GSIZE_TO_POINTER((gsize)id));
}

static void
card_sel_add(TaskLibrary *lw, gint64 id)
{
    if (id != 0)
        g_hash_table_add(lw->board.kanban_sel, GSIZE_TO_POINTER((gsize)id));
}

static void
card_sel_remove(TaskLibrary *lw, gint64 id)
{
    g_hash_table_remove(lw->board.kanban_sel, GSIZE_TO_POINTER((gsize)id));
}

static guint
card_sel_count(TaskLibrary *lw)
{
    return lw->board.kanban_sel != NULL
           ? g_hash_table_size(lw->board.kanban_sel) : 0;
}

/* ---------------------------------------------------------------------------
 * card_restyle() — paint the selection onto the cards IN PLACE.
 *
 * Runs in place rather than through a refresh: a refresh here would
 * destroy the very widget a drag is about to start from.
 * ------------------------------------------------------------------------- */
static void
card_restyle(TaskLibrary *lw)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *lane = lw->board.kanban_lanes[s];
        if (lane == NULL)
            continue;
        for (GtkWidget *child = gtk_widget_get_first_child(lane);
             child != NULL;
             child = gtk_widget_get_next_sibling(child)) {
            gint64 id = card_task_id(child);
            if (id == 0)
                continue;            /* marker / placeholder                 */
            if (card_sel_has(lw, id))
                gtk_widget_add_css_class(child, "task-card-selected");
            else
                gtk_widget_remove_css_class(child, "task-card-selected");
        }
    }
}

/* ---------------------------------------------------------------------------
 * card_shadow_restyle() — add or remove .task-card-shadow on every card
 * currently on the board, from the cached flag.
 *
 * In GTK4, box-shadow on .task-card works directly (no wrapper needed), so
 * this toggles the class on the card widget itself.
 * ------------------------------------------------------------------------- */
static void
card_shadow_restyle(TaskLibrary *lw)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *lane = lw->board.kanban_lanes[s];
        if (lane == NULL)
            continue;
        for (GtkWidget *child = gtk_widget_get_first_child(lane);
             child != NULL;
             child = gtk_widget_get_next_sibling(child)) {
            if (card_task_id(child) == 0)
                continue;            /* marker / placeholder                 */
            if (lw->board.card_shadow)
                gtk_widget_add_css_class(child, "task-card-shadow");
            else
                gtk_widget_remove_css_class(child, "task-card-shadow");
        }
    }
}

/* card_select() — collapse the selection to just `id`.                      */
static void
card_select(TaskLibrary *lw, gint64 id)
{
    g_hash_table_remove_all(lw->board.kanban_sel);
    card_sel_add(lw, id);
    lw->board.kanban_anchor = id;
    card_restyle(lw);
}

/* ---------------------------------------------------------------------------
 * lib_card_sel_ids() — selected ids in board display order (lane by lane,
 * top to bottom).  Free with g_array_unref.
 * ------------------------------------------------------------------------- */
GArray *
lib_card_sel_ids(TaskLibrary *lw)
{
    GArray *out = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GArray *lane = lane_card_ids(lw, s);
        for (guint i = 0; i < lane->len; i++) {
            gint64 id = g_array_index(lane, gint64, i);
            if (card_sel_has(lw, id))
                g_array_append_val(out, id);
        }
        g_array_unref(lane);
    }
    return out;
}

/* ---------------------------------------------------------------------------
 * card_sel_range() — select from the shift anchor to `id`.
 *
 * WITHIN ONE LANE only.  A cross-lane shift-click behaves like a
 * modify-click (just adds the card).
 * ------------------------------------------------------------------------- */
static void
card_sel_range(TaskLibrary *lw, gint64 id)
{
    gint64 anchor = lw->board.kanban_anchor;
    if (anchor == 0 || anchor == id) {
        card_sel_add(lw, id);
        card_restyle(lw);
        return;
    }
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GArray *lane = lane_card_ids(lw, s);
        gint ai = -1, bi = -1;
        for (guint i = 0; i < lane->len; i++) {
            gint64 v = g_array_index(lane, gint64, i);
            if (v == anchor) ai = (gint)i;
            if (v == id)     bi = (gint)i;
        }
        if (ai >= 0 && bi >= 0) {
            gint lo = MIN(ai, bi), hi = MAX(ai, bi);
            for (gint i = lo; i <= hi; i++)
                card_sel_add(lw, g_array_index(lane, gint64, (guint)i));
            g_array_unref(lane);
            card_restyle(lw);
            return;
        }
        g_array_unref(lane);
    }
    card_sel_add(lw, id);
    card_restyle(lw);
}

/* ---------------------------------------------------------------------------
 * on_card_press() — GtkGestureClick "pressed" handler on the card widget.
 *
 * Handles: single click (selection), double-click (open editor), right-click
 * (context menu).  Does NOT arm a drag — that belongs to the ⠿ grip's
 * GtkGestureDrag.
 *
 * Selection: plain click inside existing selection keeps it (so a drag can
 * start from any card of a multi-selection).  Collapse happens on release
 * (on_card_release) or on drag-end without a drag.
 * ------------------------------------------------------------------------- */
static void
on_card_press(GtkGestureClick *click, gint n_press, gdouble x, gdouble y,
              gpointer data)
{
    GtkWidget    *card = gtk_event_controller_get_widget(
                             GTK_EVENT_CONTROLLER(click));
    TaskLibrary  *lw   = data;
    gint64        id   = card_task_id(card);
    if (id == 0)
        return;

    guint button = gtk_gesture_single_get_current_button(
                       GTK_GESTURE_SINGLE(click));

    if (n_press == 2 && button == 1) {
        lib_card_drag_stop(lw);
        task_editor_open(lw->app, id);
        gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
        return;
    }

    GdkEvent       *event  = gtk_event_controller_get_current_event(
                                 GTK_EVENT_CONTROLLER(click));
    GdkModifierType state  = event ? gdk_event_get_modifier_state(event) : 0;
    /* In GTK4, modifier intent enums are gone.  Ctrl = modify-selection,
     * Shift = extend-selection on all platforms (the Cmd key is also
     * GDK_META_MASK on macOS but Control is always the selection modifier
     * exposed through GtkGesture's modifier state for click sequences).     */
    gboolean modify = (state & (GDK_CONTROL_MASK | GDK_META_MASK)) != 0;
    gboolean extend = (state & GDK_SHIFT_MASK) != 0;

    if (button == 3) {
        if (!card_sel_has(lw, id))
            card_select(lw, id);
        task_context_menu_popup(lw, card, x, y);
        return;
    }

    if (n_press == 1 && button == 1) {
        if (extend) {
            card_sel_range(lw, id);
        } else if (modify) {
            if (card_sel_has(lw, id))
                card_sel_remove(lw, id);
            else
                card_sel_add(lw, id);
            lw->board.kanban_anchor = id;
            card_restyle(lw);
        } else {
            /* A plain click INSIDE the selection keeps it: that is what lets
             * a multi-card drag start from any of its cards.  The collapse
             * happens on release (on_card_release or drag-end).
             *
             * The ANCHOR moves either way — it means "last plainly clicked".*/
            if (!card_sel_has(lw, id))
                card_select(lw, id);
            else
                lw->board.kanban_anchor = id;
        }
    }
}

/* on_card_release() — GtkGestureClick "released" on the card: collapse a
 * multi-selection when the click was not a drag.                            */
static void
on_card_release(GtkGestureClick *click, gint n_press, gdouble x, gdouble y,
                gpointer data)
{
    (void)n_press; (void)x; (void)y;
    GtkWidget   *card = gtk_event_controller_get_widget(
                            GTK_EVENT_CONTROLLER(click));
    TaskLibrary *lw   = data;
    gint64       id   = card_task_id(card);
    if (id == 0 || lw->board.card_dragging)
        return;

    guint button = gtk_gesture_single_get_current_button(
                       GTK_GESTURE_SINGLE(click));
    if (button != 1)
        return;

    GdkEvent       *event  = gtk_event_controller_get_current_event(
                                 GTK_EVENT_CONTROLLER(click));
    GdkModifierType state  = event ? gdk_event_get_modifier_state(event) : 0;

    /* Collapse multi-selection only on a plain (unmodified) click.          */
    if ((state & (GDK_CONTROL_MASK | GDK_META_MASK | GDK_SHIFT_MASK)) == 0 &&
        card_sel_count(lw) > 1)
        card_select(lw, id);
}

/* ---------------------------------------------------------------------------
 * card_lane_at_pos() — which lane contains overlay-coordinate point (px,py).
 *
 * Uses gtk_widget_compute_bounds with ghost_layer as the reference widget,
 * because (px, py) are in that coordinate space — the same space the ghost
 * moves through.
 * ------------------------------------------------------------------------- */
static gint
card_lane_at_pos(TaskLibrary *lw, gdouble px, gdouble py)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *box = lw->board.kanban_drops[s];
        if (box == NULL || !gtk_widget_get_mapped(box))
            continue;
        graphene_rect_t bounds;
        if (gtk_widget_compute_bounds(box, lw->ghost_layer, &bounds) &&
            graphene_rect_contains_point(&bounds,
                &GRAPHENE_POINT_INIT((float)px, (float)py)))
            return s;
    }
    return -1;
}

/* card_mark_clear() — take the insertion marker off screen.                 */
static void
card_mark_clear(TaskLibrary *lw)
{
    g_clear_pointer(&lw->board.card_mark, gtk_widget_unparent);
    lw->board.card_mark_lane = -1;
    lw->board.card_mark_slot = -1;
}

/* ---------------------------------------------------------------------------
 * card_mark_place() — show the insertion marker at (lane, slot).
 *
 * Rebuilt on a CHANGE only: re-inserting on every event would shuffle the
 * cards under the pointer continuously.
 * ------------------------------------------------------------------------- */
static void
card_mark_place(TaskLibrary *lw, gint lane, gint slot)
{
    if (lane == lw->board.card_mark_lane && slot == lw->board.card_mark_slot)
        return;
    card_mark_clear(lw);
    if (lane < 0 || lane >= TASK_STATUS_N_VALUES ||
        lw->board.kanban_lanes[lane] == NULL)
        return;

    GtkWidget *lane_box = lw->board.kanban_lanes[lane];
    GtkWidget *mark     = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(mark, "task-card-mark");
    gtk_widget_set_size_request(mark, -1, CARD_MARK_H);

    /* Find the sibling AFTER which to insert the mark.
     * slot 0 = before the first card → sibling = NULL (prepend).
     * slot N = after the Nth card → sibling = that card widget.
     * Non-card children (placeholder label) are skipped in the count but
     * do move insert_after forward.                                         */
    GtkWidget *insert_after = NULL;
    gint seen = 0;
    for (GtkWidget *child = gtk_widget_get_first_child(lane_box);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        if (card_task_id(child) != 0) {
            if (seen == slot)
                break;
            seen++;
        }
        insert_after = child;
    }
    gtk_box_insert_child_after(GTK_BOX(lane_box), mark, insert_after);

    lw->board.card_mark      = mark;
    lw->board.card_mark_lane = lane;
    lw->board.card_mark_slot = slot;
}

/* card_lane_highlight() — tint the lane the card would land in, clear rest.
 * `lane` of -1 clears every highlight.                                      */
static void
card_lane_highlight(TaskLibrary *lw, gint lane)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *box = lw->board.kanban_drops[s];
        if (box == NULL)
            continue;
        if (s == lane)
            gtk_widget_add_css_class(box, "task-lane-target");
        else
            gtk_widget_remove_css_class(box, "task-lane-target");
    }
}

/* ---------------------------------------------------------------------------
 * card_slot_at() — which SLOT in lane `s` the overlay-y coordinate `py` is
 * pointing at: 0 before the first card, n after the last.
 *
 * Uses gtk_widget_compute_bounds with ghost_layer as the reference.
 * ------------------------------------------------------------------------- */
static gint
card_slot_at(TaskLibrary *lw, gint s, gdouble py)
{
    gint slot = 0;
    GtkWidget *lane = lw->board.kanban_lanes[s];
    if (lane == NULL)
        return 0;
    for (GtkWidget *child = gtk_widget_get_first_child(lane);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        if (card_task_id(child) == 0)
            continue;                /* marker / placeholder                 */
        graphene_rect_t bounds;
        if (!gtk_widget_compute_bounds(child, lw->ghost_layer, &bounds))
            continue;
        if (py < bounds.origin.y + bounds.size.height / 2.0f)
            break;
        slot++;
    }
    return slot;
}

/* ---------------------------------------------------------------------------
 * card_drag_move() — move the ghost to the current pointer position in
 * overlay coords, and light up the target lane.
 * ------------------------------------------------------------------------- */
static void
card_drag_move(TaskLibrary *lw, gdouble cx, gdouble cy)
{
    if (lw->board.card_ghost != NULL) {
        gdouble gx = cx - lw->board.card_hot_x;
        gdouble gy = cy - lw->board.card_hot_y;
        gtk_fixed_move(GTK_FIXED(lw->ghost_layer), lw->board.card_ghost,
                       gx, gy);
        GtkWidget *badge = g_object_get_data(G_OBJECT(lw->board.card_ghost),
                                             "task-badge");
        if (badge != NULL) {
            gint ghost_w = gtk_widget_get_width(lw->board.card_ghost);
            gtk_fixed_move(GTK_FIXED(lw->ghost_layer), badge,
                           gx + ghost_w - 30, gy + 4);
        }
    }
    gint lane = card_lane_at_pos(lw, cx, cy);
    card_lane_highlight(lw, lane);
    /* No insertion bar over Done: that lane sorts by completion, so a slot
     * marker there would promise a position the drop ignores.               */
    if (lane == TASK_STATUS_DONE) {
        card_mark_clear(lw);
        return;
    }
    card_mark_place(lw, lane, lane >= 0 ? card_slot_at(lw, lane, cy) : -1);
}

/* on_card_drag_key() — GtkEventControllerKey "key-pressed" handler: Escape
 * abandons the drag.                                                         */
static gboolean
on_card_drag_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                 GdkModifierType state, gpointer data)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval != GDK_KEY_Escape)
        return FALSE;
    lib_card_drag_stop(data);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * lib_card_drag_stop() — end a drag (or a merely armed press) and put
 * everything back.  Safe to call when nothing is in flight.
 *
 * THE ONE EXIT for every drag-end path (on_grip_drag_end, Escape, and
 * on_library_destroy), so the ghost can never be orphaned and state is
 * always consistent.
 * ------------------------------------------------------------------------- */
void
lib_card_drag_stop(TaskLibrary *lw)
{
    if (lw->board.card_dragging) {
        gtk_widget_set_cursor_from_name(lw->window, NULL);
        if (lw->board.card_drag_src != NULL) {
            gtk_widget_remove_css_class(lw->board.card_drag_src,
                                        "task-card-dragging");
            gtk_widget_remove_css_class(lw->board.card_drag_src,
                                        "task-card-shadow-flat");
        }
        card_lane_highlight(lw, -1);
        card_mark_clear(lw);
    }
    /* Remove the key controller added at drag start.                        */
    if (lw->board.card_key != NULL) {
        gtk_widget_remove_controller(lw->window, lw->board.card_key);
        g_object_unref(lw->board.card_key);
        lw->board.card_key = NULL;
    }
    /* Remove the ghost (and its badge) from ghost_layer.                    */
    if (lw->board.card_ghost != NULL) {
        GtkWidget *badge = g_object_get_data(G_OBJECT(lw->board.card_ghost),
                                             "task-badge");
        if (badge != NULL)
            gtk_fixed_remove(GTK_FIXED(lw->ghost_layer), badge);
        gtk_fixed_remove(GTK_FIXED(lw->ghost_layer), lw->board.card_ghost);
        lw->board.card_ghost = NULL;
    }
    lw->board.card_dragging = FALSE;
    lw->board.card_drag_src = NULL;
    lw->board.card_drag_id  = 0;
}

/* kanban_order_key() — the current view's card-order key, or NULL for a
 * view that has no board (the forecast).  New string (g_free).              */
static gchar *
kanban_order_key(TaskLibrary *lw)
{
    gchar *key = lib_row_order_key("kanban_order", lw->sel_kind, lw->sel_id);
    if (key != NULL)
        return key;
    return task_view_order_key(lib_sel_view(lw), "kanban_order");
}

/* ---------------------------------------------------------------------------
 * kanban_order_apply() — reorder `tasks` in place to match the saved card
 * order for the current view.
 * ------------------------------------------------------------------------- */
static void
kanban_order_apply(TaskLibrary *lw, GPtrArray *tasks)
{
    gchar *key = kanban_order_key(lw);
    if (key == NULL)
        return;
    gchar *saved = task_app_config_get(key);
    g_free(key);
    if (saved == NULL || tasks->len < 2) {
        g_free(saved);
        return;
    }

    gint    n   = (gint)tasks->len;
    gint64 *ids = g_new(gint64, n);
    for (gint i = 0; i < n; i++)
        ids[i] = ((Task *)g_ptr_array_index(tasks, i))->id;
    gint *order = lib_row_order_permutation(ids, n, saved);
    g_free(saved);
    g_free(ids);
    if (order == NULL)
        return;

    gpointer *was = g_new(gpointer, n);
    memcpy(was, tasks->pdata, sizeof(gpointer) * (gsize)n);
    for (gint i = 0; i < n; i++)
        tasks->pdata[i] = was[order[i]];
    g_free(was);
    g_free(order);
}

/* lane_card_ids() — the task ids currently shown in lane `s`, in display
 * order.  Skips the marker and the empty-lane placeholder (neither carries
 * a task id).  Free with g_array_unref.                                     */
static GArray *
lane_card_ids(TaskLibrary *lw, gint s)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    if (lw->board.kanban_lanes[s] == NULL)
        return ids;
    for (GtkWidget *child =
             gtk_widget_get_first_child(lw->board.kanban_lanes[s]);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        gint64 id = card_task_id(card_of(child));
        if (id != 0)
            g_array_append_val(ids, id);
    }
    return ids;
}

/* ---------------------------------------------------------------------------
 * card_order_save() — rewrite the view's kanban_order key with every lane's
 * cards in display order, the dragged ids lifted out and re-inserted at
 * (lane, slot).  Returns TRUE when the saved order actually changed.
 *
 * REFUSES while a search is up: lane_card_ids reads the cards ON SCREEN,
 * which under a filter is only the matches, so writing that back would drop
 * every hidden task out of the saved order.
 * ------------------------------------------------------------------------- */
static gboolean
card_order_save(TaskLibrary *lw, GArray *to_move, gint lane, gint slot)
{
    if (lw->search != NULL)
        return FALSE;

    GString *order = g_string_new(NULL);
    for (gint sl = 0; sl < TASK_STATUS_N_VALUES; sl++) {
        /* Done contributes NOTHING to the key: it sorts itself by completion
         * and a capped lane would truncate the order for all completed tasks.*/
        if (sl == TASK_STATUS_DONE)
            continue;
        GArray *ids = lane_card_ids(lw, sl);
        for (guint m = 0; m < to_move->len; m++) {
            gint64 id = g_array_index(to_move, gint64, m);
            for (guint i = 0; i < ids->len; i++)
                if (g_array_index(ids, gint64, i) == id) {
                    g_array_remove_index(ids, i);
                    break;
                }
        }
        if (sl == lane) {
            gint at = CLAMP(slot, 0, (gint)ids->len);
            for (guint m = 0; m < to_move->len; m++) {
                gint64 id = g_array_index(to_move, gint64, m);
                g_array_insert_val(ids, at + (gint)m, id);
            }
        }
        for (guint i = 0; i < ids->len; i++) {
            if (order->len > 0)
                g_string_append_c(order, ',');
            g_string_append_printf(order, "%" G_GINT64_FORMAT,
                                   g_array_index(ids, gint64, i));
        }
        g_array_unref(ids);
    }

    gchar *key   = kanban_order_key(lw);
    gchar *saved = key != NULL ? task_app_config_get(key) : NULL;
    gboolean changed = (g_strcmp0(saved, order->str) != 0);
    g_free(saved);
    if (changed && key != NULL)
        task_app_config_set(key, order->str);
    g_free(key);
    g_string_free(order, TRUE);
    return changed;
}

/* ---------------------------------------------------------------------------
 * card_drop_apply() — the drop: put the dragged task(s) in `lane` at `slot`.
 *
 * Two independent halves: the STATUS (database write, stamps updated_at) and
 * the ORDER (config only, no row touch).  A drag that lands the cards
 * exactly where they were does NEITHER.  Returns TRUE when anything changed.
 * ------------------------------------------------------------------------- */
static gboolean
card_drop_apply(TaskLibrary *lw, GArray *moving, gint lane, gint slot)
{
    if (moving == NULL || moving->len == 0 ||
        lane < 0 || lane >= TASK_STATUS_N_VALUES)
        return FALSE;
    TaskStatus want = (TaskStatus)lane;

    GArray *to_move = g_array_new(FALSE, FALSE, sizeof(gint64));
    gchar  *one_title = NULL;
    guint   n_status  = 0;
    for (guint i = 0; i < moving->len; i++) {
        gint64 id = g_array_index(moving, gint64, i);
        Task *t = task_db_task_get(lw->app->db, id);
        if (t == NULL || t->deleted) {
            task_free(t);
            continue;
        }
        g_array_append_val(to_move, id);
        if (t->status != want) {
            n_status++;
            if (one_title == NULL)
                one_title = g_strdup(t->title);
        }
        task_free(t);
    }
    if (to_move->len == 0) {
        g_array_unref(to_move);
        g_free(one_title);
        return FALSE;
    }

    gboolean order_change = lane != TASK_STATUS_DONE
                          ? card_order_save(lw, to_move, lane, slot)
                          : FALSE;

    if (n_status == 0 && !order_change) {
        g_array_unref(to_move);
        g_free(one_title);
        return FALSE;
    }

    for (guint i = 0; i < to_move->len; i++)
        task_db_task_set_status(lw->app->db,
                                g_array_index(to_move, gint64, i), want);

    g_hash_table_remove_all(lw->board.kanban_sel);
    for (guint i = 0; i < to_move->len; i++)
        card_sel_add(lw, g_array_index(to_move, gint64, i));
    lw->board.kanban_anchor = g_array_index(to_move, gint64, 0);

    if (n_status == 1 && one_title != NULL)
        task_app_status(lw->app, "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 %s",
                        *one_title != '\0' ? one_title : "Untitled Task",
                        task_status_label(want));
    else if (n_status > 1)
        task_app_status(lw->app, "%u tasks \xe2\x80\x94 %s", n_status,
                        task_status_label(want));
    g_free(one_title);
    g_array_unref(to_move);
    return TRUE;
}

/* card_refresh_idle() — rebuild the board from an idle callback.
 *
 * Deferred: the drop happens inside the grip's gesture handler, and
 * lib_full_refresh destroys every card including the dragged one.           */
static gboolean
card_refresh_idle(gpointer data)
{
    TaskLibrary *lw = lib_of(data);
    if (lw != NULL)
        lib_full_refresh(lw);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * GtkGestureDrag callbacks — the hand-rolled drag on the ⠿ grip.
 *
 * drag-begin  : record the card, hot spot and press position in overlay
 *               coords.
 * drag-update : once past the threshold, create the ghost, start the visual
 *               drag and call card_drag_move on each motion.
 * drag-end    : apply the drop; if no drag occurred, check whether a
 *               multi-selection should be collapsed.
 * ------------------------------------------------------------------------- */

static void
on_grip_drag_begin(GtkGestureDrag *drag, gdouble start_x, gdouble start_y,
                   gpointer data)
{
    GtkWidget   *grip = gtk_event_controller_get_widget(
                            GTK_EVENT_CONTROLLER(drag));
    TaskLibrary *lw   = data;
    GtkWidget   *card = g_object_get_data(G_OBJECT(grip), "task-card");
    gint64       id   = card != NULL ? card_task_id(card) : 0;
    if (id == 0) {
        gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }

    /* Hot spot: where in the CARD the pointer is gripping.  The ghost hangs
     * off the pointer at this offset so the card does not jump.             */
    graphene_point_t pt;
    if (gtk_widget_compute_point(grip, card,
            &GRAPHENE_POINT_INIT((float)start_x, (float)start_y), &pt)) {
        lw->board.card_hot_x = pt.x;
        lw->board.card_hot_y = pt.y;
    } else {
        lw->board.card_hot_x = 0.0;
        lw->board.card_hot_y = 0.0;
    }

    /* Press position in overlay coordinates: every subsequent position is
     * press_overlay + gesture_offset, which is in the same space as the
     * lane bounds computed by card_lane_at_pos.                             */
    if (gtk_widget_compute_point(grip, lw->ghost_layer,
            &GRAPHENE_POINT_INIT((float)start_x, (float)start_y), &pt)) {
        lw->board.card_press_x = pt.x;
        lw->board.card_press_y = pt.y;
    } else {
        lw->board.card_press_x = start_x;
        lw->board.card_press_y = start_y;
    }

    lw->board.card_drag_src = card;
    lw->board.card_drag_id  = id;
}

static void
on_grip_drag_update(GtkGestureDrag *drag, gdouble offset_x, gdouble offset_y,
                    gpointer data)
{
    GtkWidget   *grip = gtk_event_controller_get_widget(
                            GTK_EVENT_CONTROLLER(drag));
    TaskLibrary *lw   = data;
    if (lw->board.card_drag_src == NULL)
        return;

    gdouble cur_x = lw->board.card_press_x + offset_x;
    gdouble cur_y = lw->board.card_press_y + offset_y;

    if (!lw->board.card_dragging) {
        /* Wait for the platform threshold before committing.                */
        if (!gtk_drag_check_threshold(grip, 0, 0,
                (int)offset_x, (int)offset_y))
            return;

        GtkWidget *card     = lw->board.card_drag_src;
        gint       n_moving = card_sel_has(lw, lw->board.card_drag_id)
                              ? (gint)card_sel_count(lw) : 1;

        /* Ghost: a static snapshot of the card rendered into a GtkPicture,
         * placed on the ghost_layer (GtkFixed overlay child) so it floats
         * above the board without disturbing any layout.
         *
         * gtk_widget_set_opacity works on GtkPicture in GTK4 — the render-
         * node opacity does not need a compositor (gotcha 20 is GTK3-only). */
        GdkPaintable *paintable = GDK_PAINTABLE(gtk_widget_paintable_new(card));
        GdkPaintable *snap =
            gdk_paintable_get_current_image(GDK_PAINTABLE(paintable));
        g_object_unref(paintable);

        if (snap != NULL) {
            GtkWidget *ghost = gtk_picture_new_for_paintable(snap);
            g_object_unref(snap);
            gint cw = gtk_widget_get_width(card);
            gint ch = gtk_widget_get_height(card);
            gtk_widget_set_size_request(ghost, cw, ch);
            gtk_widget_set_opacity(ghost, CARD_GHOST_ALPHA);
            gtk_widget_set_can_target(ghost, FALSE);

            gdouble gx = cur_x - lw->board.card_hot_x;
            gdouble gy = cur_y - lw->board.card_hot_y;
            gtk_fixed_put(GTK_FIXED(lw->ghost_layer), ghost, gx, gy);
            lw->board.card_ghost = ghost;

            /* Count badge for a multi-card drag.                            */
            if (n_moving > 1) {
                gchar     *txt   = g_strdup_printf("%d", n_moving);
                GtkWidget *badge = gtk_label_new(txt);
                g_free(txt);
                gtk_widget_add_css_class(badge, "task-card-badge");
                gtk_widget_set_can_target(badge, FALSE);
                gtk_fixed_put(GTK_FIXED(lw->ghost_layer), badge,
                              gx + cw - 30, gy + 4);
                g_object_set_data(G_OBJECT(ghost), "task-badge", badge);
            }
        }

        /* Cursor: closed hand over the whole window while dragging.         */
        gtk_widget_set_cursor_from_name(lw->window, "grabbing");

        /* Dim the original card in place.  It is NOT hidden: the drag
         * gesture still targets it and hiding would end the gesture.        */
        gtk_widget_add_css_class(card, "task-card-dragging");
        /* Remove its shadow while in flight — a crisp shadow under a faded
         * card reads as the shadow having come loose.                       */
        gtk_widget_add_css_class(card, "task-card-shadow-flat");

        /* Escape key handler on the window (capture phase so it wins).      */
        GtkEventController *key = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
        g_signal_connect(key, "key-pressed",
                         G_CALLBACK(on_card_drag_key), lw);
        gtk_widget_add_controller(lw->window, key);
        lw->board.card_key = key;

        lw->board.card_mark_lane = -1;
        lw->board.card_mark_slot = -1;
        lw->board.card_dragging  = TRUE;

        gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_CLAIMED);
    }

    card_drag_move(lw, cur_x, cur_y);
}

static void
on_grip_drag_end(GtkGestureDrag *drag, gdouble offset_x, gdouble offset_y,
                 gpointer data)
{
    (void)drag;
    TaskLibrary *lw = data;

    if (!lw->board.card_dragging) {
        /* A plain click on the grip (never became a drag).  The press
         * already updated the selection; collapse the multi-selection now if
         * appropriate, the same collapse on_card_release does for body
         * clicks.                                                            */
        gint64 id = lw->board.card_drag_id;
        if (id != 0 && card_sel_count(lw) > 1) {
            /* We cannot reliably read the modifier state from drag-end, so
             * just collapse unconditionally — the press established the
             * anchor and a second unmodified click on a card in the selection
             * is a clear "I want only this one".                             */
            card_select(lw, id);
        }
        lw->board.card_drag_src = NULL;
        lw->board.card_drag_id  = 0;
        return;
    }

    gdouble cur_x = lw->board.card_press_x + offset_x;
    gdouble cur_y = lw->board.card_press_y + offset_y;

    gint lane = card_lane_at_pos(lw, cur_x, cur_y);
    gint slot = (lane >= 0 && lane == lw->board.card_mark_lane)
                ? lw->board.card_mark_slot
                : (lane >= 0 ? card_slot_at(lw, lane, cur_y) : -1);

    GArray *moving = lib_card_sel_ids(lw);
    if (moving->len == 0 || !card_sel_has(lw, lw->board.card_drag_id)) {
        g_array_set_size(moving, 0);
        g_array_append_val(moving, lw->board.card_drag_id);
    }

    lib_card_drag_stop(lw);              /* clears ghost, cursor, state       */
    if (card_drop_apply(lw, moving, lane, slot))
        g_idle_add(card_refresh_idle, lw->app);
    g_array_unref(moving);
}

/* ---------------------------------------------------------------------------
 * kanban_card_new() — one task as a card.
 *
 * GTK4: the card is a plain GtkBox (horizontal, CSS class .task-card).
 * There is no GtkEventBox wrapper and no shadow wrapper — box-shadow on a
 * GtkBox works in GTK4's render-node model (gotcha 30 was GTK3-only).
 *
 * The ⠿ grip is a child GtkBox with its own CSS class and cursor; the
 * GtkGestureDrag on the grip starts drags and a GtkGestureClick on the card
 * handles clicks and double-clicks.
 *
 * Returns the card directly (not a wrapper).
 * ------------------------------------------------------------------------- */
static GtkWidget *
kanban_card_new(TaskLibrary *lw, gint64 id, const gchar *markup,
                gboolean selected)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(card, "task-card");
    if (lw->board.card_shadow)
        gtk_widget_add_css_class(card, "task-card-shadow");
    if (selected)
        gtk_widget_add_css_class(card, "task-card-selected");
    g_object_set_data(G_OBJECT(card), "task-task-id",
                      GSIZE_TO_POINTER((gsize)id));

    /* ---- The ⠿ GRIP ----------------------------------------------------- */
    GtkWidget *handle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(handle, "task-card-handle");
    gtk_widget_set_cursor_from_name(handle, "grab");

    GtkWidget *grip = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(grip),
                         "<span alpha=\"55%\">\xe2\xa0\xbf</span>");
    gtk_widget_set_margin_start(grip, CARD_GRIP_PAD);
    gtk_widget_set_margin_end(grip, CARD_GRIP_PAD);
    /* The handle box fills the card's full height.  vexpand gives the grip
     * the full allocation inside the vertical box; valign then centers the
     * glyph within it.  Without vexpand the box only allocates natural
     * height and valign has no room to act — the glyph stays at the top.   */
    gtk_widget_set_vexpand(grip, TRUE);
    gtk_widget_set_valign(grip, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(handle), grip);
    /* Explicitly block vexpand from propagating out of the handle into the
     * card.  Without this, the grip's vexpand=TRUE bubbles up through handle
     * → card → lane and each card fills the entire lane.                    */
    gtk_widget_set_vexpand(handle, FALSE);
    gtk_box_append(GTK_BOX(card), handle);

    /* ---- The TASK LABEL -------------------------------------------------- */
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    /* ELLIPSIZED, NOT WRAPPED — a performance decision: wrapping is
     * height-for-width and re-runs Pango per card on every resize.
     * max_width_chars caps the label's NATURAL width so a long title cannot
     * push the board past the viewport.                                     */
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
    pad_widget(label, CARD_PAD);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(card), label);
    g_object_set_data(G_OBJECT(card), "task-card-label", label);

    /* ---- Click gesture on the CARD (select, double-click, right-click) -- */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* any btn */
    g_signal_connect(click, "pressed",  G_CALLBACK(on_card_press),   lw);
    g_signal_connect(click, "released", G_CALLBACK(on_card_release), lw);
    gtk_widget_add_controller(card, GTK_EVENT_CONTROLLER(click));

    /* ---- Drag gesture on the GRIP (drag only from this area) ------------ */
    GtkGesture *drag_gest = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag_gest), 1);
    g_object_set_data(G_OBJECT(handle), "task-card", card);
    g_signal_connect(drag_gest, "drag-begin",  G_CALLBACK(on_grip_drag_begin),  lw);
    g_signal_connect(drag_gest, "drag-update", G_CALLBACK(on_grip_drag_update), lw);
    g_signal_connect(drag_gest, "drag-end",    G_CALLBACK(on_grip_drag_end),    lw);
    gtk_widget_add_controller(handle, GTK_EVENT_CONTROLLER(drag_gest));

    return card;
}

/* ---------------------------------------------------------------------------
 * The Done lane — the one lane with NO hand-made order.
 * ------------------------------------------------------------------------- */
#define DONE_LABEL_TO_ALL "<a href=\"#all\">Show all %u completed</a>"
#define DONE_LABEL_TO_CAP "<a href=\"#recent\">Show recent only</a>"

/* done_recent_cmp() — most recently completed first.                        */
static gint
done_recent_cmp(gconstpointer a, gconstpointer b)
{
    const Task *ta = *(const Task * const *)a;
    const Task *tb = *(const Task * const *)b;
    if (ta->completed_at != tb->completed_at)
        return ta->completed_at > tb->completed_at ? -1 : 1;
    if (ta->updated_at != tb->updated_at)
        return ta->updated_at > tb->updated_at ? -1 : 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * The PLAN: what the board is about to show, decided before touching widgets.
 * lib_refresh_kanban builds one of these per lane and asks whether the board
 * already holds exactly it (kanban_plan_matches).  When it does, only
 * changed labels are rewritten (kanban_plan_relabel) — the fast path.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;
    gchar  *markup;          /* owned */
} CardPlan;

static void
card_plan_add(GArray *lane, const Task *t, const TaskRowCtx *ctx)
{
    GPtrArray *subs = t->parent_id == 0
        ? g_hash_table_lookup(ctx->subs_by_parent, GINT_TO_POINTER(t->id))
        : NULL;
    const gchar *list_name = ctx->list_names != NULL
        ? g_hash_table_lookup(ctx->list_names, GINT_TO_POINTER(t->list_id))
        : NULL;
    gint att = GPOINTER_TO_INT(
        g_hash_table_lookup(ctx->att_counts, GINT_TO_POINTER(t->id)));
    CardPlan cp;
    cp.id     = t->id;
    cp.markup = task_rows_desc_markup(t, list_name, att, subs, ctx);
    g_array_append_val(lane, cp);
}

static void
kanban_plan_free(GArray **plan)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        for (guint i = 0; i < plan[s]->len; i++)
            g_free(g_array_index(plan[s], CardPlan, i).markup);
        g_array_free(plan[s], TRUE);
    }
}

/* kanban_plan_matches() — is the board already showing exactly these cards?
 * Compares IDS only (text is what the fast path updates) plus lane totals. */
static gboolean
kanban_plan_matches(TaskLibrary *lw, GArray * const *plan,
                    const guint *per_lane)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (lw->board.kanban_lanes[s] == NULL ||
            per_lane[s] != lw->board.kanban_counts[s])
            return FALSE;
        GArray  *ids  = lane_card_ids(lw, s);
        gboolean same = ids->len == plan[s]->len;
        for (guint i = 0; same && i < ids->len; i++)
            same = g_array_index(ids, gint64, i) ==
                   g_array_index(plan[s], CardPlan, i).id;
        g_array_unref(ids);
        if (!same)
            return FALSE;
    }
    return TRUE;
}

/* kanban_plan_relabel() — fast path: rewrite only the markup that moved.    */
static void
kanban_plan_relabel(TaskLibrary *lw, GArray * const *plan)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        guint i = 0;
        for (GtkWidget *child =
                 gtk_widget_get_first_child(lw->board.kanban_lanes[s]);
             child != NULL;
             child = gtk_widget_get_next_sibling(child)) {
            if (card_task_id(child) == 0)
                continue;
            const gchar *want = g_array_index(plan[s], CardPlan, i++).markup;
            GtkWidget   *lab  = g_object_get_data(G_OBJECT(child),
                                                   "task-card-label");
            if (lab != NULL &&
                g_strcmp0(gtk_label_get_label(GTK_LABEL(lab)), want) != 0)
                gtk_label_set_markup(GTK_LABEL(lab), want);
        }
    }
}

/* done_expand_idle() — rebuild after the Done lane's link was clicked.      */
static gboolean
done_expand_idle(gpointer data)
{
    TaskLibrary *lw = lib_of(data);
    if (lw != NULL)
        lib_refresh_tasks(lw);
    return G_SOURCE_REMOVE;
}

/* on_done_link_activate() — flip the lane between capped and all-shown.    */
static gboolean
on_done_link_activate(GtkLabel *lbl, gchar *uri, gpointer data)
{
    (void)lbl; (void)uri;
    TaskLibrary *lw = data;
    lw->board.done_show_all = !lw->board.done_show_all;
    g_idle_add(done_expand_idle, lw->app);
    return TRUE;
}

/* done_link_pack() — append the Done lane's expand/collapse link.           */
static void
done_link_pack(TaskLibrary *lw, guint total)
{
    GtkWidget *lbl = gtk_label_new(NULL);
    if (lw->board.done_show_all) {
        gtk_label_set_markup(GTK_LABEL(lbl), DONE_LABEL_TO_CAP);
    } else {
        gchar *m = g_strdup_printf(DONE_LABEL_TO_ALL, total);
        gtk_label_set_markup(GTK_LABEL(lbl), m);
        g_free(m);
    }
    /* gtk_label_set_track_visited_links was removed in GTK4.                */
    gtk_widget_set_margin_top(lbl, 4);
    gtk_widget_set_margin_bottom(lbl, 2);
    g_signal_connect(lbl, "activate-link",
                     G_CALLBACK(on_done_link_activate), lw);
    gtk_box_append(GTK_BOX(lw->board.kanban_lanes[TASK_STATUS_DONE]), lbl);
}

/* ---------------------------------------------------------------------------
 * lib_refresh_kanban() — rebuild the board from `tasks`.
 *
 * Called by lib_refresh_tasks after the task collection, so every view that
 * yields a task list can be shown as a board.
 * ------------------------------------------------------------------------- */
guint
lib_refresh_kanban(TaskLibrary *lw, GPtrArray *tasks, const TaskRowCtx *ctx)
{
    kanban_order_apply(lw, tasks);

    /* ---- Decide, before touching a widget -------------------------------- */
    GArray *plan[TASK_STATUS_N_VALUES];
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        plan[s] = g_array_new(FALSE, FALSE, sizeof(CardPlan));
    guint per_lane[TASK_STATUS_N_VALUES] = { 0 };
    guint shown = 0;

    GPtrArray *done = g_ptr_array_new();

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        gboolean done_task = t->status == TASK_STATUS_DONE;
        if (!ctx->show_done && done_task)
            continue;
        gint lane = (gint)t->status;
        if (lane < 0 || lane >= TASK_STATUS_N_VALUES)
            lane = TASK_STATUS_NEW;
        per_lane[lane]++;
        shown++;
        if (lane == TASK_STATUS_DONE)
            g_ptr_array_add(done, t);
        else
            card_plan_add(plan[lane], t, ctx);
    }

    g_ptr_array_sort(done, done_recent_cmp);
    guint done_total = done->len;
    guint done_cap   = lw->board.done_show_all ? done_total
                                         : MIN(done_total, (guint)DONE_CAP);
    for (guint i = 0; i < done_cap; i++)
        card_plan_add(plan[TASK_STATUS_DONE],
                      g_ptr_array_index(done, i), ctx);
    g_ptr_array_free(done, TRUE);

    /* ---- Fast path: same cards, only text may have moved ---------------- */
    if (kanban_plan_matches(lw, plan, per_lane)) {
        kanban_plan_relabel(lw, plan);
        kanban_plan_free(plan);
        return shown;
    }

    /* ---- Rebuild --------------------------------------------------------- */
    lib_scroll_keep_queue_win(lw->board.kanban_box);

    lw->board.card_mark      = NULL;
    lw->board.card_mark_lane = -1;
    lw->board.card_mark_slot = -1;

    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        lane_clear(lw->board.kanban_lanes[s]);

    GHashTable *alive = g_hash_table_new(NULL, NULL);
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        for (guint i = 0; i < plan[s]->len; i++) {
            const CardPlan *cp       = &g_array_index(plan[s], CardPlan, i);
            gboolean        selected = card_sel_has(lw, cp->id);
            if (selected)
                g_hash_table_add(alive, GSIZE_TO_POINTER((gsize)cp->id));
            gtk_box_append(GTK_BOX(lw->board.kanban_lanes[s]),
                           kanban_card_new(lw, cp->id, cp->markup, selected));
        }
    }
    if (done_total > (guint)DONE_CAP)
        done_link_pack(lw, done_total);
    kanban_plan_free(plan);

    g_hash_table_remove_all(lw->board.kanban_sel);
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, alive);
    while (g_hash_table_iter_next(&it, &key, NULL))
        g_hash_table_add(lw->board.kanban_sel, key);
    g_hash_table_destroy(alive);
    if (card_sel_count(lw) == 0)
        lw->board.kanban_anchor = 0;

    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        gchar *hdr = g_strdup_printf(
            "<b>%s</b>\n<small><span alpha=\"60%%\">%u task%s</span>"
            "</small>", task_status_label((TaskStatus)s), per_lane[s],
            per_lane[s] == 1 ? "" : "s");
        gtk_label_set_markup(GTK_LABEL(lw->board.kanban_labels[s]), hdr);
        g_free(hdr);

        if (per_lane[s] == 0) {
            GtkWidget *empty = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(empty),
                "<i><span alpha=\"55%\">Drop a task here</span></i>");
            gtk_widget_set_margin_top(empty, 10);
            gtk_widget_set_margin_bottom(empty, 10);
            gtk_box_append(GTK_BOX(lw->board.kanban_lanes[s]), empty);
        }
        /* In GTK4, widgets appended to a visible container are visible by
         * default — no gtk_widget_show_all needed.                          */
    }
    memcpy(lw->board.kanban_counts, per_lane, sizeof(per_lane));
    return shown;
}

/* ---------------------------------------------------------------------------
 * kanban_lane_new() — one lane: a heading label over a padded box that holds
 * the cards.
 *
 * GTK4: no GtkEventBox for the drop target (not needed — compute_bounds works
 * on any widget in the layout tree, and GTK4 renders CSS backgrounds on any
 * widget).  No GtkFrame with shadow type (gtk_frame_set_shadow_type was
 * removed in GTK4); the lane's border comes from .task-lane CSS.
 *
 * kanban_drops[status] and kanban_lanes[status] point to the SAME lane box
 * (the drop-target hit-test uses compute_bounds against the box the cards
 * live in, and lane_clear empties that same box).
 * ------------------------------------------------------------------------- */
static GtkWidget *
kanban_lane_new(TaskLibrary *lw, TaskStatus status)
{
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    lw->board.kanban_labels[status] = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(lw->board.kanban_labels[status]),
                          GTK_JUSTIFY_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(lw->board.kanban_labels[status]),
                            PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(col), lw->board.kanban_labels[status]);

    /* The lane box: holds the cards AND is the hit-test target.             */
    GtkWidget *lane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(lane, "task-lane");
    pad_widget(lane, LANE_PAD);
    gtk_widget_set_vexpand(lane, TRUE);

    /* Both pointers to the same widget: kanban_drops for hit-testing,
     * kanban_lanes for card management.                                     */
    lw->board.kanban_drops[status] = lane;
    lw->board.kanban_lanes[status] = lane;

    gtk_box_append(GTK_BOX(col), lane);
    return col;
}

/* ---------------------------------------------------------------------------
 * task_library_apply_kanban_shadow() — the single writer of the cached
 * kanban_shadow flag, called from Settings → Appearance.
 * ------------------------------------------------------------------------- */
void
task_library_apply_kanban_shadow(TaskApp *app, gboolean on)
{
    TaskLibrary *lw = lib_of(app);
    if (lw == NULL)
        return;
    lw->board.card_shadow = on;
    card_shadow_restyle(lw);
}

/* ---------------------------------------------------------------------------
 * task_kanban_build() — build the board into lw->board.kanban_box.
 * ------------------------------------------------------------------------- */
void
task_kanban_build(TaskLibrary *lw)
{
    kanban_css_install();
    GtkWidget *board = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(board), TRUE);
    gtk_widget_set_margin_start(board, 6);
    gtk_widget_set_margin_end(board, 6);
    gtk_widget_set_margin_top(board, 6);
    gtk_widget_set_margin_bottom(board, 6);
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        gtk_box_append(GTK_BOX(board), kanban_lane_new(lw, (TaskStatus)s));

    lw->board.kanban_box = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lw->board.kanban_box),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(
        GTK_SCROLLED_WINDOW(lw->board.kanban_box), board);
}
