/* ===========================================================================
 * kanban.c — the library window's Kanban BOARD: three lanes of cards, the
 * hand-rolled card drag, the per-view card order and the board's CSS (see
 * library_priv.h).
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
 * than list rows.  Lane index IS the status value, so a drop reads its
 * target status straight off the lane it landed on.
 *
 * Cards are real widgets, not cell renderers, because they have to be
 * dragged; the tree view's own row DnD is the thing gotcha 13 says to
 * stay away from on quartz.
 *
 * The drag is HAND-ROLLED — a pointer grab plus a floating ghost window —
 * not GTK's drag-and-drop.  GTK DnD was tried first and rejected
 * (2026-08-25) for one reason: on quartz it hands the gesture to
 * AppKit's NSDraggingSession, which owns the cursor for the duration and
 * paints its own arrow-plus-green-plus badge.  NOTHING in GTK can
 * override that — gdk_window_set_cursor on the card or the toplevel is
 * simply ignored while the session runs — so the closed-hand cursor is
 * unreachable through it.  Owning the gesture gets both halves of what
 * a drag should look like: the grab's own cursor, and a translucent copy
 * of the card itself following the pointer instead of a system badge.
 * It also keeps the board clear of quartz's DnD entirely, which gotchas
 * 12 and 13 both come from.
 *
 * The manual-sort row drag in the task pane works the same way (motion
 * events, no GTK DnD), so this is the established shape here.
 * =========================================================================== */

/* How far the pointer must travel before a press becomes a drag rather
 * than a click.  gtk_drag_check_threshold uses the platform's own value,
 * so a click that wobbles a pixel still selects rather than dragging.     */

/* The ghost's opacity: enough to read the card through it, enough to see
 * the lane underneath.                                                    */
#define CARD_GHOST_ALPHA 0.65

/* Thickness of the insertion marker, in logical px.  Thin on purpose: it
 * occupies a slot in the lane, so anything chunky would shove the cards
 * around as it moves between slots.                                       */
#define CARD_MARK_H 3

/* Breathing room either side of the ⠿ grip glyph.  Narrow on purpose: the
 * grip is a grab target, not a column.                                    */
#define CARD_GRIP_PAD 5

/* Inner insets, applied as WIDGET MARGINS on the child — neither CSS
 * padding nor border_width works on a visible-window GtkEventBox, see the
 * note in kanban_css_install.                                             */
#define CARD_PAD 8               /* card border → its text               */

#define LANE_PAD 6               /* lane frame → the cards inside it     */

/* How many completed tasks the Done lane shows before its "Show All" link.
 * The lane is the only one that grows without bound — a task leaves New and
 * In Progress again, but nothing leaves Done — and every card is ~5 widgets
 * of height-for-width layout, so an uncapped lane is what makes the whole
 * board slow (measured: 1971 cards rebuild in 3.3 s, 438 in 0.69 s).      */
#define DONE_CAP 10

/* pad_widget() — inset a widget from its parent on all four sides.        */
static void
pad_widget(GtkWidget *w, gint pad)
{
    gtk_widget_set_margin_start(w, pad);
    gtk_widget_set_margin_end(w, pad);
    gtk_widget_set_margin_top(w, pad);
    gtk_widget_set_margin_bottom(w, pad);
}

/* kanban_css_install() — the board's look, installed ONCE for the whole
 * screen and keyed off style classes.
 *
 * Screen-wide rather than the per-widget lib_themed_bg_css_apply the float bar
 * and column headers use, for two reasons: there is one provider instead
 * of one per card (a busy board is hundreds), and every color here is a
 * NAMED theme color, so GTK re-resolves them itself on a light/dark switch
 * — the staleness that helper exists to work around only arises because it
 * bakes a resolved literal into its CSS from C.
 * ------------------------------------------------------------------------- */
static void
kanban_css_install(void)
{
    static gboolean done = FALSE;    /* one provider per process            */
    if (done)
        return;
    done = TRUE;
    GtkCssProvider *p = gtk_css_provider_new();
    /* SQUARE corners throughout, matching the Weekly Forecast's framed day
     * sections (a plain GTK_SHADOW_IN GtkFrame, which has none): a rounded
     * tint inside a square frame reads as a mistake, and rounded cards
     * inside that made the board the odd view out.  No border-radius here
     * is deliberate — don't add one back.                                  */
    /* No `padding` here, deliberately.  Both classes land on a
     * GtkEventBox, and a visible-window event box honors NEITHER CSS
     * padding NOR gtk_container_set_border_width for its own size — both
     * were tried, and the card came out exactly as tall as its label
     * (measured 214x15 against a 15 px label), text hard against the
     * border.  The inset is set with WIDGET MARGINS on the child instead,
     * which GTK's size machinery always folds into the preferred size, so
     * the card grows by them and the background and border still paint at
     * the widget's own edge.                                              */
    gtk_css_provider_load_from_data(p,
        ".task-lane {"
        "  background-color: alpha(@theme_fg_color, 0.05);"
        "}"
        ".task-card {"
        "  background-color: @theme_base_color;"
        "  border: 1px solid alpha(@theme_fg_color, 0.22);"
        "}"
        ".task-card:hover {"
        "  border-color: alpha(@theme_fg_color, 0.45);"
        "}"
        /* The landing indicator, in two parts: the lane tint says which
         * COLUMN, the marker bar says which SLOT within it.  .task-lane-target
         * is listed AFTER .task-lane so it wins at equal specificity (both
         * classes sit on the same widget).                               */
        ".task-lane-target {"
        "  background-color: alpha(@theme_selected_bg_color, 0.22);"
        "}"
        ".task-card-mark {"
        "  background-color: @theme_selected_bg_color;"
        "}"
        /* The card's DROP SHADOW, and it is on the WRAPPER rather than on
         * the card, because a visible-window GtkEventBox CANNOT PAINT
         * OUTSIDE ITSELF: an outset box-shadow on .task-card is clipped
         * away entirely and silently.  See gotcha 30 — this is gotcha 18's
         * twin, and the wrapper (a plain GtkBox, no window of its own) is
         * what gives GTK somewhere to extend the clip to.
         *
         * The light is in the UPPER LEFT, so the shadow falls to the
         * BOTTOM and RIGHT only.  That takes an offset that out-reaches
         * the blur: at "0 1px 2px" the blur spreads on all four sides and
         * the card wears a halo.  The 4th length is a NEGATIVE SPREAD,
         * which shrinks the shadow box so the wider blur still clears the
         * top and left edges — measured mean darkening in the 4 px band
         * outside each edge (0-255): top 2.1, left 2.0 against bottom
         * 23.6, right 22.4.
         *
         * It fits without moving anything: the lane pads its cards by
         * LANE_PAD and stacks them 6 px apart, so a 2 px offset with a
         * 3 px blur lands inside the lane and never reaches the frame.  */
        ".task-card-shadow {"
        "  box-shadow: 2px 2px 3px -1px alpha(@theme_fg_color, 0.40);"
        "}"
        /* While its card is in flight the shadow goes AWAY rather than
         * dimming with it: .task-card-dragging fades the card to 40%, and
         * a crisp shadow under a nearly transparent card reads as the
         * shadow having come loose.  Flat here, and the GHOST is the thing
         * that looks lifted.  Listed AFTER .task-card-shadow so it wins at
         * equal specificity — both classes sit on the same widget, the
         * same rule .task-lane-target follows above.                     */
        ".task-card-shadow-flat {"
        "  box-shadow: none;"
        "}"
        /* The ⠿ grip strip down the card's left edge.  Only this area
         * starts a drag, and only it wears the hand cursor — the rest of
         * the card clicks and selects like an ordinary row.               */
        ".task-card-handle:hover {"
        "  background-color: alpha(@theme_fg_color, 0.10);"
        "}"
        /* The original card stays in place while its ghost is carried
         * around, dimmed so it reads as "this is the one in flight".     */
        ".task-card-dragging {"
        "  opacity: 0.40;"
        "}"
        /* The board's stand-in for a tree selection: what Delete Task and
         * the status bar are talking about.                                */
        ".task-card-selected {"
        "  border-color: @theme_selected_bg_color;"
        "  background-color: alpha(@theme_selected_bg_color, 0.16);"
        "}", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* lane_clear() — destroy a lane's cards.  The board's equivalent of the
 * forecast's gtk_list_store_clear: cards are widgets, so emptying a lane
 * means destroying its children (which also drops their drag sources).    */
static void
lane_clear(GtkWidget *lane)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(lane));
    for (GList *k = kids; k != NULL; k = k->next)
        gtk_widget_destroy(GTK_WIDGET(k->data));
    g_list_free(kids);
}

