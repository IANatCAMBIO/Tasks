#import <Cocoa/Cocoa.h>

#import "shared_tool_panel.h"
#import "shared_ui.h"

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

/* ── Images ─────────────────────────────────────────────────────────── */

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



