#import <Cocoa/Cocoa.h>

/*
 * ui_util — small AppKit helpers shared by every window, so each exists
 * exactly once: the bundled icons, correctly framed image views, and the
 * four kinds of dialog (notice, confirm, path picker, custom sheet).
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

/* ── Images ─────────────────────────────────────────────────────────── */

/*
 * tasks_icon — one of the app's bundled PNG icons (Contents/Resources),
 * scaled down ONCE to the size it will be drawn at.
 *
 * The bundled icons are 512 x 512 pixels.  Handing such an image to a view
 * makes AppKit decode and cache the full bitmap (1 MB each, and a layer-
 * backed view keeps a second copy) to draw a 24 pt button.  The image
 * returned here holds only a bitmap of `size` points at 2x, a few
 * kilobytes, so it is safe to give to any view, button, toolbar item or
 * menu item.  Cached per name and size.
 *
 * Inputs:
 *   name — icon base name without extension: "add", "remove",
 *          "left-and-right", "hidden", "visible", "manual", "automatic",
 *          "menu", "board", "document"
 *   size — edge length in points the icon will be drawn at
 *
 * Output:
 *   shared NSImage of size x size points (do not modify it), or nil when
 *   the bundle has no such icon.
 */
NSImage *tasks_icon(NSString *name, CGFloat size);

/*
 * tasks_app_icon — the app's own icon (the bundled "document" artwork), for
 * the Dock tile, alerts and the About panel.  macOS 26 puts a bundle icon
 * that is not its rounded-square shape on a grey backdrop; handing those
 * places the image explicitly shows just the icon.
 *
 * Inputs:
 *   size — edge length in points
 *
 * Output:
 *   shared NSImage, or nil when the bundle has no such icon.
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
 *   image — the image to show (a tasks_icon, a file icon from NSWorkspace,
 *           ...); may be nil
 *   size  — edge length in points
 *
 * Output:
 *   new NSImageView, size x size points, scaling proportionally down.
 */
NSImageView *tasks_image_view(NSImage *image, CGFloat size);

/* ── Dialogs ────────────────────────────────────────────────────────── */

/*
 * tasks_ui_notice — tell the user something, with an OK button.
 *
 * Inputs:
 *   parent  — window to attach the sheet to; nil shows an app-modal alert
 *   title   — the headline: a plain statement ("Could not open the database")
 *   message — the detail; may be nil
 *   done    — called when it is dismissed; may be nil
 */
void tasks_ui_notice(NSWindow *parent, NSString *title, NSString *message,
                     void (^done)(void));

/*
 * tasks_ui_confirm — ask before doing something.  The first button performs
 * the action and is named for it ("Delete", "Clear"); the second is Cancel,
 * which Escape also chooses.
 *
 * Inputs:
 *   parent      — window to attach the sheet to; nil for app-modal
 *   title       — the question ("Delete 3 tasks?")
 *   message     — the consequence; may be nil
 *   action      — the action button's title
 *   destructive — YES for an action that cannot be undone: the alert takes
 *                 the warning style and the button the destructive look
 *   done        — called with YES when the action button was chosen, NO for
 *                 Cancel or Escape
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
 * tasks_ui_sheet — show custom content as a sheet with a Cancel / <accept>
 * button pair.  The scaffold for the small dialogs: New List, New Group,
 * the date picker.  Return chooses the accept button, Escape chooses
 * Cancel.  The content's first key view takes focus.
 *
 * The content view is sized by its fittingSize, so give it constraints (or
 * build it from stack and grid views) that define its width and height.
 * `done` runs while the content view is still alive, so it reads the
 * controls it needs from there — keep references to them in the block.
 *
 * Inputs:
 *   parent  — window to attach the sheet to (required)
 *   content — the view above the buttons
 *   accept  — the accept button's title ("Create", "Rename", "OK")
 *   done    — called with YES for the accept button, NO for Cancel/Escape
 */
void tasks_ui_sheet(NSWindow *parent, NSView *content, NSString *accept,
                    void (^done)(BOOL accepted));