/* ---------------------------------------------------------------------------
 * card_cursor() — one of the board's two cached cursors, built on first
 * use from `name` and kept on `slot`.
 *
 * Returns NULL when the display cannot supply that name, which callers
 * pass straight to gdk_window_set_cursor: the window default is the right
 * fallback, not a guessed stock cursor.  (Same contract as the task
 * view's "ns-resize" cursor.)
 * ------------------------------------------------------------------------- */
static GdkCursor *
card_cursor(GtkWidget *w, GdkCursor **slot, const gchar *name)
{
    if (*slot == NULL)
        *slot = gdk_cursor_new_from_name(gtk_widget_get_display(w), name);
    return *slot;
}

/* card_set_cursor() — point a realized widget's window at `cursor`.        */
static void
card_set_cursor(GtkWidget *w, GdkCursor *cursor)
{
    GdkWindow *win = gtk_widget_get_window(w);
    if (win != NULL)
        gdk_window_set_cursor(win, cursor);
}

/* on_handle_realize() — the ⠿ grip just got its GdkWindow: give it the
 * open hand.  Set on the WINDOW rather than tracked with enter/leave
 * handlers, so hovering costs nothing per motion event and the cursor is
 * simply a property of the grip's own area — which is precisely why the
 * grip is a separate widget: the rest of the card keeps the default
 * arrow because it never gets a cursor of its own.                        */
static void
on_handle_realize(GtkWidget *handle, gpointer data)
{
    TaskLibrary *lw = data;
    card_set_cursor(handle, card_cursor(handle, &lw->board.card_grab, "grab"));
}

static void card_lane_highlight(TaskLibrary *lw, gint lane);

static void card_mark_clear(TaskLibrary *lw);

static GArray *lane_card_ids(TaskLibrary *lw, gint s);

static void on_handle_realize(GtkWidget *handle, gpointer data);

static gboolean on_handle_press(GtkWidget *handle, GdkEventButton *ev,
                                gpointer data);

/* card_task_id() — the task a card stands for (0 if somehow unset).        */
static gint64
card_task_id(GtkWidget *card)
{
    return (gint64)GPOINTER_TO_SIZE(
        g_object_get_data(G_OBJECT(card), "task-task-id"));
}

/* ---------------------------------------------------------------------------
 * card_of() — the CARD inside a lane's child.
 *
 * A lane's children are the shadow WRAPPERS kanban_card_new returns, not
 * the cards themselves, so everything that walks a lane goes through this
 * first: the id, the style classes and the label all live on the card.
 * The wrapper names its card with the same "task-card" key the ⠿ grip
 * uses, and the key means the same thing in both places.
 *
 * Input:
 *   child — any child of a lane box.
 *
 * Output:
 *   the card, or `child` itself when it is not a wrapper — which is what
 *   makes it safe on the insertion marker, the empty-lane placeholder and
 *   the Done lane's "Show All" link, none of which carry a task id, and
 *   idempotent if it is ever handed a card directly.
 * ------------------------------------------------------------------------- */
static GtkWidget *
card_of(GtkWidget *child)
{
    GtkWidget *card = g_object_get_data(G_OBJECT(child), "task-card");
    return card != NULL ? card : child;
}

/* card_shadow_of() — the shadow-carrying wrapper a card sits in, which by
 * construction (kanban_card_new) is its PARENT.  NULL-safe both ways: a
 * card that has been unparented answers NULL.                             */
static GtkWidget *
card_shadow_of(GtkWidget *card)
{
    return card != NULL ? gtk_widget_get_parent(card) : NULL;
}

/* ---------------------------------------------------------------------------
 * on_handle_press() — a press on the ⠿ grip ARMS a drag.
 *
 * Arming rather than dragging immediately is what keeps a click a click:
 * the press only becomes a drag once on_card_motion sees the pointer pass
 * the platform's threshold.
 *
 * Returns FALSE so the press keeps propagating to the CARD, which selects
 * — clicking the grip should select the card like clicking anywhere else
 * on it, and a double-click on the grip should still open the editor.
 *
 * The hot spot is translated into the CARD's coordinates: the ghost is a
 * picture of the whole card, so it has to hang off the pointer where the
 * card was gripped, not where the grip was.
 * ------------------------------------------------------------------------- */
static gboolean
on_handle_press(GtkWidget *handle, GdkEventButton *ev, gpointer data)
{
    TaskLibrary *lw = data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1)
        return FALSE;
    GtkWidget *card = g_object_get_data(G_OBJECT(handle), "task-card");
    gint64 id = card != NULL ? card_task_id(card) : 0;
    if (id == 0)
        return FALSE;

    gint cx = 0, cy = 0;
    gtk_widget_translate_coordinates(handle, card, (gint)ev->x, (gint)ev->y,
                                     &cx, &cy);
    lw->board.card_armed       = TRUE;
    lw->board.card_drag_src    = card;
    lw->board.card_drag_handle = handle;
    lw->board.card_drag_id     = id;
    lw->board.card_press_rx    = ev->x_root;
    lw->board.card_press_ry    = ev->y_root;
    lw->board.card_hot_x       = cx;
    lw->board.card_hot_y       = cy;
    return FALSE;                    /* let the card select as well        */
}

/* card_sel_has() — is `id` selected?                                       */
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
    return lw->board.kanban_sel != NULL ? g_hash_table_size(lw->board.kanban_sel) : 0;
}

/* ---------------------------------------------------------------------------
 * card_restyle() — paint the selection onto the cards.
 *
 * Runs IN PLACE rather than through a refresh: a refresh here would
 * destroy the very widget a drag is about to start from, and the click
 * would never become one.
 * ------------------------------------------------------------------------- */
static void
card_restyle(TaskLibrary *lw)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (lw->board.kanban_lanes[s] == NULL)
            continue;
        GList *kids =
            gtk_container_get_children(GTK_CONTAINER(lw->board.kanban_lanes[s]));
        for (GList *k = kids; k != NULL; k = k->next) {
            GtkWidget *card = card_of(GTK_WIDGET(k->data));
            gint64 id = card_task_id(card);
            if (id == 0)
                continue;            /* the marker / empty placeholder      */
            GtkStyleContext *sc = gtk_widget_get_style_context(card);
            if (card_sel_has(lw, id))
                gtk_style_context_add_class(sc, "task-card-selected");
            else
                gtk_style_context_remove_class(sc, "task-card-selected");
        }
        g_list_free(kids);
    }
}

/* ---------------------------------------------------------------------------
 * card_shadow_restyle() — add or remove .task-card-shadow on every card
 * wrapper currently on the board, from the cached flag.
 *
 * Walks the lanes IN PLACE rather than asking for a refresh, and that is
 * not an optimization: lib_refresh_kanban takes its FAST PATH when the same
 * cards are still showing (kanban_plan_matches), so a refresh would
 * relabel and build nothing, and the setting would appear to do nothing
 * until the board happened to change for some other reason.  The same
 * trap the Google and Notes intervals have, where writing the key without
 * re-arming the worker leaves it taking effect at the next launch.
 *
 * The class goes on the WRAPPER — the lane's child — while the task id
 * lives on the CARD inside it, which is why this reads one and writes the
 * other.  A card in flight is left alone by nothing here:
 * .task-card-shadow-flat is listed after .task-card-shadow and still
 * wins, so a toggle mid-drag cannot put a shadow back under the dragged
 * card.
 * ------------------------------------------------------------------------- */
static void
card_shadow_restyle(TaskLibrary *lw)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (lw->board.kanban_lanes[s] == NULL)
            continue;
        GList *kids =
            gtk_container_get_children(GTK_CONTAINER(lw->board.kanban_lanes[s]));
        for (GList *k = kids; k != NULL; k = k->next) {
            GtkWidget *wrap = GTK_WIDGET(k->data);
            if (card_task_id(card_of(wrap)) == 0)
                continue;            /* marker / placeholder / the link    */
            GtkStyleContext *sc = gtk_widget_get_style_context(wrap);
            if (lw->board.card_shadow)
                gtk_style_context_add_class(sc, "task-card-shadow");
            else
                gtk_style_context_remove_class(sc, "task-card-shadow");
        }
        g_list_free(kids);
    }
}

/* card_select() — collapse the selection to just `id`.                     */
static void
card_select(TaskLibrary *lw, gint64 id)
{
    g_hash_table_remove_all(lw->board.kanban_sel);
    card_sel_add(lw, id);
    lw->board.kanban_anchor = id;
    card_restyle(lw);
}

