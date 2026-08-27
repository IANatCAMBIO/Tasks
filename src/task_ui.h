/* ===========================================================================
 * task_ui.h — the window's extension points, as registries.
 *
 * Four places a feature can put itself: the toolbar, the menu bar, a
 * task's right-click menu, and the editor window.  Each used to be a
 * literal in library_window.c or editor_window.c — the Sync button was
 * built inline, "Sync Now" was a menu_item() call among ten others,
 * "Open in Google Tasks" was an if inside the context-menu builder, and
 * the "From Google" box was a field on the editor struct.  A feature
 * that ships separately can edit none of those.
 *
 * As with the view registry, the app registers its OWN through this API.
 * If the built-ins cannot be expressed by it, it is not good enough for
 * a plugin either.
 *
 * ORDERING.  Every def carries a `sort`; lower comes first, ties keep
 * registration order.  The app's own items use values spaced far enough
 * apart that a contributor can land between them.
 *
 * LIFETIME.  Definitions are borrowed and must outlive the app — a
 * file-static struct.  Registration happens once at startup, before any
 * window exists; the registries are unlocked, like the others.
 *
 * PERFORMANCE.  A `visible` or `enabled` predicate runs when the widget
 * is BUILT or refreshed, never per draw.  Keep them to a config read or
 * a field test; anything that queries the database belongs behind a
 * cached answer.
 * =========================================================================== */

#ifndef TASK_UI_H
#define TASK_UI_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * Toolbar items.
 *
 * `icon` names a file in icons/ without its extension; `fallback_markup`
 * is the Pango glyph shown when no such file loads, so a missing icon
 * degrades to a character rather than an empty button.
 *
 * `visible` is consulted on every full refresh, which is how a button
 * tied to a setting appears and disappears without its owner watching
 * for the change.  NULL means always visible.
 * ------------------------------------------------------------------------- */
typedef struct {
    const gchar *id;                 /* stable; used to find it again      */
    const gchar *icon;
    const gchar *fallback_markup;
    const gchar *label;
    const gchar *tooltip;
    gint         sort;
    void       (*clicked)(TaskApp *app, gpointer user_data);
    gboolean   (*visible)(TaskApp *app, gpointer user_data);
    gpointer     user_data;
} TaskUiToolDef;

void task_ui_add_tool(const TaskUiToolDef *def);

/* task_ui_tool_set_sensitive() — grey a contributed toolbar button out,
 * for an action already in flight.  Safe before the window exists and
 * after it has gone.                                                      */
void task_ui_tool_set_sensitive(const gchar *id, gboolean sensitive);

/* ---------------------------------------------------------------------------
 * Menu-bar items.  Contributed items land in their own section of the
 * chosen menu, between the app's own groups, separated by rules.
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_UI_MENU_FILE = 0,
    TASK_UI_MENU_VIEW,
} TaskUiMenu;

typedef struct {
    const gchar *id;
    TaskUiMenu   menu;
    const gchar *label;
    gint         sort;
    void       (*activate)(TaskApp *app, gpointer user_data);
    gpointer     user_data;
} TaskUiMenuDef;

void task_ui_add_menu_item(const TaskUiMenuDef *def);

/* ---------------------------------------------------------------------------
 * Task context-menu items — the right-click menu over the task pane.
 *
 * `ids` holds the selected task ids and is BORROWED for the call: the
 * menu is multi-select, so an item that only makes sense for one row
 * says so from `enabled` rather than assuming.
 *
 * `enabled` runs once as the menu is built, not per draw.  Returning
 * FALSE greys the item rather than hiding it, so the menu does not
 * change shape under the pointer between right-clicks.
 * ------------------------------------------------------------------------- */
typedef struct {
    const gchar *id;
    const gchar *label;
    gint         sort;
    gboolean   (*enabled)(TaskApp *app, GArray *ids, gpointer user_data);
    void       (*activate)(TaskApp *app, GArray *ids, gpointer user_data);
    gpointer     user_data;
} TaskUiTaskMenuDef;

void task_ui_add_task_menu_item(const TaskUiTaskMenuDef *def);

/* ---------------------------------------------------------------------------
 * Editor sections — extra content in a task's editor window.
 *
 * `build` returns a widget to pack, or NULL when this task has nothing
 * to show, which is the common case: a section tied to a remote copy
 * appears only for tasks that have one.  Returning NULL is not an
 * error and costs the editor nothing.
 *
 * Called while the editor is being populated, once per open and again
 * on reload.  The widget belongs to the editor afterwards.
 * ------------------------------------------------------------------------- */
typedef struct {
    const gchar *id;
    gint         sort;
    GtkWidget *(*build)(TaskApp *app, const Task *task, gpointer user_data);
    gpointer     user_data;
} TaskUiEditorDef;

void task_ui_add_editor_section(const TaskUiEditorDef *def);

/* ---------------------------------------------------------------------------
 * Enumeration, for the windows that build from these.  Entries come back
 * in display order.
 * ------------------------------------------------------------------------- */
guint                     task_ui_tool_count(void);
const TaskUiToolDef      *task_ui_tool_nth(guint i);
guint                     task_ui_menu_count(void);
const TaskUiMenuDef      *task_ui_menu_nth(guint i);
guint                     task_ui_task_menu_count(void);
const TaskUiTaskMenuDef  *task_ui_task_menu_nth(guint i);
guint                     task_ui_editor_count(void);
const TaskUiEditorDef    *task_ui_editor_nth(guint i);

/* task_ui_tool_bind() / _forget() — the library window records the widget
 * it built for a contributed item, so task_ui_tool_set_sensitive() and
 * the visibility pass can find it.  Window-private plumbing.              */
void task_ui_tool_bind(const gchar *id, GtkWidget *widget);
void task_ui_tool_forget_all(void);

/* task_ui_tools_apply_visibility() — re-run every contributed toolbar
 * item's `visible` predicate.  Called from the full refresh.              */
void task_ui_tools_apply_visibility(TaskApp *app);

/* task_ui_any_tool_visible() — is ANY contributed toolbar item on screen
 * right now?  The window uses this to hide the divider that introduces
 * them: a rule with nothing after it reads as a mistake, and with every
 * contributed button hidden that is exactly what it would be.            */
gboolean task_ui_any_tool_visible(TaskApp *app);

/* ---------------------------------------------------------------------------
 * task_ui_remove_owner() — remove everything plugin `owner`
 * registered here.  Called when a plugin is switched off while the app is
 * running; the app's OWN registrations are unowned and never match.
 * ------------------------------------------------------------------------- */
void task_ui_remove_owner(const gchar *owner);

#endif /* TASK_UI_H */
