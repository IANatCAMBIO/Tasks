#import <Cocoa/Cocoa.h>

#include "db.h"

/*
 * ui_util — small AppKit helpers shared by every window, so each exists
 * exactly once: the icons, correctly framed image views, the table view
 * every list is made of, and the four kinds of dialog (notice, confirm,
 * path picker, custom sheet).
 *
 * Every dialog takes a completion block, called once on the main thread.
 * Given a parent window the dialog is a sheet and the call returns at once
 * (ASYNCHRONOUS: the rest of the caller's work belongs in the block); with
 * a nil parent it is app-modal and the block has run by the time the call
 * returns.  The block is released when the dialog ends, so it may capture
 * its owner strongly without creating a cycle — but it must cope with the
 * owner's window having closed while the dialog was up (check whatever
 * state it is about to act on).
 *
 * All functions are main thread only.
 */

/* ── Text ───────────────────────────────────────────────────────────── */

/*
 * tasks_string — an NSString from text the C core returned.  THE way to
 * bring core text into AppKit: +stringWithUTF8String: (and the @() literal)
 * return nil for bytes that are not valid UTF-8, which a hand-edited
 * database can hold, and a nil string handed to a control raises.
 *
 * Inputs:
 *   text — NUL-terminated bytes from the core; NULL is allowed
 *
 * Output:
 *   the string; text that is not UTF-8 is read as Latin-1 so that it still
 *   shows; @"" for NULL.  Never nil.
 */
NSString *tasks_string(const char *text);

/*
 * tasks_title_string — the text a label, block or reminder shows for a
 * task: its title, or `untitled` when that is empty.  The AppKit layer's
 * one route to it; the rows of the list and the Kanban get the same text
 * from task_rows.
 *
 * Inputs:
 *   task     — the task, not NULL
 *   untitled — what to show when the title is empty
 *
 * Output:
 *   the string; never nil.
 */
NSString *tasks_title_string(const Task *task, NSString *untitled);

/*
 * tasks_tag_title — the text a menu item or button shows for a tag: its
 * label (task_tag_label: emoji, two spaces, name; the name alone without
 * an emoji).
 *
 * Inputs:
 *   tag — the tag
 *
 * Output:
 *   the title.
 */
NSString *tasks_tag_title(const TaskTag *tag);

/*
 * tasks_label_name — the bare name in a label of task_tag_label's form
 * (a tag's, a view's): what follows the emoji and its two spaces, or the
 * whole label when it has none.  For type-select and VoiceOver, which
 * must not start at the emoji.
 *
 * Inputs:
 *   label — the label
 *
 * Output:
 *   the name.
 */
NSString *tasks_label_name(NSString *label);

/* ── Images ─────────────────────────────────────────────────────────── */

/*
 * tasks_icon — a command's icon: an SF Symbol, as a TEMPLATE image.
 *
 * A template image is a shape, not a picture: the control showing it draws
 * the shape in its own text colour, so the icon follows light and dark,
 * the disabled look and a pressed button with no code here.  (The GTK
 * build's PNG artwork was dark ink on transparent and all but vanished on
 * a dark toolbar.)  Use it for toolbar items, buttons and menu items.
 * Cached per name and size.
 *
 * Inputs:
 *   name — SF Symbol name: "plus", "trash", "sidebar.left", "eye", ...
 *   size — the symbol's point size, as a font's: the symbol is drawn about
 *          as tall as text set at that size
 *
 * Output:
 *   shared NSImage (do not modify it), or nil when the system has no
 *   symbol of that name.
 */
NSImage *tasks_icon(NSString *name, CGFloat size);

/*
 * tasks_symbol — an SF Symbol with no point size of its own, for a
 * toolbar item, which sizes its image itself.  Cached per name: the
 * toolbar re-validates its items after every event.
 *
 * Inputs:
 *   name — SF Symbol name
 *
 * Output:
 *   shared NSImage (do not modify it), or nil when the system has no
 *   symbol of that name.
 */
NSImage *tasks_symbol(NSString *name);

/*
 * tasks_app_icon — the app's own icon (the bundled "document" artwork, the
 * one PNG the app ships), for the Dock tile, alerts and the About panel.
 * macOS 26 puts a bundle icon that is not its rounded-square shape on a
 * grey backdrop; handing those places the image explicitly shows just the
 * icon.
 *
 * The artwork is 512 x 512 pixels.  Handing such an image to a view makes
 * AppKit decode and cache the full bitmap (1 MB, and a layer-backed view
 * keeps a second copy) to draw a 64 pt picture.  The image returned here
 * holds only a bitmap of `size` points at 2x, scaled ONCE, so it is safe
 * to give to any view.  Cached per size.
 *
 * Inputs:
 *   size — edge length in points
 *
 * Output:
 *   shared NSImage of size x size points (do not modify it), or nil when
 *   the bundle has no such icon.
 */
NSImage *tasks_app_icon(CGFloat size);