/* ---------------------------------------------------------------------------
 * lib_card_sel_ids() — the selected ids in BOARD DISPLAY ORDER (lane by lane,
 * top to bottom), skipping any whose card is no longer on screen.
 *
 * Display order rather than hash order so a bulk action reads the way the
 * board looks — and so a multi-card drag keeps the cards' relative order
 * when it re-inserts them.  Free with g_array_unref.
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
 * WITHIN ONE LANE only.  A run down a column is the obvious meaning of
 * shift-click on a board; "everything between" two cards in DIFFERENT
 * columns is not, and would quietly select a screenful.  So a cross-lane
 * shift-click behaves like a modify-click and just adds the card, which is
 * the least surprising thing that is still useful.
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
        if (ai >= 0 && bi >= 0) {    /* both in THIS lane: take the run    */
            gint lo = MIN(ai, bi), hi = MAX(ai, bi);
            for (gint i = lo; i <= hi; i++)
                card_sel_add(lw, g_array_index(lane, gint64, (guint)i));
            g_array_unref(lane);
            card_restyle(lw);
            return;
        }
        g_array_unref(lane);
    }
    card_sel_add(lw, id);            /* different lanes: just add it        */
    card_restyle(lw);
}

/* on_card_press() — click SELECTS; double-click opens the editor;
 * right-click raises the shared context menu.  It does NOT arm a drag:
 * that belongs to the ⠿ grip alone (on_handle_press), so the card body
 * behaves like an ordinary clickable row.  Returns FALSE on the first
 * click of a double so GTK still delivers the second.                     */
static gboolean
on_card_press(GtkWidget *card, GdkEventButton *ev, gpointer data)
{
    TaskLibrary *lw = data;
    gint64 id = card_task_id(card);
    if (id == 0)
        return FALSE;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1) {
        lib_card_drag_stop(lw);          /* the first press armed one          */
        task_editor_open(lw->app, id);
        return TRUE;
    }
    /* ---- selection -----------------------------------------------------
     * The two modifiers come from GTK, not hardcoded: MODIFY_SELECTION is
     * Ctrl on X11 and Cmd on quartz, and asking the widget is the only way
     * to be right on both.
     *
     * A RIGHT-click inside an existing selection LEAVES IT ALONE, so a
     * bulk action can be reached from any of the selected cards — the same
     * rule the task view follows.  Outside it, it collapses first.        */
    GdkModifierType mod_mask = gtk_widget_get_modifier_mask(card,
        GDK_MODIFIER_INTENT_MODIFY_SELECTION);
    GdkModifierType ext_mask = gtk_widget_get_modifier_mask(card,
        GDK_MODIFIER_INTENT_EXTEND_SELECTION);
    gboolean modify = (ev->state & mod_mask) != 0;
    gboolean extend = (ev->state & ext_mask) != 0;

    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        if (!card_sel_has(lw, id))
            card_select(lw, id);
        /* The SAME menu the list view's rows show, from the same function
         * against the same selection — so a multi-selection gets the
         * multi variant ("Delete 3 Tasks") for free.  Anchored to
         * kanban_box, never the card: an attached menu dies with its
         * widget, and every action here refreshes the board and destroys
         * the card underneath it.                                        */
        return task_context_menu_popup(lw, lw->board.kanban_box, ev);
    }

    if (ev->type == GDK_BUTTON_PRESS) {
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
            /* A plain click INSIDE the selection keeps it: that is what
             * lets a multi-card drag start from any of its cards.  The
             * collapse happens on release instead (on_card_release), only
             * when no drag took place.
             *
             * The ANCHOR moves either way.  It has to: it means "the last
             * card plainly clicked", and a shift-click straight after this
             * one must measure its run from HERE.  Tying it to the
             * collapse instead left it pointing at a card the user last
             * touched several clicks ago, and the run came out wrong.    */
            if (!card_sel_has(lw, id))
                card_select(lw, id);
            else
                lw->board.kanban_anchor = id;
        }
    }

    return FALSE;
}

/* card_lane_at_root() — which lane's drop box contains this ROOT point,
 * or -1.  Root coordinates because the pointer spends the drag over other
 * widgets, and every lane box is realized, so each has a window origin to
 * measure from.                                                            */
static gint
card_lane_at_root(TaskLibrary *lw, gint rx, gint ry)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *box = lw->board.kanban_drops[s];
        if (box == NULL || !gtk_widget_get_mapped(box))
            continue;
        GdkWindow *win = gtk_widget_get_window(box);
        if (win == NULL)
            continue;
        gint ox, oy;
        gdk_window_get_origin(win, &ox, &oy);
        GtkAllocation a;
        gtk_widget_get_allocation(box, &a);
        if (rx >= ox && rx < ox + a.width && ry >= oy && ry < oy + a.height)
            return s;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * on_ghost_draw() — paint the ghost: the snapshot, at CARD_GHOST_ALPHA.
 *
 * The window is app-paintable and draws NOTHING else, which is what keeps
 * the theme's own window background from showing as a grey plate around
 * the card.  On a composited screen the surface is cleared to fully
 * transparent first (OPERATOR_SOURCE, so it replaces rather than blends)
 * and the card painted over it with alpha, giving real see-through.
 * Without a compositor that clear would land as BLACK, so there the card
 * is painted opaque instead — a solid card that follows the pointer,
 * which is the honest degradation rather than a black rectangle.
 * ------------------------------------------------------------------------- */
static gboolean
on_ghost_draw(GtkWidget *ghost, cairo_t *cr, gpointer data)
{
    (void)data;
    cairo_surface_t *surf = g_object_get_data(G_OBJECT(ghost), "task-surface");
    if (surf == NULL)
        return FALSE;
    gboolean composited = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(ghost), "task-composited"));
    if (composited) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint_with_alpha(cr, composited ? CARD_GHOST_ALPHA : 1.0);

    /* More than one card in flight?  Say so ON the ghost.  A snapshot of
     * the gripped card alone would claim a single-card move, and the drop
     * is about to touch several.                                          */
    gint n = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ghost), "task-n"));
    if (n > 1) {
        gchar *txt = g_strdup_printf("%d", n);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13.0);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, txt, &ext);
        gdouble pad = 5.0;
        gdouble w = ext.width + pad * 2, h = 18.0;
        gint aw = gtk_widget_get_allocated_width(ghost);
        gdouble bx = aw - w - 4.0, by = 4.0;
        cairo_set_source_rgba(cr, 0.18, 0.36, 0.75, 0.95);
        cairo_rectangle(cr, bx, by, w, h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, bx + pad - ext.x_bearing,
                      by + (h - ext.height) / 2 - ext.y_bearing);
        cairo_show_text(cr, txt);
        g_free(txt);
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * card_ghost_new() — a translucent copy of `card` in a popup window.
 *
 * The snapshot is drawn into a surface made from the card's OWN window, so
 * it inherits the display's scale factor and stays sharp on HiDPI.
 *
 * Translucency is a PAINTED alpha on an RGBA visual, not
 * gtk_widget_set_opacity: window opacity is a compositor feature that
 * several X11 setups (and quartz popups) quietly ignore, which is exactly
 * how this shipped opaque the first time.  Painting it ourselves also
 * means the window has no background of its own to leak round the edges.
 * ------------------------------------------------------------------------- */
static GtkWidget *
card_ghost_new(GtkWidget *card, gint n_moving)
{
    GtkAllocation a;
    gtk_widget_get_allocation(card, &a);
    GdkWindow *cw = gtk_widget_get_window(card);
    if (cw == NULL || a.width <= 0 || a.height <= 0)
        return NULL;

    cairo_surface_t *surf = gdk_window_create_similar_surface(
        cw, CAIRO_CONTENT_COLOR_ALPHA, a.width, a.height);
    cairo_t *cr = cairo_create(surf);
    gtk_widget_draw(card, cr);
    cairo_destroy(cr);

    GtkWidget *ghost = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_type_hint(GTK_WINDOW(ghost), GDK_WINDOW_TYPE_HINT_DND);
    gtk_widget_set_app_paintable(ghost, TRUE);   /* no theme background   */

    /* An RGBA visual is what makes per-pixel alpha possible at all; a
     * screen with no compositor running cannot honor it, and the draw
     * handler falls back to opaque rather than painting onto black.      */
    GdkScreen *screen = gtk_widget_get_screen(ghost);
    GdkVisual *rgba   = gdk_screen_get_rgba_visual(screen);
    gboolean composited = (rgba != NULL && gdk_screen_is_composited(screen));
    if (composited)
        gtk_widget_set_visual(ghost, rgba);

    g_object_set_data_full(G_OBJECT(ghost), "task-surface", surf,
                           (GDestroyNotify)cairo_surface_destroy);
    g_object_set_data(G_OBJECT(ghost), "task-composited",
                      GINT_TO_POINTER(composited));
    g_object_set_data(G_OBJECT(ghost), "task-n",
                      GINT_TO_POINTER(n_moving));
    g_signal_connect(ghost, "draw", G_CALLBACK(on_ghost_draw), NULL);
    gtk_widget_set_size_request(ghost, a.width, a.height);
    gtk_widget_show(ghost);
    return ghost;
}

/* lib_card_drag_stop() — end a drag (or a merely armed press) and put
 * everything back.  Safe to call when nothing is in flight.               */
void
lib_card_drag_stop(TaskLibrary *lw)
{
    if (lw->board.card_dragging) {
        GdkDisplay *dpy = gtk_widget_get_display(lw->window);
        gdk_seat_ungrab(gdk_display_get_default_seat(dpy));
        /* Put the window cursors back.  The card keeps the OPEN hand (the
         * pointer may still be over it); the toplevel goes back to its
         * default so every other widget inherits normally again.          */
        card_set_cursor(lw->window, NULL);
        /* The grip keeps the OPEN hand (the pointer may still be over it);
         * the card loses its dimming.  Two different widgets, so two
         * different restorations.                                         */
        if (lw->board.card_drag_handle != NULL)
            card_set_cursor(lw->board.card_drag_handle,
                            card_cursor(lw->board.card_drag_handle,
                                        &lw->board.card_grab, "grab"));
        if (lw->board.card_drag_src != NULL) {
            gtk_style_context_remove_class(
                gtk_widget_get_style_context(lw->board.card_drag_src),
                "task-card-dragging");
            GtkWidget *sh = card_shadow_of(lw->board.card_drag_src);
            if (sh != NULL)
                gtk_style_context_remove_class(
                    gtk_widget_get_style_context(sh),
                    "task-card-shadow-flat");
        }
        card_lane_highlight(lw, -1);
        card_mark_clear(lw);
    }
    if (lw->board.card_key_handler != 0) {
        g_signal_handler_disconnect(lw->window, lw->board.card_key_handler);
        lw->board.card_key_handler = 0;
    }
    g_clear_pointer(&lw->board.card_ghost, gtk_widget_destroy);
    lw->board.card_dragging    = FALSE;
    lw->board.card_armed       = FALSE;
    lw->board.card_drag_src    = NULL;
    lw->board.card_drag_handle = NULL;
    lw->board.card_drag_id     = 0;
}

/* kanban_order_key() — the current view's card-order key, or NULL for a
 * view that has no board (the forecast).  New string (g_free).            */
static gchar *
kanban_order_key(TaskLibrary *lw)
{
    gchar *key = lib_row_order_key("kanban_order", lw->sel_kind, lw->sel_id);
    if (key != NULL)
        return key;
    return task_view_order_key(lib_sel_view(lw), "kanban_order");
}

/* ---------------------------------------------------------------------------
 * kanban_order_apply() — reorder `tasks` in place to match the saved
 * card order for the current view (see lib_row_order_permutation).
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

    /* Same elements, new sequence — the array does not own the tasks, so
     * this is a pure permutation and nothing is freed.  The copy is what
     * makes it safe to read and write pdata in one pass.                 */
    gpointer *was = g_new(gpointer, n);
    memcpy(was, tasks->pdata, sizeof(gpointer) * (gsize)n);
    for (gint i = 0; i < n; i++)
        tasks->pdata[i] = was[order[i]];
    g_free(was);
    g_free(order);
}

/* lane_card_ids() — the task ids currently shown in lane `s`, in display
 * order, skipping the marker and the empty-lane placeholder (neither
 * carries a task id).  Free with g_array_unref.                           */
static GArray *
lane_card_ids(TaskLibrary *lw, gint s)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    if (lw->board.kanban_lanes[s] == NULL)
        return ids;
    GList *kids = gtk_container_get_children(
        GTK_CONTAINER(lw->board.kanban_lanes[s]));
    for (GList *k = kids; k != NULL; k = k->next) {
        gint64 id = card_task_id(card_of(GTK_WIDGET(k->data)));
        if (id != 0)
            g_array_append_val(ids, id);
    }
    g_list_free(kids);
    return ids;
}

/* ---------------------------------------------------------------------------
 * card_slot_at() — which SLOT in lane `s` the pointer at root-y `ry` is
 * pointing at: 0 before the first card, n after the last.
 *
 * Measured against each card's vertical MIDPOINT, and the dragged card is
 * counted like any other so the slot it already occupies is reachable
 * (that is what makes "put it back" a no-op rather than a move).  The
 * marker carries no task id and is skipped.
 * ------------------------------------------------------------------------- */
static gint
card_slot_at(TaskLibrary *lw, gint s, gint ry)
{
    gint slot = 0;
    if (lw->board.kanban_lanes[s] == NULL)
        return 0;
    GList *kids = gtk_container_get_children(
        GTK_CONTAINER(lw->board.kanban_lanes[s]));
    for (GList *k = kids; k != NULL; k = k->next) {
        /* The CARD, not the lane child: the wrapper has no window of its
         * own, so gtk_widget_get_window would answer the LANE's window and
         * every card would report the lane's origin as its top.           */
        GtkWidget *w = card_of(GTK_WIDGET(k->data));
        if (card_task_id(w) == 0)
            continue;                /* marker / placeholder               */
        GdkWindow *win = gtk_widget_get_window(w);
        if (win == NULL)
            continue;
        gint ox, oy;
        gdk_window_get_origin(win, &ox, &oy);
        (void)ox;
        GtkAllocation a;
        gtk_widget_get_allocation(w, &a);
        if (ry < oy + a.height / 2)
            break;                   /* above this card's middle           */
        slot++;
    }
    g_list_free(kids);
    return slot;
}

/* card_mark_clear() — take the insertion marker off screen.                */
static void
card_mark_clear(TaskLibrary *lw)
{
    g_clear_pointer(&lw->board.card_mark, gtk_widget_destroy);
    lw->board.card_mark_lane = -1;
    lw->board.card_mark_slot = -1;
}

/* ---------------------------------------------------------------------------
 * card_mark_place() — show the insertion marker at (lane, slot).
 *
 * Rebuilt on a CHANGE only, never per motion event: the marker takes up
 * room in the lane, so re-inserting it on every event would shuffle the
 * cards under the pointer continuously.  Rebuilding rather than
 * reparenting keeps the ref juggling out of it — gtk_container_remove
 * would drop the last reference and destroy the thing we meant to move.
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

    GtkWidget *mark = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mark),
                                "task-card-mark");
    gtk_widget_set_size_request(mark, -1, CARD_MARK_H);
    gtk_box_pack_start(GTK_BOX(lw->board.kanban_lanes[lane]), mark,
                       FALSE, FALSE, 0);
    /* Translate the CARD slot into a child index: the placeholder label
     * of an empty lane is a child too, so count real cards.               */
    gint child_idx = 0, seen = 0;
    GList *kids = gtk_container_get_children(
        GTK_CONTAINER(lw->board.kanban_lanes[lane]));
    for (GList *k = kids; k != NULL; k = k->next, child_idx++) {
        GtkWidget *w = GTK_WIDGET(k->data);
        if (w == mark)
            continue;
        if (card_task_id(card_of(w)) != 0) {
            if (seen == slot)
                break;
            seen++;
        }
    }
    g_list_free(kids);
    gtk_box_reorder_child(GTK_BOX(lw->board.kanban_lanes[lane]), mark, child_idx);
    gtk_widget_show(mark);

    lw->board.card_mark      = mark;
    lw->board.card_mark_lane = lane;
    lw->board.card_mark_slot = slot;
}

/* card_lane_highlight() — mark the lane the card would land in, and only
 * that one.  `lane` of -1 clears every highlight (pointer outside the
 * board, or the drag ending).                                             */
static void
card_lane_highlight(TaskLibrary *lw, gint lane)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GtkWidget *box = lw->board.kanban_drops[s];
        if (box == NULL)
            continue;
        GtkStyleContext *sc = gtk_widget_get_style_context(box);
        if (s == lane)
            gtk_style_context_add_class(sc, "task-lane-target");
        else
            gtk_style_context_remove_class(sc, "task-lane-target");
    }
}

/* card_drag_move() — put the ghost under the pointer, offset so the card
 * stays gripped where it was picked up, and light up the lane it would
 * land in so the drop is never a guess.                                   */
static void
card_drag_move(TaskLibrary *lw, gint rx, gint ry)
{
    if (lw->board.card_ghost != NULL)
        gtk_window_move(GTK_WINDOW(lw->board.card_ghost),
                        rx - lw->board.card_hot_x, ry - lw->board.card_hot_y);
    gint lane = card_lane_at_root(lw, rx, ry);
    card_lane_highlight(lw, lane);
    /* No insertion bar over Done: that lane sorts itself by completion, so
     * a slot marker there would promise a landing position the drop then
     * ignores.  The lane TINT still says the card is going there, which is
     * the part that is true.                                              */
    if (lane == TASK_STATUS_DONE) {
        card_mark_clear(lw);
        return;
    }
    /* The marker is placed BEFORE the slot is read back at drop time, so
     * what the user sees is exactly what the release will do.             */
    card_mark_place(lw, lane, lane >= 0 ? card_slot_at(lw, lane, ry) : -1);
}