/*
 * tasks_image_view — an image view created AT ITS FINAL SIZE and only then
 * given its image.  THE way to make an NSImageView in this app.
 *
 * An image view made with a zero frame (-init, +imageViewWithImage:), sized
 * only by Auto Layout and given its image before any layout pass, has no
 * target size when the image arrives: AppKit renders the image's LARGEST
 * representation (1024 x 1024 px = 4 MB for a system file icon), caches
 * it, and the view's layer holds a second copy.  A table of such cells
 * costs tens of megabytes.  Creating the view with its frame first avoids
 * it.  The view also carries width and height constraints, so it keeps the
 * size inside a stack or grid view.
 *
 * Inputs:
 *   image — the image to show (tasks_app_icon, a file icon from
 *           NSWorkspace, ...); may be nil
 *   size  — edge length in points
 *
 * Output:
 *   new NSImageView, size x size points, scaling proportionally down.
 */
NSImageView *tasks_image_view(NSImage *image, CGFloat size);

/* ── Tables ─────────────────────────────────────────────────────────── */

/*
 * TasksTable — THE table view of this app: every list is one (or a
 * subclass of one).  It selects a clicked row on mouse-DOWN, as Finder
 * does, where a stock NSTableView waits for mouse-up while it tells a
 * click from a drag.  Everything else is NSTableView's own: drags (of the
 * whole selection, when the press lands on a selected row), double-click
 * actions, type-select and the context menu.
 *
 * Left to NSTableView as before: a press on a row that is already
 * selected, a ⌘-, ⇧- or ^-click, a double-click, and a press on a button
 * inside a cell (a checkbox acts on its own row and leaves the selection
 * alone).  The delegate's tableView:selectionIndexesForProposedSelection:
 * or tableView:shouldSelectRow: is asked first, so a row it refuses is not
 * selected early.
 */
@interface TasksTable : NSTableView
@end

/* ── Dialogs ────────────────────────────────────────────────────────── */

/*
 * tasks_ui_notice — tell the user something, with an OK button, in a tool
 * panel (tool_panel.h) — never a system alert.  Returns at once.
 *
 * Inputs:
 *   parent  — window to centre the panel over; nil centres it on the screen
 *   title   — the headline: a plain statement ("Could not open the database")
 *   message — the detail; may be nil
 *   done    — called when it is closed, by OK, Escape or the close box;
 *             may be nil
 */
void tasks_ui_notice(NSWindow *parent, NSString *title, NSString *message,
                     void (^done)(void));

/*
 * tasks_ui_confirm — ask before doing something, in a tool panel
 * (tool_panel.h) — never a system alert.  Returns at once.  Cancel is the
 * default button (Return), as in Notes, so a key press never performs the
 * action; the action button is named for it ("Delete", "Clear") and sits
 * to Cancel's left.  Escape and the close box count as Cancel.
 *
 * Inputs:
 *   parent      — window to centre the panel over; nil centres it on the
 *                 screen
 *   title       — the question ("Delete 3 tasks?")
 *   message     — the consequence; may be nil
 *   action      — the action button's title
 *   destructive — YES for an action that cannot be undone: a warning
 *                 symbol, and the action button takes the destructive look
 *   done        — called with YES when the action button was chosen, NO for
 *                 Cancel, Escape or the close box
 */
void tasks_ui_confirm(NSWindow *parent, NSString *title, NSString *message,
                      NSString *action, BOOL destructive, void (^done)(BOOL confirmed));

/* What tasks_ui_pick_path asks for. */
typedef NS_ENUM(NSInteger, TasksPickKind) {
    TasksPickFile = 0,              /* one existing file                  */
    TasksPickFolder                 /* one existing folder (may create)   */
};

/*
 * tasks_ui_pick_path — let the user choose a file or a folder.
 *
 * Inputs:
 *   parent     — window to attach the panel to; nil for app-modal
 *   kind       — a file or a folder
 *   message    — a line shown in the panel saying what is wanted; may be nil
 *   action     — the accept button's title ("Open", "Attach", "Choose")
 *   extensions — file name extensions to allow, without the dot (@[@"db"]);
 *                nil or empty allows everything.  Ignored for folders.
 *   start_dir  — folder the panel opens in; nil for the system's choice
 *   done       — called with the chosen path, or nil when cancelled
 */
void tasks_ui_pick_path(NSWindow *parent, TasksPickKind kind, NSString *message,
                        NSString *action, NSArray<NSString *> *extensions,
                        NSString *start_dir, void (^done)(NSString *path));

/*
 * tasks_ui_place_lower_right — move a window to the lower-right corner of a
 * screen's work area (what the menu bar and the Dock leave free),
 * tasks_look_screen_margin in from its right and bottom edges.  The window
 * keeps its size; a window too big for the corner keeps its title bar on
 * screen.
 *
 * Inputs:
 *   window — the window to move
 *   screen — the screen; nil leaves the window where it is
 */
void tasks_ui_place_lower_right(NSWindow *window, NSScreen *screen);

/*
 * tasks_emoji_field — Edit Tag's emoji slot: a small text field that holds
 * at most one character and opens the system's emoji picker when clicked
 * (the same slot as Notes').  The picker is the system's Character Viewer,
 * which types into whatever text is being edited, so the slot has to be a
 * real text field.
 *
 * Inputs:
 *   emoji — the current emoji; empty for none
 *
 * Output:
 *   the field, tasks_look_tool_emoji_w wide, in the app's text size.
 */
NSTextField *tasks_emoji_field(NSString *emoji);