/* on_card_drag_key() — Escape abandons the drag, changing nothing.        */
static gboolean
on_card_drag_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    TaskLibrary *lw = data;
    if (ev->keyval != GDK_KEY_Escape)
        return FALSE;
    lib_card_drag_stop(lw);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * card_order_save() — the ORDER half of a board drop: rewrite the view's
 * kanban_order_<id> key with every lane's cards in display order, the
 * dragged ids lifted out of wherever they were and re-inserted into `lane`
 * at `slot` with their relative order intact.  Removing before inserting is
 * what makes `slot` — measured against the cards on screen, the dragged
 * ones included — land where the marker was.
 *
 * Returns TRUE when the saved order actually changed.
 *
 * REFUSES OUTRIGHT while a search is up, and that is the point of it being
 * separate: lane_card_ids reads the cards ON SCREEN, which under a filter
 * is only the matches, so writing that back would drop every hidden task
 * out of the saved order for good.  Same trap as the list view's drag, and
 * lib_manual_sort_live carries the full reasoning.  The caller's STATUS half
 * still runs — dragging a card to Done while searching is a perfectly good
 * thing to do and loses nothing.
 *   lw      — the library window.
 *   to_move — the dragged task ids, in the order they should land.
 *   lane    — the destination lane (its index IS the TaskStatus).
 *   slot    — the position within that lane.
 * ------------------------------------------------------------------------- */
static gboolean
card_order_save(TaskLibrary *lw, GArray *to_move, gint lane, gint slot)
{
    if (lw->search != NULL)
        return FALSE;                /* filtered: not ours to rewrite      */

    GString *order = g_string_new(NULL);
    for (gint sl = 0; sl < TASK_STATUS_N_VALUES; sl++) {
        /* Done contributes NOTHING to the key: it sorts itself by
         * completion and has no hand-made order to preserve (see the
         * Done-lane banner).  Skipping it is also what keeps a CAPPED
         * lane from truncating the saved order — its on-screen cards are
         * only the most recent few, and writing those back would drop
         * every other completed task out of the key, the same trap a
         * search springs.                                                */
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
 * card_drop_apply() — the drop: put the dragged task(s) in `lane` at
 * `slot`, keeping their relative order.  `moving` is the whole dragged
 * selection, so one card and twenty take the same path.
 *
 * Two independent halves, either of which may be a no-op:
 *
 *   the STATUS, when the lane changed — a real database write that stamps
 *     updated_at and syncs;
 *   the ORDER, always — local-only, config, never touches the row, and
 *     handed to card_order_save, which refuses it while a search is up.
 *
 * A drag that lands the cards exactly where they already were does
 * NEITHER, which is what keeps "pick up and put back" from buying a sync
 * round trip.  Returns TRUE when anything changed (so the caller
 * refreshes).
 * ------------------------------------------------------------------------- */
static gboolean
card_drop_apply(TaskLibrary *lw, GArray *moving, gint lane, gint slot)
{
    if (moving == NULL || moving->len == 0 ||
        lane < 0 || lane >= TASK_STATUS_N_VALUES)
        return FALSE;
    TaskStatus want = (TaskStatus)lane;

    /* Which of the dragged tasks actually need a status write?  A
     * multi-card drag routinely mixes lanes, and only the ones arriving
     * from elsewhere are a real change.                                   */
    GArray *to_move = g_array_new(FALSE, FALSE, sizeof(gint64));
    gchar  *one_title = NULL;        /* for the single-task status message */
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

    /* The ORDER half — but never for Done, which sorts itself by
     * completion.  A drag landing there has nothing to write, so a move
     * within the lane changes nothing at all and buys no rebuild.        */
    gboolean order_change = lane != TASK_STATUS_DONE
                          ? card_order_save(lw, to_move, lane, slot)
                          : FALSE;

    if (n_status == 0 && !order_change) {
        g_array_unref(to_move);
        g_free(one_title);
        return FALSE;                /* put back exactly where it was      */
    }

    for (guint i = 0; i < to_move->len; i++)
        task_db_task_set_status(lw->app->db,
                                g_array_index(to_move, gint64, i), want);

    /* Keep the moved cards selected across the rebuild.                   */
    g_hash_table_remove_all(lw->board.kanban_sel);
    for (guint i = 0; i < to_move->len; i++)
        card_sel_add(lw, g_array_index(to_move, gint64, i));
    lw->board.kanban_anchor = g_array_index(to_move, gint64, 0);

    /* Only announce a STATUS move: a reorder is its own feedback (the
     * cards are visibly somewhere else) and would otherwise spam the
     * status bar for every nudge within a lane.                           */
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
 * Deferred deliberately: the drop happens inside the dragged CARD's own
 * event handler, and lib_full_refresh destroys every card including that one,
 * so refreshing inline would return into a freed widget.  Re-resolves the
 * library (the window may close first) rather than capturing it, the same
 * rule every async callback here follows.                                 */
static gboolean
card_refresh_idle(gpointer data)
{
    TaskLibrary *lw = lib_of(data);
    if (lw != NULL)
        lib_full_refresh(lw);
    return G_SOURCE_REMOVE;
}

/* on_card_motion() — start the drag once the pointer has travelled far
 * enough, then track it.  Connected to the ⠿ GRIP, so `w` is the grip and
 * the grab lands on its window — which is what makes the grip the only
 * place a drag can begin.                                                  */
static gboolean
on_card_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    TaskLibrary *lw = data;
    if (!lw->board.card_armed && !lw->board.card_dragging)
        return FALSE;

    if (!lw->board.card_dragging) {
        if (!gtk_drag_check_threshold(w,
                (gint)lw->board.card_press_rx, (gint)lw->board.card_press_ry,
                (gint)ev->x_root, (gint)ev->y_root))
            return FALSE;            /* still just a click                  */

        /* The ghost is a picture of the whole CARD; the grab goes on the
         * GRIP, which is the window the press came from and therefore the
         * one motion and release will be delivered to.                    */
        GtkWidget *card = lw->board.card_drag_src;
        /* How many cards this drag will move: the whole selection when
         * the gripped card is in it, else just the one.                    */
        gint n_moving = card_sel_has(lw, lw->board.card_drag_id)
                        ? (gint)card_sel_count(lw) : 1;
        lw->board.card_ghost = card_ghost_new(card, n_moving);
        GdkDisplay *dpy  = gtk_widget_get_display(w);
        GdkSeat    *seat = gdk_display_get_default_seat(dpy);
        GdkCursor  *grabbing =
            card_cursor(w, &lw->board.card_grabbing, "grabbing");
        if (gdk_seat_grab(seat, gtk_widget_get_window(w),
                          GDK_SEAT_CAPABILITY_ALL_POINTING, FALSE,
                          grabbing, (GdkEvent *)ev, NULL,
                          NULL) != GDK_GRAB_SUCCESS) {
            lib_card_drag_stop(lw);      /* no grab: stay a click               */
            return FALSE;
        }
        /* Belt AND braces on the closed hand.  The grab's cursor argument
         * is the portable lever and is what X11 honors; some backends
         * apply the CURSOR OF THE WINDOW the pointer is over instead, so
         * the same cursor goes on the grip and on the toplevel as well.
         * Setting all three costs nothing and leaves no backend showing
         * an arrow mid-drag.  lib_card_drag_stop puts them all back.          */
        card_set_cursor(w, grabbing);
        card_set_cursor(lw->window, grabbing);
        /* Dim the original in place — the ghost is the one moving.  It is
         * NOT hidden: its GdkWindow is the grab window, and unmapping
         * that would break the grab and end the drag on the spot.        */
        gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                    "task-card-dragging");
        /* ... and take its SHADOW away with the same gesture: a crisp
         * shadow under a card faded to 40% reads as the shadow having come
         * loose from it.  The ghost is what should look lifted.          */
        GtkWidget *sh = card_shadow_of(card);
        if (sh != NULL)
            gtk_style_context_add_class(gtk_widget_get_style_context(sh),
                                        "task-card-shadow-flat");
        lw->board.card_mark_lane = -1;     /* force the first placement          */
        lw->board.card_mark_slot = -1;
        lw->board.card_dragging  = TRUE;
        lw->board.card_key_handler =
            g_signal_connect(lw->window, "key-press-event",
                             G_CALLBACK(on_card_drag_key), lw);
    }
    card_drag_move(lw, (gint)ev->x_root, (gint)ev->y_root);
    return TRUE;
}

/* on_card_release() — drop: whichever lane the pointer is over wins.      */
static gboolean
on_card_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    TaskLibrary *lw = data;
    if (!lw->board.card_dragging) {
        /* A plain click that never became a drag.  The PRESS deliberately
         * left an existing multi-selection alone (so a drag could start
         * from any of its cards); now that we know it was only a click,
         * collapse to the clicked card — unless a modifier was held, which
         * means the press already did the right thing.                    */
        if (lw->board.card_armed && lw->board.card_drag_id != 0 && ev->button == 1) {
            GdkModifierType mod = gtk_widget_get_modifier_mask(w,
                GDK_MODIFIER_INTENT_MODIFY_SELECTION);
            GdkModifierType ext = gtk_widget_get_modifier_mask(w,
                GDK_MODIFIER_INTENT_EXTEND_SELECTION);
            if ((ev->state & (mod | ext)) == 0 && card_sel_count(lw) > 1)
                card_select(lw, lw->board.card_drag_id);
        }
        lw->board.card_armed = FALSE;
        return FALSE;
    }
    gint   lane = card_lane_at_root(lw, (gint)ev->x_root, (gint)ev->y_root);
    /* Read the slot from the MARKER, not by re-measuring: the marker is
     * what the user was looking at, and re-measuring now would answer
     * against a lane whose geometry the marker itself has shifted.        */
    gint   slot = (lane >= 0 && lane == lw->board.card_mark_lane)
                  ? lw->board.card_mark_slot
                  : (lane >= 0 ? card_slot_at(lw, lane, (gint)ev->y_root)
                               : -1);
    /* WHAT moves: the whole selection when the gripped card is part of it,
     * otherwise just that card.  Snapshot it BEFORE lib_card_drag_stop, which
     * clears the drag state.                                              */
    GArray *moving = lib_card_sel_ids(lw);
    if (moving->len == 0 ||
        !card_sel_has(lw, lw->board.card_drag_id)) {
        g_array_set_size(moving, 0);
        g_array_append_val(moving, lw->board.card_drag_id);
    }
    lib_card_drag_stop(lw);              /* ungrab BEFORE touching the model   */
    if (card_drop_apply(lw, moving, lane, slot))
        g_idle_add(card_refresh_idle, lw->app);
    g_array_unref(moving);
    return TRUE;
}

/* on_card_grab_broken() — the compositor or another grab took the pointer
 * away mid-drag; abandon quietly rather than leaving a ghost on screen.   */
static gboolean
on_card_grab_broken(GtkWidget *w, GdkEventGrabBroken *ev, gpointer data)
{
    (void)w; (void)ev;
    lib_card_drag_stop(data);
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * kanban_card_new() — one task as a card: the same Pango markup the list
 * rows and the forecast use (so a task reads identically in all three
 * views), wrapped in an event box that can be clicked and dragged.
 *
 * RETURNS THE SHADOW WRAPPER, not the card: see the note at the foot of
 * this function.  Callers pack what they are given and reach the card
 * through card_of(), which is the one place that indirection is spelled.
 * ------------------------------------------------------------------------- */
static GtkWidget *
kanban_card_new(TaskLibrary *lw, gint64 id, const gchar *markup,
                gboolean selected)
{
    GtkWidget *card = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(card), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                "task-card");
    if (selected)
        gtk_style_context_add_class(gtk_widget_get_style_context(card),
                                    "task-card-selected");
    g_object_set_data(G_OBJECT(card), "task-task-id",
                      GSIZE_TO_POINTER((gsize)id));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(card), row);

    /* The ⠿ GRIP.  Its own event box, because a different cursor needs a
     * different GdkWindow — and because it is the only place a drag may
     * start from, exactly like the list view's handle column.  The glyph
     * is dimmed with Pango ALPHA, never a fixed gray: a gray stays gray
     * on the selection tint and goes unreadable.                          */
    GtkWidget *handle = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(handle), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(handle),
                                "task-card-handle");
    GtkWidget *grip = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(grip),
                         "<span alpha=\"55%\">\xe2\xa0\xbf</span>");
    gtk_widget_set_margin_start(grip, CARD_GRIP_PAD);
    gtk_widget_set_margin_end(grip, CARD_GRIP_PAD);
    gtk_container_add(GTK_CONTAINER(handle), grip);
    gtk_box_pack_start(GTK_BOX(row), handle, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    /* ELLIPSIZED, NOT WRAPPED, and that is a performance decision rather
     * than a typographic one.  A wrapping label is height-for-width: its
     * height cannot be known until its width is, so every size negotiation
     * re-runs a Pango layout for every card on the board.  Measured in
     * this app over 1936 cards, the board took 1641 ms to settle wrapped
     * against 573 ms ellipsized — and that cost is paid on every rebuild:
     * switching views, a drop, a sync pull.  The price is that a title
     * longer than the lane is cut with an ellipsis instead of running on
     * to a second line, which was weighed and accepted (2026-09-02).
     *
     * The cell markup is MULTI-LINE (title, "in <list>", a notes preview,
     * subtasks), and that keeps working because nothing here calls
     * gtk_label_set_lines: the layout's height stays 0, which is what
     * makes Pango ellipsize each paragraph SEPARATELY rather than cutting
     * the card off after its first line.  Setting a line count would
     * quietly turn every card into a one-line card.
     *
     * max_width_chars stays for the reason it was added: it caps the
     * label's NATURAL width, so a 200-character title cannot push the
     * board wider than the (horizontally unscrollable) viewport.  It does
     * NOT cap the allocation — the lane is homogeneous and the label fills
     * it, so the ellipsis lands at the lane's edge, not at 22 characters.  */
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
    pad_widget(label, CARD_PAD);     /* text off the card's border        */
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    /* Remembered so kanban_plan_relabel can reach it: the card is an event
     * box wrapping a box, and walking down to the label per refresh would
     * be one more place that knows this card's shape.                     */
    g_object_set_data(G_OBJECT(card), "task-card-label", label);

    /* The CARD takes clicks: select, double-click to open, right-click for
     * the context menu.  It gets NO cursor, so the pointer stays the
     * ordinary arrow over the text.                                       */
    gtk_widget_add_events(card, GDK_BUTTON_PRESS_MASK |
                                GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(card, "button-press-event",
                     G_CALLBACK(on_card_press), lw);
    /* Release on the card too, not just the grip: press the grip, drift a
     * couple of pixels onto the text, let go — without a grab that release
     * lands HERE, and the armed flag would otherwise be left set.          */
    g_signal_connect(card, "button-release-event",
                     G_CALLBACK(on_card_release), lw);

    /* The GRIP takes the drag.  Press arms, motion past the threshold
     * starts it, release drops.  Its press handler returns FALSE so the
     * card still sees it and selects — clicking the grip selects too.
     * "realize" rather than a one-off call: there is no GdkWindow to put a
     * cursor on until then, which happens after lib_refresh_kanban's show_all
     * (and not at all while the board is hidden).  The CLOSED hand comes
     * from the pointer grab in on_card_motion.                            */
    gtk_widget_add_events(handle, GDK_BUTTON_PRESS_MASK |
                                  GDK_BUTTON_RELEASE_MASK |
                                  GDK_BUTTON1_MOTION_MASK);
    g_object_set_data(G_OBJECT(handle), "task-card", card);
    g_signal_connect(handle, "button-press-event",
                     G_CALLBACK(on_handle_press), lw);
    g_signal_connect(handle, "motion-notify-event",
                     G_CALLBACK(on_card_motion), lw);
    g_signal_connect(handle, "button-release-event",
                     G_CALLBACK(on_card_release), lw);
    g_signal_connect(handle, "grab-broken-event",
                     G_CALLBACK(on_card_grab_broken), lw);
    g_signal_connect(handle, "realize",
                     G_CALLBACK(on_handle_realize), lw);

    /* THE SHADOW'S CARRIER, and the reason this returns a wrapper rather
     * than the card: the card is a visible-window GtkEventBox, and such a
     * widget cannot paint outside its own GdkWindow — an outset
     * box-shadow on it is clipped away with no warning (gotcha 30).  A
     * plain GtkBox has NO window of its own, so GTK extends its clip to
     * cover the shadow and it lands in the lane's gap where it belongs.
     *
     * It costs nothing in layout: a vertical GtkBox gives its child the
     * full width and its own natural height, so the card is exactly the
     * size and place it was before.  What it does cost is one indirection
     * for everything that walks a lane — hence card_of().               */
    GtkWidget *shadow = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* The WRAPPER IS ALWAYS BUILT, shadow or no shadow, and that is what
     * makes "off" genuinely give the old performance back: a wrapper
     * carrying no shadow class measured 0.83 ms against the pre-shadow
     * 0.82 over a 12-card lane, i.e. free — the whole cost was ever the
     * BLUR (gotcha 30).  Building it either way also keeps ONE widget
     * shape for card_of() and the drag code to know about, instead of a
     * board whose tree depends on a setting.                             */
    if (lw->board.card_shadow)
        gtk_style_context_add_class(gtk_widget_get_style_context(shadow),
                                    "task-card-shadow");
    g_object_set_data(G_OBJECT(shadow), "task-card", card);
    gtk_box_pack_start(GTK_BOX(shadow), card, FALSE, FALSE, 0);
    return shadow;
}

/* ---------------------------------------------------------------------------
 * The Done lane, which is the one lane with NO hand-made order.
 *
 * Nothing ever leaves Done, so a position in it is not something a user
 * maintains — "what did I just finish?" is the only question that lane
 * answers, and completed_at answers it exactly.  Two consequences follow
 * and are spelled out where they bite: card_order_save never writes the
 * lane, and card_drag_move shows no insertion bar over it.
 *
 * It is also the only lane that grows without bound, which is what made
 * the board slow: every card is ~5 widgets of height-for-width layout, so
 * a full rebuild measured 3.3 s at 1971 cards against 0.69 s at 438.  The
 * cap is what keeps the lane a fixed cost; the link below lifts it.
 * ------------------------------------------------------------------------- */

/* Named for what a click DOES, the *_LABEL_TO_* idiom the View menu's
 * items follow.  The capped face carries the count, so a collapsed lane
 * never hides an unknown quantity.
 *
 * A real Pango <a> link, not a hand-underlined label: GtkLabel then paints
 * it in the THEME's link color rather than the row's text color, gives it
 * the pointer cursor and keyboard activation on its own, and emits
 * "activate-link".  The href is never followed (the handler returns TRUE),
 * so it only has to be non-empty — it names the state it moves to purely
 * so the markup reads.                                                    */
#define DONE_LABEL_TO_ALL "<a href=\"#all\">Show all %u completed</a>"

#define DONE_LABEL_TO_CAP "<a href=\"#recent\">Show recent only</a>"

/* ---------------------------------------------------------------------------
 * done_recent_cmp() — most recently completed first.
 *
 * completed_at is stamped on ENTERING Done and never cleared, so it is
 * monotonic and is the lane's whole order.  A Done task can still carry NO
 * stamp — a row completed before 2026-08-27, or one a remote source turned
 * Done (every remote reports 0 for anything it does not consider done) —
 * so updated_at breaks the tie and those rows sort last among themselves
 * rather than in whatever order the query happened to return.
 * ------------------------------------------------------------------------- */
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
 * The PLAN: what the board is about to show, decided before a single
 * widget is touched.
 *
 * lib_refresh_kanban builds one of these per lane and then asks whether the
 * board already holds exactly it (kanban_plan_matches).  When it does,
 * nothing is destroyed and only the labels whose text actually moved are
 * rewritten — which is the difference between a keystroke costing 704 ms
 * and costing nothing at 438 cards (3734 ms against 4.2 ms at 1971).
 * An editor autosave fires notify_tasks every 600 ms while someone types,
 * so that path is walked constantly and almost never has structural work
 * to do.
 *
 * Deciding first is also what makes the test trustworthy: the plan is
 * built from the same query, filter, order and cap the rebuild would use,
 * so "the board already shows this" cannot be answered from a stale idea
 * of what the board should be.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;                      /* the task this card stands for      */
    gchar  *markup;                  /* its finished cell markup, owned    */
} CardPlan;

/* card_plan_add() — append `t`'s card to `lane`'s plan, generating the
 * markup once.  The row-markup lookups live here and nowhere else, so the
 * fast path and the rebuild cannot disagree about what a card says.       */
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

/* kanban_plan_free() — drop a plan and the markup it owns.                */
static void
kanban_plan_free(GArray **plan)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        for (guint i = 0; i < plan[s]->len; i++)
            g_free(g_array_index(plan[s], CardPlan, i).markup);
        g_array_free(plan[s], TRUE);
    }
}

/* ---------------------------------------------------------------------------
 * kanban_plan_matches() — is the board already showing exactly these
 * cards, in these lanes, in this order?
 *
 * Compares IDS only: a card's text is what the fast path is about to
 * update, so a changed title must NOT count as a mismatch.  The lane
 * TOTALS are compared as well, because they are what the headings state
 * and the Done lane's are not the number of cards drawn — 11 completed
 * tasks and 10 both draw ten cards, but only one of them wants the "Show
 * all" link.
 *
 * Returns FALSE for anything it cannot vouch for, which is what makes the
 * whole thing safe: every structural change falls through to the rebuild.
 * ------------------------------------------------------------------------- */
static gboolean
kanban_plan_matches(TaskLibrary *lw, GArray * const *plan,
                    const guint *per_lane)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        if (lw->board.kanban_lanes[s] == NULL ||
            per_lane[s] != lw->board.kanban_counts[s])
            return FALSE;
        /* lane_card_ids skips every child with no task id — the drag
         * marker, the empty-lane placeholder and the Done link — so none
         * of them has to be reasoned about here.                         */
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

/* ---------------------------------------------------------------------------
 * kanban_plan_relabel() — the fast path: rewrite the markup of the cards
 * whose text actually moved and leave every widget where it is.
 *
 * Only called after kanban_plan_matches has vouched for the lanes, so the
 * plan and the cards line up index for index.  The comparison before the
 * write is not tidiness: gtk_label_set_markup re-runs Pango and queues a
 * resize, and an autosave typically moves ONE card of several hundred.
 * ------------------------------------------------------------------------- */
static void
kanban_plan_relabel(TaskLibrary *lw, GArray * const *plan)
{
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        GList *kids = gtk_container_get_children(
            GTK_CONTAINER(lw->board.kanban_lanes[s]));
        guint i = 0;
        for (GList *k = kids; k != NULL; k = k->next) {
            GtkWidget *card = card_of(GTK_WIDGET(k->data));
            if (card_task_id(card) == 0)
                continue;            /* marker / placeholder / the link    */
            const gchar *want =
                g_array_index(plan[s], CardPlan, i++).markup;
            GtkWidget *lab = g_object_get_data(G_OBJECT(card),
                                               "task-card-label");
            if (lab != NULL &&
                g_strcmp0(gtk_label_get_label(GTK_LABEL(lab)), want) != 0)
                gtk_label_set_markup(GTK_LABEL(lab), want);
        }
        g_list_free(kids);
    }
}

/* done_expand_idle() — rebuild the pane after the Done lane's link was
 * clicked.  Deferred for the same reason card_refresh_idle is: the refresh
 * destroys every child of the lane, the link included, and that is the
 * widget whose handler we are inside.                                     */
static gboolean
done_expand_idle(gpointer data)
{
    TaskLibrary *lw = lib_of(data);
    if (lw != NULL)
        lib_refresh_tasks(lw);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * on_done_link_activate() — flip the lane between the most recent DONE_CAP
 * and all of them.
 *
 * Flips the FLAG only; lib_refresh_kanban writes the label, the same rule the
 * View menu follows — the single applier owns the label, never the
 * handler.  Returns TRUE so GtkLabel does not hand the href to
 * gtk_show_uri and try to open "#all" in a browser.
 * ------------------------------------------------------------------------- */
static gboolean
on_done_link_activate(GtkLabel *lbl, gchar *uri, gpointer data)
{
    (void)lbl; (void)uri;
    TaskLibrary *lw = data;
    lw->board.done_show_all = !lw->board.done_show_all;
    g_idle_add(done_expand_idle, lw->app);
    return TRUE;                     /* handled; do not follow the href    */
}

/* ---------------------------------------------------------------------------
 * done_link_pack() — append the Done lane's expand/collapse link.
 *
 * A bare GtkLabel carrying a Pango <a> link — no button.  GtkLabel handles
 * a link itself: the theme's link color, the pointer cursor, keyboard
 * activation, and an "activate-link" signal.  It also carries NO task id,
 * so card_slot_at, card_mark_place and kanban_plan_relabel skip it exactly
 * as they skip the empty-lane placeholder — the drag code and the fast
 * path need to know nothing about it.
 *
 * Visited-link tracking is turned OFF: this is a toggle, not a destination,
 * and GTK would otherwise recolor it permanently the first time it is used,
 * which reads as the control having been spent.
 *
 * It sits at the BOTTOM of the lane, after the last card, which is the
 * "load more" idiom the collapsed state wants.  Known cost, accepted: in
 * the EXPANDED state the collapse link is below every completed task, so
 * getting back to the capped lane means scrolling to the end of it.
 *   total — how many completed tasks the lane stands for, capped or not.
 * ------------------------------------------------------------------------- */
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
    gtk_label_set_track_visited_links(GTK_LABEL(lbl), FALSE);
    gtk_widget_set_margin_top(lbl, 4);      /* off the last card's border  */
    gtk_widget_set_margin_bottom(lbl, 2);
    g_signal_connect(lbl, "activate-link",
                     G_CALLBACK(on_done_link_activate), lw);
    gtk_box_pack_start(GTK_BOX(lw->board.kanban_lanes[TASK_STATUS_DONE]), lbl,
                       FALSE, FALSE, 0);
}

/* ---------------------------------------------------------------------------
 * lib_refresh_kanban() — rebuild the board from `tasks` (already collected for
 * the current view by lib_refresh_tasks, so every view that has a task list
 * can be shown as a board).  Returns the number of cards placed.
 *
 * The lanes are emptied and refilled per refresh, like the forecast's
 * stores: cards are widgets, so "clear" means destroying the children.
 * ------------------------------------------------------------------------- */
guint
lib_refresh_kanban(TaskLibrary *lw, GPtrArray *tasks, const TaskRowCtx *ctx)
{
    /* The saved slot order, applied before the tasks are handed out to
     * lanes — one list for the whole view, which the status filter below
     * projects onto each lane (see kanban_order_key).                     */
    kanban_order_apply(lw, tasks);

    /* ---- Decide, before touching a widget (see the PLAN banner) ------- */
    GArray *plan[TASK_STATUS_N_VALUES];
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        plan[s] = g_array_new(FALSE, FALSE, sizeof(CardPlan));
    guint per_lane[TASK_STATUS_N_VALUES] = { 0 };
    guint shown = 0;

    /* The Done lane is planned in a SECOND pass: it takes no hand-made
     * order, so its cards are sorted by completion and capped rather than
     * taken in the order the query handed them over.                      */
    GPtrArray *done = g_ptr_array_new();

    for (guint i = 0; i < tasks->len; i++) {
        Task *t = g_ptr_array_index(tasks, i);
        gboolean done_task = t->status == TASK_STATUS_DONE;
        /* The completed-visibility toggle applies here exactly as it does
         * to every other view: with completed hidden the Done lane simply
         * empties.  It stays on screen as a drop target, so ticking a task
         * off by dragging still works — and the card vanishing afterwards
         * is the same behavior as the list's fade-out.                     */
        if (!ctx->show_done && done_task)
            continue;
        gint lane = (gint)t->status;
        if (lane < 0 || lane >= TASK_STATUS_N_VALUES)
            lane = TASK_STATUS_NEW;    /* a status off disk, clamped        */

        per_lane[lane]++;
        shown++;
        if (lane == TASK_STATUS_DONE)
            g_ptr_array_add(done, t);  /* second pass, below               */
        else
            card_plan_add(plan[lane], t, ctx);
    }

    /* The Done pass.  `shown` and per_lane already counted every one of
     * these, because the status bar and the lane heading answer "how many
     * are there", not "how many did we draw" — a capped lane that also
     * shrank its own count would hide the fact that it is capped.         */
    g_ptr_array_sort(done, done_recent_cmp);
    guint done_total = done->len;
    guint done_cap   = lw->board.done_show_all ? done_total
                                         : MIN(done_total, (guint)DONE_CAP);
    for (guint i = 0; i < done_cap; i++)
        card_plan_add(plan[TASK_STATUS_DONE],
                      g_ptr_array_index(done, i), ctx);
    g_ptr_array_free(done, TRUE);      /* borrowed elements; `tasks` owns  */

    /* ---- The fast path: same cards, so only the text can have moved --- */
    if (kanban_plan_matches(lw, plan, per_lane)) {
        kanban_plan_relabel(lw, plan);
        kanban_plan_free(plan);
        return shown;                  /* nothing destroyed, nothing built */
    }

    /* ---- The rebuild ------------------------------------------------- */
    lib_scroll_keep_queue_win(lw->board.kanban_box);

    /* A rebuild destroys the marker along with everything else; drop the
     * dangling pointer so card_mark_place does not reorder freed memory
     * if a refresh lands mid-drag (an editor autosave can do that).  The
     * fast path above returns BEFORE this, which is what lets a drag
     * survive the autosaves running underneath it.                        */
    lw->board.card_mark      = NULL;
    lw->board.card_mark_lane = -1;
    lw->board.card_mark_slot = -1;

    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        lane_clear(lw->board.kanban_lanes[s]);

    /* Selections for tasks that have since vanished must not survive the
     * rebuild — Delete Task would act on a tombstone.  Collect the ones
     * that DID come back and keep only those.                             */
    GHashTable *alive = g_hash_table_new(NULL, NULL);
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++) {
        for (guint i = 0; i < plan[s]->len; i++) {
            const CardPlan *cp = &g_array_index(plan[s], CardPlan, i);
            gboolean selected = card_sel_has(lw, cp->id);
            if (selected)
                g_hash_table_add(alive, GSIZE_TO_POINTER((gsize)cp->id));
            gtk_box_pack_start(GTK_BOX(lw->board.kanban_lanes[s]),
                               kanban_card_new(lw, cp->id, cp->markup,
                                               selected),
                               FALSE, FALSE, 0);
        }
    }
    if (done_total > (guint)DONE_CAP)
        done_link_pack(lw, done_total);
    kanban_plan_free(plan);

    /* Replace the selection with the survivors.                          */
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

        /* An empty lane still needs to say so — and still needs to be a
         * drop target, which it is: the DEST is the lane box itself, not
         * its cards.                                                       */
        if (per_lane[s] == 0) {
            GtkWidget *empty = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(empty),
                "<i><span alpha=\"55%\">Drop a task here</span></i>");
            gtk_widget_set_margin_top(empty, 10);
            gtk_widget_set_margin_bottom(empty, 10);
            gtk_box_pack_start(GTK_BOX(lw->board.kanban_lanes[s]), empty,
                               FALSE, FALSE, 0);
        }
        gtk_widget_show_all(lw->board.kanban_lanes[s]);
    }
    /* What the lanes now stand for, for the next refresh to compare.      */
    memcpy(lw->board.kanban_counts, per_lane, sizeof(per_lane));
    return shown;
}

/* ---------------------------------------------------------------------------
 * kanban_lane_new() — one lane: a heading label over a framed, padded
 * body that holds the cards and accepts drops.  Mirrors
 * forecast_day_section's shape (label + framed body, natural height, no
 * scroller of its own).  Fills lw->board.kanban_labels / kanban_lanes [status].
 *
 * The drop target is an EVENT BOX wrapping the card box, not the card box
 * itself: a GtkBox is a no-window widget, and a drag destination needs a
 * real GdkWindow to receive the platform's drag events reliably.  The
 * event box is also what paints the lane's tint, for the same reason —
 * a windowless widget has no surface of its own to fill.
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
    gtk_box_pack_start(GTK_BOX(col), lw->board.kanban_labels[status],
                       FALSE, FALSE, 2);

    GtkWidget *drop = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(drop), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(drop),
                                "task-lane");
    /* Remembered for the drop hit-test: card_lane_at_root measures the
     * pointer's ROOT position against each of these boxes.  No GTK drag
     * destination — the board owns its own drag (see the banner).         */
    lw->board.kanban_drops[status] = drop;

    /* The cards themselves.  Kept separate from the event box so
     * lane_clear can empty it without disturbing the drop target.          */
    GtkWidget *lane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    pad_widget(lane, LANE_PAD);      /* cards off the lane's frame        */
    gtk_container_add(GTK_CONTAINER(drop), lane);
    lw->board.kanban_lanes[status] = lane;

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(frame), drop);
    /* expand=TRUE so the frame (and the event box inside it) fills the
     * column's height: a short lane must still be a drop target all the
     * way down, not just behind the cards it happens to hold.              */
    gtk_box_pack_start(GTK_BOX(col), frame, TRUE, TRUE, 0);
    return col;
}

/* ---------------------------------------------------------------------------
 * task_library_apply_kanban_shadow() — the single writer of the cached
 * kanban_shadow flag, and the live applier behind the Settings check
 * (see header).  Mirrors task_library_apply_native_menubar: the caller
 * has already written the config key, this makes it true on screen.
 * ------------------------------------------------------------------------- */
void
task_library_apply_kanban_shadow(TaskApp *app, gboolean on)
{
    TaskLibrary *lw = lib_of(app);
    if (lw == NULL)                  /* the window may be gone             */
        return;
    lw->board.card_shadow = on;
    card_shadow_restyle(lw);
}

/* ---------------------------------------------------------------------------
 * task_kanban_build() — build the board into lw->board.kanban_box (see library_priv.h).
 * ------------------------------------------------------------------------- */
void
task_kanban_build(TaskLibrary *lw)
{
    /* The Kanban board: three equal lanes side by side, 6 px apart, in
     * one outer scroller — the forecast's construction with the sections
     * turned through 90°.  Homogeneous so a lane holding one card is as
     * wide as a lane holding thirty; NEVER horizontally scrollable so the
     * board always fits the pane and only ever grows downwards.            */
    kanban_css_install();
    GtkWidget *board = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(board), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(board), 6);
    for (gint s = 0; s < TASK_STATUS_N_VALUES; s++)
        gtk_box_pack_start(GTK_BOX(board),
                           kanban_lane_new(lw, (TaskStatus)s),
                           TRUE, TRUE, 0);
    lw->board.kanban_box = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lw->board.kanban_box),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(lw->board.kanban_box), board);
}
