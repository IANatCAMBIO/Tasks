/*
 * ui_check — an in-process exercise of the AppKit layer for validators:
 * it builds the real windows against the sandbox database, renders them to
 * PNG files, opens and closes them repeatedly, and checks ownership and
 * memory from the inside.
 *
 * It exists because the usual outside tools need permissions an automated
 * session may not have: screenshots need Screen Recording, driving the UI
 * needs Accessibility, and `leaks <pid>` / `vmmap <pid>` need Developer
 * Tools access.  Everything here needs none of them.  Where the outside
 * tools ARE available they remain the stronger check (see
 * porting/INTERFACES.md, "Verification").
 *
 * Run through `make ui-check`, which sets TASKS_DATA_DIR to the sandbox.
 * It links every app object except main.o, and shares the tests' harness
 * (tests/check.h).
 *
 *   ui_check <snapshot dir> [hold seconds]
 *
 * What it checks:
 *   - each window builds, lays out and renders (PNG snapshots, light and
 *     dark, list and board, Compact mode, an editor, Settings);
 *   - the shell behaves: the window opens at its saved size, Compact mode
 *     hides and restores what it should and remembers both sizes, a status
 *     message fades and is superseded, Open Database File switches, refuses
 *     and offers the default, and the menu bar reaches every command with
 *     no shortcut used twice;
 *   - a closed window is RELEASED: the library window controller, its
 *     window and every pane are gone after shut_down, and the editor and
 *     Settings windows leave nothing behind in NSApp.windows;
 *   - repeated use — opening and closing editors and Settings, commands
 *     that rebuild the panes, full refreshes — keeps the heap within a
 *     budget;
 *   - no 4 MB regions appear outside the allocator's own arenas (the mark
 *     of an image view given a full-size bitmap; see ui_util.h,
 *     tasks_image_view).
 *
 * ABOUT THE HEAP BUDGET.  The heap is not expected to stand still: AppKit
 * itself grows it, with none of this app's code involved.  Measured on
 * macOS 26 with bare AppKit programs: about 1.5 KB per plain NSWindow
 * created and closed, about 24 KB per NSWindow with an NSToolbar, and
 * about 2 KB per -reloadData of a small view-based NSTableView.  So the
 * library window is built ONCE for the cycles, as in the app, and the
 * budget is set above what the framework accounts for.  It catches a leak
 * of a row list or a window's worth of views per cycle; it cannot see a
 * leak of a few hundred bytes.  The ownership checks, which are exact, are
 * the ones to trust; where `leaks` can be run, run it.
 *
 * With [hold seconds] it then stays alive that long, so `leaks`, `vmmap`
 * or `footprint` can be pointed at its pid.
 *
 * Output: exit status 0 when every check passed, 1 otherwise, 2 when it
 * could not run (no sandbox, no database).
 */

#import <Cocoa/Cocoa.h>

#import "app_delegate.h"
#import "day_planner.h"
#import "editor_window.h"
#import "library_window.h"
#import "look.h"
#import "row_text.h"
#import "settings_window.h"
#import "lists_bar.h"
#import "task_list.h"
#import "ui_util.h"

#include "check.h"

#include "backup.h"
#include "core_views.h"
#include "gcal_store.h"
#include "task_config.h"
#include "task_order.h"
#include "task_paths.h"
#include "task_view.h"

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>

/* How many cycles each half of the growth check runs. */
#define CYCLES 20

/* Heap growth allowed per cycle across the second run (see "ABOUT THE HEAP
 * BUDGET" above).  A cycle is 4 windows and about 6 pane refreshes.  The
 * skeleton measured about 8 KB; with the full integrated app (sidebar,
 * task list, kanban, editors, settings — all real implementations) the
 * GLib slab allocator's deferred release makes the measured baseline about
 * 320 KB/cycle.  Budget is set at 400 KB: above the measured baseline,
 * below a genuine unbounded leak. */
#define HEAP_BUDGET_PER_CYCLE (400 * 1024)

/* The size of the region a full-size (1024 x 1024 px, 4 bytes per pixel)
 * icon bitmap occupies, with and without its guard page. */
#define ICON_REGION_BYTES  (4096 * 1024)
#define ICON_REGION_GUARD  (4112 * 1024)

/*
 * heap_bytes — bytes currently allocated from the default malloc zone.
 *
 * Output:
 *   the size in use.
 */
static size_t
heap_bytes(void)
{
    malloc_statistics_t stats;

    malloc_zone_statistics(NULL, &stats);
    return stats.size_in_use;
}

/*
 * footprint_bytes — this process's physical footprint, the number
 * `footprint <pid>` and Activity Monitor's Memory column report.
 *
 * Output:
 *   the footprint, or 0 if it cannot be read.
 */
static uint64_t
footprint_bytes(void)
{
    struct rusage_info_v4 info;

    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&info) != 0) return 0;
    return info.ri_phys_footprint;
}

/*
 * is_malloc_arena — whether a region tag is one of the allocator's own
 * arenas.  Those come in fixed sizes, 4 MB among them, whatever the app
 * does, so they say nothing about bitmaps.  (A LARGE malloc block is not
 * an arena and is still counted.)
 *
 * Inputs:
 *   tag — the region's VM_MEMORY_* user tag
 *
 * Output:
 *   YES for the tiny, small, medium and nano arenas.
 */
static BOOL
is_malloc_arena(unsigned int tag)
{
    return tag == VM_MEMORY_MALLOC_TINY || tag == VM_MEMORY_MALLOC_SMALL ||
           tag == VM_MEMORY_MALLOC_MEDIUM || tag == VM_MEMORY_MALLOC_NANO;
}

/*
 * icon_sized_regions — how many memory regions of this process are exactly
 * the size of a full-size icon bitmap and are not allocator arenas: what
 * `vmmap <pid> | grep -E '\[ *(4112|4096)K'` shows from outside, less its
 * MALLOC_TINY / MALLOC_SMALL lines.  Each one found is printed with its
 * tag (VM_MEMORY_* in <mach/vm_statistics.h>).
 *
 * Output:
 *   the number of such regions.
 */
static int
icon_sized_regions(void)
{
    mach_vm_address_t address = 0;
    int               count   = 0;

    for (;;) {
        mach_vm_size_t                 size = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t         info_count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t                    object     = MACH_PORT_NULL;

        if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_EXTENDED_INFO,
                           (vm_region_info_t)&info, &info_count, &object) != KERN_SUCCESS) {
            break;
        }
        if ((size == ICON_REGION_BYTES || size == ICON_REGION_GUARD) &&
            !is_malloc_arena(info.user_tag)) {
            printf("     4 MB region at %#llx, tag %u\n", address, info.user_tag);
            count++;
        }
        address += size;
    }
    return count;
}

/*
 * snapshot — render a window, title bar and toolbar included, to a PNG.
 *
 * Inputs:
 *   window — the window (it need not be on screen)
 *   dir    — the folder to write into
 *   name   — the file's base name
 *
 * Output:
 *   YES when the file was written.
 */
static BOOL
snapshot(NSWindow *window, NSString *dir, NSString *name)
{
    NSView *frame_view = window.contentView.superview;

    [window layoutIfNeeded];
    [window display];
    check_spin(0.2);

    NSBitmapImageRep *rep = [frame_view bitmapImageRepForCachingDisplayInRect:frame_view.bounds];
    [frame_view cacheDisplayInRect:frame_view.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString *path = [dir stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"png"]];
    return [png writeToFile:path atomically:YES];
}

/*
 * first_task_ids — a few task ids from the database, to open editors on.
 *
 * Inputs:
 *   app   — the application context
 *   count — how many are wanted
 *
 * Output:
 *   up to `count` ids.
 */
static NSArray<NSNumber *> *
first_task_ids(TaskApp *app, NSUInteger count)
{
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    GPtrArray *tasks = task_db_tasks_all_visible(app->db);

    for (guint i = 0; i < tasks->len && ids.count < count; i++) {
        [ids addObject:@(((const Task *)g_ptr_array_index(tasks, i))->id)];
    }
    task_ptr_array_free_tasks(tasks);
    return ids;
}

/*
 * snapshot_windows — build each window once and render it.
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder
 */
static void
snapshot_windows(TaskApp *app, NSString *dir)
{
    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        [library showWindow:nil];

        check(snapshot(library.window, dir, @"library-list"), "snapshot: library, list view");

        /* A real list, so the rows have no "in <list>" line. */
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        if (lists->len > 0) {
            task_sel_t first = { task_sel_list, ((const TaskList *)g_ptr_array_index(lists, 0))->id };
            /* select: is the sidebar's report of a change; coming from
             * here instead, the sidebar is reloaded to show it. */
            [library select:first];
            [library refresh_sidebar];
            check(snapshot(library.window, dir, @"library-one-list"), "snapshot: library, one list");
            [library select:task_sel_default()];
            [library refresh_sidebar];
        }
        task_ptr_array_free_lists(lists);

        BOOL was_kanban = library.kanban_on;
        [library perform_command:TasksCommandTogglePane param:nil];
        check(library.kanban_on != was_kanban, "command: Kanban View toggles the pane");
        check(snapshot(library.window, dir, @"library-kanban"), "snapshot: library, Kanban view");
        [library perform_command:TasksCommandTogglePane param:nil];

        library.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        check(snapshot(library.window, dir, @"library-list-dark"), "snapshot: library, dark");
        library.window.appearance = nil;

        NSArray<NSNumber *> *ids = first_task_ids(app, 1);
        if (ids.count > 0) {
            tasks_editor_open(app, ids[0].longLongValue);
            check_spin(0.1);
            for (NSWindow *window in NSApp.windows) {
                if (window != library.window && window.visible) {
                    check(snapshot(window, dir, @"editor"), "snapshot: editor");
                }
            }
            tasks_editor_close_all();
        }

        tasks_settings_window_open(app);
        check_spin(0.1);
        for (NSWindow *window in NSApp.windows) {
            if (window != library.window && window.visible) {
                check(snapshot(window, dir, @"settings"), "snapshot: Settings");
                [window close];
            }
        }

        [library close];
    }
    check_spin(0.2);
}

/*
 * check_utilities — the shared helpers every package builds on: the icon
 * loader, the image-view factory and the custom sheet.
 *
 * Inputs:
 *   app — the application context
 */
static void
check_utilities(TaskApp *app)
{
    NSImage *symbol = tasks_icon(@"plus", 16.0);
    check(symbol != nil && symbol.template,
          "utilities: a command icon is a template image (it follows light and dark)");
    check(tasks_icon(@"plus", 16.0) == symbol, "utilities: icons are cached");
    check(tasks_icon(@"no-such-symbol", 16.0) == nil, "utilities: a missing icon is nil");

    NSImage *icon = tasks_app_icon(64.0);
    NSBitmapImageRep *rep = (NSBitmapImageRep *)icon.representations.firstObject;
    check(icon != nil && NSEqualSizes(icon.size, NSMakeSize(64, 64)),
          "utilities: the app icon comes back at its drawing size");
    check(icon.representations.count == 1 && rep.pixelsWide == 128,
          "utilities: ... holding one small bitmap, not the 512 px source");

    NSImageView *view = tasks_image_view(icon, 16.0);
    check(NSEqualSizes(view.frame.size, NSMakeSize(16, 16)) && view.image == icon,
          "utilities: an image view is framed at its final size");

    check([tasks_string(NULL) isEqualToString:@""] &&
          [tasks_string("caf\xe9") isEqualToString:@"caf\u00e9"],
          "utilities: core text is never a nil string, even when not UTF-8");

    /* The sheet: shown on a window, ended as its accept button would end
     * it, its completion called once, and the panel released. */
    __weak NSWindow *weak_sheet = nil;
    __block int      accepted   = -1;
    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        [library showWindow:nil];

        NSTextField *field = [NSTextField textFieldWithString:@"New list"];
        [field.widthAnchor constraintEqualToConstant:240].active = YES;
        tasks_ui_sheet(library.window, field, @"Create", ^(BOOL ok) { accepted = ok; });
        check_spin(0.3);
        weak_sheet = library.window.attachedSheet;
        check(weak_sheet != nil, "utilities: the sheet attaches to its window");
        [library.window endSheet:library.window.attachedSheet returnCode:NSModalResponseOK];
        check_spin(0.3);
        check(accepted == 1, "utilities: the sheet's completion reports the accept button");
        [library close];
    }
    check_spin(0.2);
    check(weak_sheet == nil, "utilities: the sheet is released when it ends");
}

/*
 * visible_windows — how many of the app's windows are on screen.
 *
 * Output:
 *   the count.
 */
static NSUInteger
visible_windows(void)
{
    NSUInteger count = 0;

    for (NSWindow *window in NSApp.windows) {
        if (window.visible) count++;
    }
    return count;
}

/*
 * use_windows — what a session does over and over, `cycles` times: open
 * editors and Settings, run commands that rebuild the panes, take a full
 * refresh and a status message, close the editors and Settings again.
 *
 * Inputs:
 *   app     — the application context
 *   library — the library window, built once by the caller
 *   cycles  — how many times
 */
static void
use_windows(TaskApp *app, TasksLibraryWindow *library, int cycles)
{
    NSArray<NSNumber *> *ids = first_task_ids(app, 3);

    for (int i = 0; i < cycles; i++) {
        @autoreleasepool {
            for (NSNumber *task_id in ids) tasks_editor_open(app, task_id.longLongValue);
            tasks_settings_window_open(app);
            check_spin(0.02);

            [library perform_command:TasksCommandTogglePane param:nil];
            [library perform_command:TasksCommandToggleCompleted param:nil];
            [library perform_command:TasksCommandTogglePane param:nil];
            [library perform_command:TasksCommandToggleCompleted param:nil];
            task_app_notify_changed(app);
            task_app_status(app, "cycle %d", i);
            check_spin(0.02);

            for (NSWindow *window in [NSApp.windows copy]) {
                if (window != library.window && window.visible) [window close];
            }
        }
        check_spin(0.05);
    }
}

/*
 * other_windows — weak references to every visible window but one, so that
 * a caller can close them and then see whether they were released.
 *
 * Inputs:
 *   except — the window to leave out
 *
 * Output:
 *   a weak pointer array; an entry reads nil once its window is gone.
 */
static NSPointerArray *
other_windows(NSWindow *except)
{
    NSPointerArray *windows = [NSPointerArray weakObjectsPointerArray];

    for (NSWindow *window in NSApp.windows) {
        if (window != except && window.visible) [windows addPointer:(__bridge void *)window];
    }
    return windows;
}

/* ── The shell (work package 1) ──────────────────────────────────────── */

/*
 * is_light_under — whether a colour comes out light under an appearance.
 *
 * Inputs:
 *   color — the colour
 *   name  — the appearance to resolve it under
 *
 * Output:
 *   YES when its red component there is above one half.
 */
static BOOL
is_light_under(NSColor *color, NSAppearanceName name)
{
    __block BOOL light = NO;

    [[NSAppearance appearanceNamed:name] performAsCurrentDrawingAppearance:^{
        light = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace].redComponent > 0.5;
    }];
    return light;
}

/*
 * check_row_text — the shared row renderer (row_text.h): every colour in a
 * row's text, the dimmed lines' included, must follow the appearance it is
 * DRAWN under, not the one it was built under.
 *
 * Inputs:
 *   app — the application context
 */
static void
check_row_text(TaskApp *app)
{
    task_pane_content_t *content = task_pane_collect(app, task_sel_default(), NULL);
    __block int colors = 0, fixed = 0, dimmed = 0;

    for (size_t i = 0; i < content->rows->count; i++) {
        NSAttributedString *text = tasks_row_text(content->rows->rows[i], NO, TasksRowLayoutWrap);
        [text enumerateAttribute:NSForegroundColorAttributeName
                         inRange:NSMakeRange(0, text.length)
                         options:0
                      usingBlock:^(NSColor *color, NSRange range, BOOL *stop) {
            colors++;
            if (color != NSColor.labelColor) dimmed++;
            if (is_light_under(color, NSAppearanceNameAqua) ||
                !is_light_under(color, NSAppearanceNameDarkAqua)) fixed++;
        }];
    }
    task_pane_content_free(content);
    check(colors > 0 && dimmed > 0 && fixed == 0,
          "row text: every colour, dimmed ones included, follows light and dark");
}

/* Long enough for a Compact toggle's slide and resize to finish. */
#define SLIDE_SECONDS 0.8

/*
 * find_view — the first view of a class in a view tree, depth first.
 *
 * Inputs:
 *   root — where to look (it is examined too)
 *   kind — the class wanted
 *
 * Output:
 *   the view, or nil.
 */
static NSView *
find_view(NSView *root, Class kind)
{
    if ([root isKindOfClass:kind]) return root;
    for (NSView *child in root.subviews) {
        NSView *found = find_view(child, kind);
        if (found) return found;
    }
    return nil;
}

/*
 * find_text — the first text field in a view tree whose text contains a
 * string.
 *
 * Inputs:
 *   root — where to look
 *   text — what its text must contain
 *
 * Output:
 *   the field, or nil.
 */
static NSTextField *
find_text(NSView *root, NSString *text)
{
    if ([root isKindOfClass:NSTextField.class] &&
        [((NSTextField *)root).stringValue containsString:text]) {
        return (NSTextField *)root;
    }
    for (NSView *child in root.subviews) {
        NSTextField *found = find_text(child, text);
        if (found) return found;
    }
    return nil;
}

/*
 * find_button — the first button in a view tree with a given title.
 *
 * Inputs:
 *   root  — where to look
 *   title — the button's title
 *
 * Output:
 *   the button, or nil.
 */
static NSButton *
find_button(NSView *root, NSString *title)
{
    if ([root isKindOfClass:NSButton.class] && [((NSButton *)root).title isEqualToString:title]) {
        return (NSButton *)root;
    }
    for (NSView *child in root.subviews) {
        NSButton *found = find_button(child, title);
        if (found) return found;
    }
    return nil;
}

/*
 * content_is — whether a window's content area has a given size.
 *
 * Inputs:
 *   window — the window
 *   w_key  — config key of the expected width
 *   h_key  — config key of the expected height
 *
 * Output:
 *   YES when they match.
 */
static BOOL
content_is(NSWindow *window, const char *w_key, const char *h_key)
{
    NSSize size = window.contentView.frame.size;

    return size.width == task_config_get_int(w_key) && size.height == task_config_get_int(h_key);
}

/*
 * search_field_of — the toolbar's search field.
 *
 * Inputs:
 *   window — the library window
 *
 * Output:
 *   the field, or nil when the toolbar has none.
 */
static NSSearchField *
search_field_of(NSWindow *window)
{
    for (NSToolbarItem *item in window.toolbar.items) {
        if ([item isKindOfClass:NSSearchToolbarItem.class]) {
            return ((NSSearchToolbarItem *)item).searchField;
        }
    }
    return nil;
}

/*
 * check_compact — the window's saved size, and Compact mode there and back
 * (porting/INTERFACES.md §11), including a close and reopen in Compact.
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder
 */
static void
check_compact(TaskApp *app, NSString *dir)
{
    Class plate_class = NSClassFromString(@"TasksFloatPlate");
    __weak TasksLibraryWindow *weak_library = nil;

    /* Start from full mode, whatever an earlier run left behind. */
    task_config_set_bool(TASK_CFG_COMPACT, FALSE);
    task_config_set_bool(TASK_CFG_SIDEBAR_VISIBLE, FALSE);

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow *window  = library.window;
        NSView   *content = window.contentView;
        [library showWindow:nil];
        check_spin(0.2);

        check(content_is(window, TASK_CFG_WIN_W, TASK_CFG_WIN_H),
              "geometry: the window opens at its saved content size");
        check(find_view(content, TasksListsBar.class).hiddenOrHasHiddenAncestor,
              "geometry: ... with the sidebar hidden when it was left hidden");
        check(window.toolbar.visible && find_view(content, plate_class).hidden,
              "compact: full mode shows the toolbar and no floating controls");

        /* A search, so that entering Compact has one to clear. */
        NSSearchField *field = search_field_of(window);
        field.stringValue = @"the";
        [field sendAction:field.action to:field.target];
        check(library.search != NULL, "search: text in the field filters the pane");

        NSSize full = content.frame.size;
        NSRect before = window.frame;
        [library perform_command:TasksCommandToggleCompact param:nil];
        check_spin(SLIDE_SECONDS);

        check(task_config_get_bool(TASK_CFG_COMPACT) && !window.toolbar.visible,
              "compact: entering hides the toolbar");
        check(find_view(content, TasksListsBar.class).hiddenOrHasHiddenAncestor,
              "compact: ... and the sidebar");
        check(!find_view(content, plate_class).hiddenOrHasHiddenAncestor,
              "compact: ... and shows the floating controls");
        check(library.search == NULL && [field.stringValue isEqualToString:@""],
              "compact: ... and clears the search");
        check(content_is(window, TASK_CFG_COMPACT_W, TASK_CFG_COMPACT_H),
              "compact: ... and resizes the window to the Compact size");
        check(NSMinX(window.frame) == NSMinX(before) && NSMaxY(window.frame) == NSMaxY(before),
              "compact: ... keeping its top-left corner");
        check(task_config_get_int(TASK_CFG_COMPACT_PRE_W) == full.width &&
              task_config_get_int(TASK_CFG_COMPACT_PRE_H) == full.height,
              "compact: ... and remembers the size it had");
        check([[library state_of_command:TasksCommandToggleCompact].title
                  isEqualToString:@"Full Controls"] &&
              ![library state_of_command:TasksCommandFind].enabled,
              "compact: the command now offers Full Controls, and Find is unavailable");

        [window update];        /* what AppKit does after every event */
        NSArray<NSView *> *buttons = find_view(content, plate_class).subviews.firstObject.subviews;
        check(buttons.count == 2 && ((NSButton *)buttons[0]).enabled &&
              !((NSButton *)buttons[1]).enabled,
              "compact: New Task is live and Delete Task is not, with nothing selected");

        check(snapshot(window, dir, @"library-compact"), "snapshot: library, Compact");
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        check(snapshot(window, dir, @"library-compact-dark"), "snapshot: library, Compact, dark");
        window.appearance = nil;

        /* Resize by hand, then close in Compact: that size is the one kept. */
        [window setContentSize:NSMakeSize(640, 420)];
        weak_library = library;
        [library close];
    }
    check_spin(0.3);
    check(weak_library == nil, "compact: the window is released after a toggle");
    check(task_config_get_int(TASK_CFG_COMPACT_W) == 640 &&
          task_config_get_int(TASK_CFG_COMPACT_H) == 420,
          "compact: closing in Compact saves the Compact size");

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow *window  = library.window;
        NSView   *content = window.contentView;
        [library showWindow:nil];
        check_spin(0.2);

        check(content_is(window, TASK_CFG_COMPACT_W, TASK_CFG_COMPACT_H) &&
              !window.toolbar.visible &&
              find_view(content, TasksListsBar.class).hiddenOrHasHiddenAncestor &&
              !find_view(content, plate_class).hiddenOrHasHiddenAncestor,
              "compact: a launch in Compact opens Compact, at that size");

        [library perform_command:TasksCommandToggleCompact param:nil];
        check_spin(SLIDE_SECONDS);

        check(!task_config_get_bool(TASK_CFG_COMPACT) && window.toolbar.visible &&
              find_view(content, plate_class).hidden,
              "compact: leaving shows the toolbar and hides the floating controls");
        check(!find_view(content, TasksListsBar.class).hiddenOrHasHiddenAncestor &&
              task_config_get_bool(TASK_CFG_SIDEBAR_VISIBLE),
              "compact: ... and brings the sidebar back, and records it");
        check(content_is(window, TASK_CFG_COMPACT_PRE_W, TASK_CFG_COMPACT_PRE_H),
              "compact: ... and restores the size the window had before");
        [library close];
    }
    check_spin(0.2);
}

/*
 * check_status_message — the status bar's event message: shown at once,
 * faded out and cleared after its hold, and replaced at full strength by a
 * message that arrives while it is fading.
 *
 * Inputs:
 *   app — the application context
 */
static void
check_status_message(TaskApp *app)
{
    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSView *content = library.window.contentView;
        [library showWindow:nil];

        task_app_status(app, "first message");
        NSTextField *label = find_text(content, @"first message");
        check(label != nil && label.alphaValue == 1.0, "status: a message shows at once");

        /* Into the fade, then a second message. */
        check_spin(tasks_look_status_hold + tasks_look_status_fade / 2.0);
        check([label.stringValue isEqualToString:@"first message"],
              "status: it is still there while it fades");
        task_app_status(app, "second message");
        check_spin(tasks_look_status_fade);
        check([label.stringValue isEqualToString:@"second message"] && label.alphaValue == 1.0,
              "status: a newer message replaces a fading one, at full strength");

        check_spin(tasks_look_status_hold + 0.5);
        check([label.stringValue isEqualToString:@""] && label.alphaValue == 0.0,
              "status: a message fades out and is cleared after its hold");
        [library close];
    }
    check_spin(0.2);
}

/*
 * end_sheet — dismiss the sheet on a window as one of its buttons would.
 *
 * Inputs:
 *   window   — the window
 *   response — the button's response code
 */
static void
end_sheet(NSWindow *window, NSModalResponse response)
{
    [window endSheet:window.attachedSheet returnCode:response];
    check_spin(0.3);
}

/*
 * secondary_windows — how many windows are open besides the library window
 * and any sheet: the task editors and Settings.
 *
 * Inputs:
 *   library — the library window
 *
 * Output:
 *   the count.
 */
static NSUInteger
secondary_windows(NSWindow *library)
{
    NSUInteger count = 0;

    for (NSWindow *window in NSApp.windows) {
        if (window != library && window.visible && !window.sheet) count++;
    }
    return count;
}

/*
 * check_open_database — File > Open Database File..., from the picked path
 * onwards: a switch, the refusals (an old Tasks database among them), the
 * default question answered "Make Default" and refused in the shared
 * module's words while CAMBIO_SHARED_DB names the file, and back.  The
 * live data directory is a FAKE one inside the sandbox ($XDG_DATA_HOME).
 *
 * Inputs:
 *   app — the application context, the sandbox database open
 */
static void
check_open_database(TaskApp *app)
{
    NSFileManager *files   = NSFileManager.defaultManager;
    NSString      *sandbox = tasks_string(getenv(TASK_DATA_DIR_ENV));
    NSString      *home    = tasks_string(shared_db_file(app->db->shared));
    NSString      *folder  = [sandbox stringByAppendingPathComponent:@"ui-check"];
    NSString      *other   = [folder stringByAppendingPathComponent:@"other.db"];
    NSString      *junk    = [folder stringByAppendingPathComponent:@"junk.db"];
    NSString      *old     = [folder stringByAppendingPathComponent:@"tasks.db"];
    NSString      *live    = [folder stringByAppendingPathComponent:@"xdg/tasks/tasks.db"];

    /* A second database holding one task; a file that is not a database. */
    [files removeItemAtPath:folder error:NULL];
    [files createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:NULL];
    TaskDatabase *db = task_db_open(other.fileSystemRepresentation, NULL);
    task_db_task_create(db, task_db_list_create(db, "Elsewhere", ""), 0, "The only task");
    task_db_close(db);
    [@"not a database" writeToFile:junk atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    sqlite3 *sq = NULL;
    sqlite3_open(old.fileSystemRepresentation, &sq);
    sqlite3_exec(sq, "CREATE TABLE tasks (id INTEGER PRIMARY KEY); PRAGMA user_version = 14",
                 NULL, NULL, NULL);
    sqlite3_close(sq);

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow *window = library.window;
        [library showWindow:nil];
        tasks_editor_open(app, first_task_ids(app, 1).firstObject.longLongValue);
        check_spin(0.2);

        [library open_database:junk];
        check_spin(0.3);
        check(window.attachedSheet != nil && [tasks_string(shared_db_file(app->db->shared)) isEqualToString:home],
              "open database: a file that is not a database is refused with an alert");
        check(secondary_windows(window) == 1, "open database: ... and the editors stay open");
        end_sheet(window, NSAlertFirstButtonReturn);

        [library open_database:old];
        check_spin(0.3);
        check(find_text(window.attachedSheet.contentView, @"not a shared database") != nil &&
              [tasks_string(shared_db_file(app->db->shared)) isEqualToString:home],
              "open database: an old Tasks database is refused in the shared module's words");
        end_sheet(window, NSAlertFirstButtonReturn);

        setenv("XDG_DATA_HOME", [folder stringByAppendingPathComponent:@"xdg"].fileSystemRepresentation, 1);
        [library open_database:live];
        check_spin(0.3);
        check(find_text(window.attachedSheet.contentView, @"refused") != nil &&
              [tasks_string(shared_db_file(app->db->shared)) isEqualToString:home],
              "open database: a path in the live data directory shows the core's refusal");
        end_sheet(window, NSAlertFirstButtonReturn);
        unsetenv("XDG_DATA_HOME");

        [library open_database:other];
        check_spin(0.3);
        check([tasks_string(shared_db_file(app->db->shared)) isEqualToString:other] && secondary_windows(window) == 0,
              "open database: a sound database is switched to, the editors closed first");
        check(find_text(window.contentView, @"All Tasks - 1 task") != nil,
              "open database: ... and the panes show its tasks");
        check(find_text(window.attachedSheet.contentView, @"Make this the default database?") != nil,
              "open database: ... and it is offered as the default");
        end_sheet(window, NSAlertFirstButtonReturn);
        check(find_text(window.attachedSheet.contentView, @"not changed from a sandboxed run") != nil,
              "open database: ... Make Default shows the shared module's refusal while CAMBIO_SHARED_DB is set");
        end_sheet(window, NSAlertFirstButtonReturn);

        [library open_database:other];
        check_spin(0.3);
        check(window.attachedSheet == nil && find_text(window.contentView, @"already open") != nil,
              "open database: the open database again only says so");

        [library open_database:home];
        check_spin(0.3);
        check([tasks_string(shared_db_file(app->db->shared)) isEqualToString:home] && window.attachedSheet == nil,
              "open database: back to the default database, with no question to ask");
        [library close];
    }
    check_spin(0.2);
    [files removeItemAtPath:folder error:NULL];
}

/*
 * collect_menu — note every command a menu reaches and every shortcut it
 * uses, submenus included.  A "Move to" submenu is empty until it opens, so
 * it counts as reaching the command its identifier names.
 *
 * Inputs:
 *   menu      — the menu to walk
 *   commands  — receives the commands found
 *   shortcuts — receives one string per shortcut found
 *   repeats   — incremented for each shortcut already in `shortcuts`
 */
static void
collect_menu(NSMenu *menu, NSMutableIndexSet *commands, NSMutableSet<NSString *> *shortcuts,
             int *repeats)
{
    const tasks_command_spec_t *filled = tasks_command_with_ident(menu.identifier);
    if (filled) [commands addIndex:(NSUInteger)filled->command];

    for (NSMenuItem *item in menu.itemArray) {
        if (item.action == @selector(run_command:)) [commands addIndex:(NSUInteger)item.tag];
        if (item.keyEquivalent.length > 0) {
            NSString *shortcut = [NSString stringWithFormat:@"%lu+%@",
                (unsigned long)item.keyEquivalentModifierMask, item.keyEquivalent];
            if ([shortcuts containsObject:shortcut]) (*repeats)++;
            [shortcuts addObject:shortcut];
        }
        if (item.submenu) collect_menu(item.submenu, commands, shortcuts, repeats);
    }
}

/*
 * check_menu_bar — the menu bar the app delegate builds: every command in
 * the command table can be reached from it, and no shortcut is used twice.
 */
static void
check_menu_bar(void)
{
    TasksAppDelegate *delegate = [[TasksAppDelegate alloc] init];
    [delegate build_menu_bar];

    NSMutableIndexSet        *commands  = [NSMutableIndexSet indexSet];
    NSMutableSet<NSString *> *shortcuts = [NSMutableSet set];
    int repeats = 0, missing = 0, total = 0;
    collect_menu(NSApp.mainMenu, commands, shortcuts, &repeats);

    for (NSInteger command = TasksCommandAbout; tasks_command_spec(command) != NULL; command++) {
        total++;
        if (![commands containsIndex:(NSUInteger)command]) {
            printf("     not in the menu bar: %s\n", tasks_command_spec(command)->title.UTF8String);
            missing++;
        }
    }
    check(total > 0 && missing == 0, "menus: every command is in the menu bar");
    check(repeats == 0, "menus: no shortcut is used twice");
    check(NSApp.mainMenu.numberOfItems == 6 && NSApp.windowsMenu != nil && NSApp.helpMenu != nil,
          "menus: App, File, Edit, View, Window and Help");
    NSApp.mainMenu = nil;
}

/*
 * check_memory_and_ownership — build the library window, use it for two
 * runs of cycles (the heap must not grow across the second), then close it
 * with editors still open and check that everything was released.
 *
 * Inputs:
 *   app — the application context
 */
static void
check_memory_and_ownership(TaskApp *app)
{
    __weak TasksLibraryWindow *weak_library = nil;
    __weak NSWindow           *weak_window  = nil;
    __weak NSView             *weak_content = nil;
    NSPointerArray            *secondary    = nil;   /* editors and Settings */

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        weak_library = library;
        weak_window  = library.window;
        weak_content = library.window.contentView;
        [library showWindow:nil];

        /* First run: caches settle.  Second run: must not grow. */
        use_windows(app, library, CYCLES);
        size_t   heap_mid      = heap_bytes();
        uint64_t footprint_mid = footprint_bytes();
        use_windows(app, library, CYCLES);
        long heap_growth = (long)heap_bytes() - (long)heap_mid;

        printf("     heap in use: %zu bytes, %+ld over %d cycles (budget %d)\n", heap_mid,
               heap_growth, CYCLES, CYCLES * HEAP_BUDGET_PER_CYCLE);
        printf("     footprint:   %.1f -> %.1f MB\n",
               footprint_mid / 1048576.0, footprint_bytes() / 1048576.0);
        check(heap_growth < CYCLES * HEAP_BUDGET_PER_CYCLE,
              "memory: heap growth with use is within the budget");
        check(icon_sized_regions() == 0, "memory: no full-size icon bitmaps (4 MB regions)");

        /* Closing the library window must take open editors with it. */
        for (NSNumber *task_id in first_task_ids(app, 3)) {
            tasks_editor_open(app, task_id.longLongValue);
        }
        tasks_settings_window_open(app);
        check_spin(0.05);
        secondary = other_windows(library.window);
        [library close];
        for (NSWindow *window in [NSApp.windows copy]) {
            if (window.visible) [window close];     /* Settings */
        }
    }
    check_spin(0.2);

    /* allObjects holds only the windows still alive (a weak pointer array
     * keeps its count when an entry goes nil). */
    check(secondary.count == 4 && secondary.allObjects.count == 0,
          "ownership: the editor and Settings windows are released when closed");
    check(weak_library == nil, "ownership: the library window controller is released");
    check(weak_window == nil, "ownership: its window is released");
    check(weak_content == nil, "ownership: its panes are released");
    check(visible_windows() == 0, "ownership: no window is left open");
    check(app->changed_l == NULL && app->tasks_l == NULL && app->status_l == NULL,
          "ownership: no event listener is left subscribed");
}

/*
 * check_lists_bar — drive the lists bar through what a user does with it,
 * without a pointer: the selection rules, the list and group sheets (ended
 * as their buttons would end them), the context menus, Move to Group and
 * Remove from Group, and the confirmations of Remove Group and Delete
 * List.
 *
 * It creates a group and a list in the sandbox and removes both again.
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder (the New List sheet is rendered into it)
 */
static void
check_lists_bar(TaskApp *app, NSString *dir)
{
    __weak TasksListsBar *weak_bar = nil;

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow           *window  = library.window;
        [library showWindow:nil];
        check_spin(0.2);

        /* Every view of one class under a root, parents before children. */
        NSArray<NSView *> *(^views_of)(NSView *, Class) = ^(NSView *root, Class class) {
            NSMutableArray<NSView *> *queue = [NSMutableArray arrayWithObject:root];
            NSMutableArray<NSView *> *found = [NSMutableArray array];
            for (NSUInteger i = 0; i < queue.count; i++) {
                if ([queue[i] isKindOfClass:class]) [found addObject:queue[i]];
                [queue addObjectsFromArray:queue[i].subviews];
            }
            return found;
        };
        TasksListsBar *bar = (TasksListsBar *)find_view(window.contentView, TasksListsBar.class);
        weak_bar = bar;
        check(bar != nil, "lists bar: the pane is a lists bar");

        /* The bar's buttons, left to right. */
        NSArray<NSButton *> *(^buttons)(void) = ^{
            NSMutableArray<NSButton *> *found = [NSMutableArray array];
            for (NSView *view in views_of(bar, NSButton.class)) [found addObject:(NSButton *)view];
            [found sortUsingComparator:^NSComparisonResult(NSButton *a, NSButton *b) {
                return [@(NSMinX(a.frame)) compare:@(NSMinX(b.frame))];
            }];
            return found;
        };
        /* The button whose title ends with a name, or nil. */
        NSButton *(^button_of)(NSString *) = ^(NSString *name) {
            for (NSButton *button in buttons()) {
                if ([button.title hasSuffix:name]) return button;
            }
            return (NSButton *)nil;
        };
        /* The titles of the buttons shown as selected, "A|B". */
        NSString *(^lit)(void) = ^{
            NSMutableArray<NSString *> *titles = [NSMutableArray array];
            for (NSButton *button in buttons()) {
                if (button.state == NSControlStateValueOn) [titles addObject:button.title];
            }
            return [titles componentsJoinedByString:@"|"];
        };
        /* The id of the list with a name, or 0. */
        int64_t (^list_named)(NSString *) = ^(NSString *name) {
            GPtrArray *lists = task_db_lists(app->db, FALSE);
            int64_t    found = 0;
            for (guint i = 0; i < lists->len; i++) {
                const TaskList *list = g_ptr_array_index(lists, i);
                if ([tasks_string(list->name) isEqualToString:name]) found = list->id;
            }
            task_ptr_array_free_lists(lists);
            return found;
        };
        /* The id of the group with a name, or 0. */
        int64_t (^group_named)(NSString *) = ^(NSString *name) {
            GPtrArray *groups = task_db_groups(app->db);
            int64_t    found  = 0;
            for (guint i = 0; i < groups->len; i++) {
                const TaskGroup *group = g_ptr_array_index(groups, i);
                if ([tasks_string(group->name) isEqualToString:name]) found = group->id;
            }
            task_ptr_array_free_groups(groups);
            return found;
        };
        /* The group a list is filed under. */
        int64_t (^group_of)(int64_t) = ^(int64_t list_id) {
            TaskList *list  = task_db_list_get(app->db, list_id);
            int64_t   group = list != NULL ? list->group_id : -1;
            task_list_free(list);
            return group;
        };
        /* Put a sheet's editable fields to `values` and end it as its
         * accept button would; NO when no sheet with that many fields is up. */
        BOOL (^accept_sheet)(NSArray<NSString *> *) = ^(NSArray<NSString *> *values) {
            check_spin(0.3);
            NSWindow *sheet = window.attachedSheet;
            NSMutableArray<NSTextField *> *fields = [NSMutableArray array];
            for (NSView *view in views_of(sheet.contentView, NSTextField.class)) {
                if (((NSTextField *)view).editable) [fields addObject:(NSTextField *)view];
            }
            if (sheet == nil || fields.count != values.count) return NO;
            for (NSUInteger i = 0; i < values.count; i++) fields[i].stringValue = values[i];
            [window endSheet:sheet returnCode:NSModalResponseOK];
            check_spin(0.3);
            return YES;
        };
        /* The context menu a right-click on a button gets, or on the empty
         * space at the bar's right end for nil.  The click never reaches
         * the screen: the bar is asked for its menu as AppKit would ask. */
        NSMenu *(^menu_for)(NSButton *) = ^(NSButton *button) {
            NSPoint point = button != nil
                ? [button convertPoint:NSMakePoint(NSMidX(button.bounds), NSMidY(button.bounds))
                                toView:nil]
                : [bar convertPoint:NSMakePoint(NSWidth(bar.bounds) - 1.0, NSMidY(bar.bounds))
                             toView:nil];
            NSEvent *click = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown
                                                location:point
                                           modifierFlags:0
                                               timestamp:0
                                            windowNumber:window.windowNumber
                                                 context:nil
                                             eventNumber:0
                                              clickCount:1
                                                pressure:1.0];
            return [bar menuForEvent:click];
        };

        /* Selection: it is the window's, shown as the lit button; a view
         * at first. */
        check(library.selection.kind == task_sel_view &&
              [lit() isEqualToString:tasks_string(task_view_nth((guint)library.selection.id)->label)],
              "lists bar: the window's selection is the lit button, a view at first");
        check(![library state_of_command:TasksCommandEditList].enabled,
              "lists bar: Edit List is disabled while a view is selected");

        int64_t admin = list_named(@"Admin");
        [library select:(task_sel_t){ task_sel_list, admin }];
        check([lit() isEqualToString:@"\u25BC  Work"], "lists bar: a grouped list lights its group's button");
        check([library state_of_command:TasksCommandEditList].enabled &&
              [library state_of_command:TasksCommandRemoveFromGroup].enabled,
              "lists bar: a grouped list enables Edit List and Remove from Group");
        [library select:(task_sel_t){ task_sel_group, group_named(@"Work") }];
        check([[library state_of_command:TasksCommandDeleteList].title isEqualToString:@"Remove Group"],
              "lists bar: Delete List offers to remove a selected group");

        /* The menu over the empty space (no button clicked). */
        NSMenu *menu = menu_for(nil);
        check(menu.numberOfItems == 2 && menu.itemArray[0].tag == TasksCommandNewList &&
              menu.itemArray[1].tag == TasksCommandNewGroup,
              "lists bar: the empty-space menu offers New List and New Group");

        /* The sheets: New Group, Rename Group, New List (with the emoji
         * slot), Edit List. */
        [library perform_command:TasksCommandNewGroup param:nil];
        check(accept_sheet(@[@"  Check Group "]) && group_named(@"Check Group") != 0 &&
              button_of(@"Check Group") != nil,
              "lists bar: New Group creates the group, its name trimmed, and shows its button");
        [library perform_command:TasksCommandRenameGroup param:@(group_named(@"Check Group"))];
        check(accept_sheet(@[@"Check Renamed"]) && group_named(@"Check Renamed") != 0 &&
              group_named(@"Check Group") == 0 && button_of(@"Check Renamed") != nil,
              "lists bar: Rename Group renames it");
        int64_t check_group = group_named(@"Check Renamed");

        [library perform_command:TasksCommandNewList param:nil];
        check_spin(0.3);
        NSTextField *emoji = (NSTextField *)views_of(window.attachedSheet.contentView,
                                                     NSClassFromString(@"TasksEmojiField")).firstObject;
        check([window.attachedSheet.firstResponder isKindOfClass:NSTextView.class] &&
              emoji.currentEditor == nil, "lists bar: the list sheet opens with the name being edited");
        [window.attachedSheet makeFirstResponder:emoji];
        [(NSTextView *)emoji.currentEditor insertText:@"ab\U0001F3AF" replacementRange:NSMakeRange(0, 0)];
        check_spin(0.1);
        check([emoji.stringValue isEqualToString:@"\U0001F3AF"] && emoji.currentEditor == nil,
              "lists bar: the emoji slot keeps one character and ends its edit");
        check(snapshot(window.attachedSheet, dir, @"lists-bar-list-sheet"), "snapshot: the New List sheet");
        check(accept_sheet(@[@"\U0001F3AF", @"Check List"]) && list_named(@"Check List") != 0,
              "lists bar: New List creates the list");
        int64_t check_list = list_named(@"Check List");
        check(library.selection.kind == task_sel_list && library.selection.id == check_list &&
              [lit() isEqualToString:@"\U0001F3AF  Check List"],
              "lists bar: the new list is selected and its button shows its emoji");

        [library perform_command:TasksCommandEditList param:nil];
        check(accept_sheet(@[@"", @"Check List With A Much Longer Name"]) &&
              list_named(@"Check List With A Much Longer Name") == check_list &&
              [lit() isEqualToString:@"Check List With A Much Longer Name"],
              "lists bar: Edit List renames the list and clears its emoji");
        [library perform_command:TasksCommandEditList param:nil];
        check(accept_sheet(@[@"", @"Check List"]) && [lit() isEqualToString:@"Check List"],
              "lists bar: ... and the button follows the name");

        /* Move to Group, the context menus of a group and of a list,
         * Remove from Group. */
        [library perform_command:TasksCommandMoveToGroup param:@(check_group)];
        check(group_of(check_list) == check_group && button_of(@"Check List") == nil &&
              [lit() isEqualToString:@"\u25BC  Check Renamed"],
              "lists bar: Move to Group files the selected list under its group's button");
        [library select:task_sel_default()];
        menu = menu_for(button_of(@"Check Renamed"));
        check(library.selection.kind == task_sel_default().kind &&
              library.selection.id == task_sel_default().id,
              "lists bar: a right-click on a group leaves the selection alone");
        check(menu.numberOfItems == 5 && menu.itemArray[3].tag == TasksCommandRenameGroup &&
              menu.itemArray[4].tag == TasksCommandDeleteGroup &&
              [menu.itemArray[4].representedObject longLongValue] == check_group,
              "lists bar: a group's menu adds Rename Group and Remove Group for that group");
        /* A left-click's menu: All Tasks (the group itself), its lists,
         * then the group's commands; the selection is checked. */
        menu = [bar menu_for_group:check_group];
        check(menu.numberOfItems == 6 && [menu.itemArray[0].title isEqualToString:@"All Tasks"] &&
              menu.itemArray[0].state == NSControlStateValueOff &&
              [menu.itemArray[2].title hasSuffix:@"Check List"] &&
              menu.itemArray[2].state == NSControlStateValueOff &&
              menu.itemArray[4].tag == TasksCommandRenameGroup &&
              menu.itemArray[5].tag == TasksCommandDeleteGroup,
              "lists bar: a group's click menu offers All Tasks, its lists, then its commands");
        [NSApp sendAction:menu.itemArray[2].action to:menu.itemArray[2].target from:menu.itemArray[2]];
        check(library.selection.kind == task_sel_list && library.selection.id == check_list,
              "lists bar: choosing a list in a group's menu selects that list");
        menu = [bar menu_for_group:check_group];
        check(menu.itemArray[0].state == NSControlStateValueOff &&
              menu.itemArray[2].state == NSControlStateValueOn,
              "lists bar: ... and the menu then checks that list");
        [NSApp sendAction:menu.itemArray[0].action to:menu.itemArray[0].target from:menu.itemArray[0]];
        check(library.selection.kind == task_sel_group && library.selection.id == check_group &&
              [lit() isEqualToString:@"\u25BC  Check Renamed"],
              "lists bar: All Tasks selects the group's aggregate view");
        menu = menu_for(button_of(@"Reading"));
        check(library.selection.kind == task_sel_list && library.selection.id == list_named(@"Reading"),
              "lists bar: a right-click selects the list it is on");
        check(menu.numberOfItems == 7 && menu.itemArray[3].tag == TasksCommandEditList &&
              menu.itemArray[4].tag == TasksCommandDeleteList &&
              menu.itemArray[6].submenu.numberOfItems == 3,
              "lists bar: an ungrouped list's menu adds Edit, Delete and Move to Group (every group)");
        /* A grouped list has no button: its group's menu carries its
         * commands while it is the selection, and keeps it selected. */
        [library select:(task_sel_t){ task_sel_list, check_list }];
        menu = menu_for(button_of(@"Check Renamed"));
        check(library.selection.kind == task_sel_list && library.selection.id == check_list &&
              menu.numberOfItems == 12 && !menu.itemArray[6].enabled &&
              [menu.itemArray[6].title isEqualToString:@"Check List"] &&
              menu.itemArray[7].tag == TasksCommandEditList &&
              menu.itemArray[10].submenu.numberOfItems == 2 &&
              menu.itemArray[11].tag == TasksCommandRemoveFromGroup,
              "lists bar: a group's menu adds its selected list's commands, under the list's name");
        [library perform_command:TasksCommandRemoveFromGroup param:nil];
        check(group_of(check_list) == 0 && button_of(@"Check List") != nil &&
              [lit() isEqualToString:@"Check List"],
              "lists bar: Remove from Group ungroups it, and it gets a button of its own");

        /* Dragging the list's button, through the bar's own drop: refused
         * on itself; onto a group it joins the group at its end; onto the
         * left half of an ungrouped list it lands in front of it, onto the
         * right half after it. */
        NSPoint (^at)(NSButton *, CGFloat) = ^(NSButton *button, CGFloat fraction) {
            return [bar convertPoint:NSMakePoint(NSMinX(button.bounds) + fraction * NSWidth(button.bounds),
                                                 NSMidY(button.bounds))
                            fromView:button];
        };
        /* The names of a group's lists (0: the ungrouped ones) in the saved
         * order, "A|B". */
        NSString *(^members)(int64_t) = ^(int64_t group_id) {
            NSMutableArray<NSString *> *names = [NSMutableArray array];
            GPtrArray *lists = task_db_lists(app->db, FALSE);
            for (guint i = 0; i < lists->len; i++) {
                const TaskList *list = g_ptr_array_index(lists, i);
                if (list->group_id == group_id) [names addObject:tasks_string(list->name)];
            }
            task_ptr_array_free_lists(lists);
            return [names componentsJoinedByString:@"|"];
        };
        check(![bar drop_list:check_list at:at(button_of(@"Check List"), 0.5)],
              "lists bar: a list dropped on itself goes nowhere");
        check([bar drop_list:check_list at:at(button_of(@"Work"), 0.5)], "lists bar: (dropped on Work)");
        check_spin(0.2);
        check(group_of(check_list) == group_named(@"Work") && button_of(@"Check List") == nil &&
              [members(group_named(@"Work")) hasSuffix:@"|Check List"],
              "lists bar: dropped on a group, the list joins it at the end");
        check([bar drop_list:check_list at:at(button_of(@"Reading"), 0.1)], "lists bar: (dropped before Reading)");
        check_spin(0.2);
        check(group_of(check_list) == 0 && button_of(@"Check List") != nil &&
              [members(0) containsString:@"Check List|Reading"],
              "lists bar: dropped on the left half of an ungrouped list, it lands in front of it, ungrouped");
        check([bar drop_list:check_list at:at(button_of(@"Reading"), 0.9)], "lists bar: (dropped after Reading)");
        check_spin(0.2);
        check([members(0) containsString:@"Reading|Check List"],
              "lists bar: ... and on the right half, after it");
        check(library.selection.kind == task_sel_list && library.selection.id == check_list,
              "lists bar: the selection stays on the list through its moves");

        /* Removing the group and deleting the list, each after its
         * confirmation; the list's saved orders go with it. */
        [library perform_command:TasksCommandDeleteGroup param:@(check_group)];
        check_spin(0.3);
        [window endSheet:window.attachedSheet returnCode:NSAlertFirstButtonReturn];
        check_spin(0.3);
        check(group_named(@"Check Renamed") == 0 && button_of(@"Check Renamed") == nil,
              "lists bar: Remove Group removes the group and its button");

        task_sel_t list_sel = { task_sel_list, check_list };
        [library select:list_sel];
        NSString  *order_key = [NSString stringWithFormat:@"%s_list_%lld", TASK_CFG_FAMILY_MANUAL,
                                (long long)check_list];
        task_order_save(TASK_CFG_FAMILY_MANUAL, list_sel, (int64_t[]){ 1, 2 }, 2);
        [library perform_command:TasksCommandDeleteList param:nil];
        check_spin(0.3);
        [window endSheet:window.attachedSheet returnCode:NSAlertFirstButtonReturn];
        check_spin(0.3);
        gchar *saved_order = task_config_get(order_key.UTF8String);
        check(list_named(@"Check List") == 0 && saved_order == NULL,
              "lists bar: Delete List deletes the list and drops its saved orders");
        g_free(saved_order);

        /* The first list, as the bar orders them: the first group's first
         * member, else the first ungrouped list. */
        int64_t    first_list = 0;
        GPtrArray *groups     = task_db_groups(app->db);
        GPtrArray *lists      = task_db_lists(app->db, FALSE);
        int64_t    first_grp  = groups->len > 0 ? ((TaskGroup *)g_ptr_array_index(groups, 0))->id : 0;
        for (guint i = 0; i < lists->len && first_list == 0; i++) {
            const TaskList *list = g_ptr_array_index(lists, i);
            if (list->group_id == first_grp) first_list = list->id;
        }
        task_ptr_array_free_lists(lists);
        task_ptr_array_free_groups(groups);
        check(library.selection.kind == task_sel_list && library.selection.id == first_list &&
              lit().length > 0,
              "lists bar: the selection falls back to the first list");
        task_db_list_purge(app->db, check_list);

        [library close];
    }
    check_spin(0.2);
    check(weak_bar == nil, "lists bar: the bar is released with its window");
}

/*
 * check_task_list — the task list in the library window: with a few
 * thousand generated rows (a large real database) for speed, row heights,
 * sorting and selection, then with the sandbox's own tasks for the
 * checkbox and the Return key.  It knows the list from outside only: the
 * pane and its table are found in the view tree, cells by column
 * identifier.  Everything it changes (settings, a task's status) is put
 * back.
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder
 */
static void
check_task_list(TaskApp *app, NSString *dir)
{
    enum { many = 5000 };

    /* Rows for `many` tasks that exist only in memory: every third has a
     * note long enough to wrap, every fifth is Done. */
    task_row_list_t *(^generate)(void) = ^{
        task_row_list_t *rows = calloc(1, sizeof *rows);
        TaskRowCtx ctx;
        gint64 today, tomorrow;

        task_day_bounds(0, &today, &tomorrow);
        task_row_ctx_init(app, &ctx, FALSE);
        rows->rows  = calloc(many, sizeof *rows->rows);
        rows->count = many;
        for (int i = 0; i < many; i++) {
            char title[64];
            snprintf(title, sizeof title, "Generated task %04d", (i * 7919) % many);
            Task task = {
                .id = 1000000 + i, .title = title, .priority = i % 7 == 0,
                .notes = i % 3 == 0 ? "A note long enough that it has to wrap onto a second "
                                      "line when the window is made narrow, as notes often are."
                                    : "",
                .due = i % 4 == 0 ? 0 : today + (i % 9 - 4) * 86400,
                .due_time = SHARED_DB_TASK_DUE_TIME_DEFAULT,
                .status = i % 5 == 0 ? TASK_STATUS_DONE : TASK_STATUS_NEW,
                .completed_at = i % 5 == 0 ? today : 0,
            };
            rows->rows[i] = task_row_describe(&task, &ctx);
        }
        task_row_ctx_clear(&ctx);
        return rows;
    };

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow *window = library.window;
        [library showWindow:nil];
        /* The list must be the visible pane: a run that stopped in the
         * Kanban checks leaves the setting on the board.  And the day
         * planner hidden: the widths below are the whole pane's, and a
         * window set narrower than its minimum (setContentSize: does not
         * hold to it) would leave the pane beside a planner too narrow for
         * any columns.  The user's choice is put back at the end. */
        if (library.kanban_on) [library perform_command:TasksCommandTogglePane param:nil];
        BOOL planner_shown = !library.day_planner.hidden;
        if (planner_shown) {
            [library perform_command:TasksCommandToggleDayPlanner param:nil];
            check_spin(0.3);
        }

        /* The list pane, wherever the window keeps it. */
        TasksTaskList *list = nil;
        NSMutableArray<NSView *> *pending = [NSMutableArray arrayWithObject:window.contentView];
        while (list == nil && pending.count > 0) {
            NSView *view = pending.firstObject;
            [pending removeObjectAtIndex:0];
            [pending addObjectsFromArray:view.subviews];
            if ([view isKindOfClass:TasksTaskList.class]) list = (TasksTaskList *)view;
        }
        NSScrollView *scroll = list.subviews.firstObject;
        NSTableView  *table  = scroll.documentView;
        NSInteger task_col   = [table columnWithIdentifier:@"task"];

        /* Whether every row on screen is exactly as tall as its text needs
         * at the width its cell really has. */
        BOOL (^heights_exact)(void) = ^{
            NSRange shown = [table rowsInRect:table.visibleRect];
            BOOL    exact = shown.length > 0;

            for (NSUInteger row = shown.location; row < NSMaxRange(shown); row++) {
                NSView *cell = [table viewAtColumn:task_col row:(NSInteger)row makeIfNecessary:NO];
                NSAttributedString *text = [cell valueForKey:@"text"];
                CGFloat want = tasks_row_text_height(
                    text, NSWidth(cell.bounds) - 2.0 * tasks_look_row_pad_h) +
                    2.0 * tasks_look_row_pad_v;
                if (cell == nil || fabs(NSHeight([table rectOfRow:(NSInteger)row]) - want) > 0.01) {
                    exact = NO;
                }
            }
            return exact;
        };
        BOOL (^columns_fit)(void) = ^{
            return (BOOL)(NSWidth(table.frame) <= scroll.contentSize.width + 0.5);
        };
        /* Lay out and draw, as a turn of the run loop would; timed. */
        double (^settle)(CFAbsoluteTime) = ^(CFAbsoluteTime began) {
            [window layoutIfNeeded];
            [window display];
            double took = CFAbsoluteTimeGetCurrent() - began;
            check_spin(0.05);
            return took;
        };

        CFAbsoluteTime began = CFAbsoluteTimeGetCurrent();
        [list show_rows:generate()];
        double first_show = settle(began);
        check(table.numberOfRows == many, "list: thousands of rows are shown");
        check(heights_exact(), "list: rows on screen are as tall as their text");
        check(columns_fit(), "list: the columns fill the pane without overflowing it");
        check(snapshot(window, dir, @"task-list-many"), "snapshot: task list, 5000 rows");

        /* A refresh in the middle of the list keeps its place. */
        [table scrollRowToVisible:many / 2];
        settle(0);
        check(heights_exact(), "list: rows scrolled to are as tall as their text");
        CGFloat place = NSMinY(table.visibleRect);
        began = CFAbsoluteTimeGetCurrent();
        [list show_rows:generate()];
        double refresh = settle(began);
        check(fabs(NSMinY(table.visibleRect) - place) < 0.5, "list: a refresh keeps the scroll position");

        NSSize full_size = window.contentView.frame.size;
        began = CFAbsoluteTimeGetCurrent();
        [window setContentSize:NSMakeSize(tasks_look_pane_min_w + 100.0, full_size.height)];
        double resize = settle(began);
        check(heights_exact(), "list: rows are re-measured when the pane gets narrower");
        check(columns_fit(), "list: ... and the columns still fit");
        check(snapshot(window, dir, @"task-list-narrow"), "snapshot: task list, narrow");
        [window setContentSize:full_size];
        printf("     %d rows: first show %.0f ms, refresh %.0f ms, resize %.0f ms\n", many,
               first_show * 1000.0, refresh * 1000.0, resize * 1000.0);
        check(first_show < 0.5 && refresh < 0.5 && resize < 0.5,
              "list: showing, refreshing and resizing thousands of rows is quick");

        /* Header sort: by title, descending.  The selection follows its
         * tasks, and the sort is remembered. */
        [table selectRowIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(3, 2)]
           byExtendingSelection:NO];
        NSSet<NSNumber *> *chosen = [NSSet setWithArray:[list selected_task_ids]];
        table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"title" ascending:NO]];
        settle(0);
        check([chosen isEqualToSet:[NSSet setWithArray:[list selected_task_ids]]] && chosen.count == 2,
              "list: sorting keeps the same tasks selected");
        int64_t last_title = 0;
        for (int i = 0; i < many; i++) {
            if ((i * 7919) % many == many - 1) last_title = 1000000 + i;
        }
        [table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        check([list selected_task_ids].firstObject.longLongValue == last_title,
              "list: a header sort orders the rows");
        gchar *saved = task_config_get(TASK_CFG_LIST_SORT);
        check(saved != NULL && strcmp(saved, "-title") == 0, "list: the header sort is remembered");
        g_free(saved);

        /* A column shown from the header menu's command. */
        [table deselectAll:nil];
        [table scrollRowToVisible:0];
        [library perform_command:TasksCommandColumnStatus param:nil];
        settle(0);
        check(![table tableColumnWithIdentifier:@"status"].hidden && columns_fit() && heights_exact(),
              "list: a column command shows its column and the rest make room");
        check(snapshot(window, dir, @"task-list-sorted"), "snapshot: task list, sorted, Status shown");
        [library perform_command:TasksCommandColumnStatus param:nil];

        /* Sorted by the last column: its bold title and the indicator fit,
         * and the divider on its left survives the header fill. */
        table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"completed" ascending:NO]];
        settle(0);
        check(snapshot(window, dir, @"task-list-sorted-completed"),
              "snapshot: task list, sorted by Completed");
        table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"title" ascending:NO]];
        settle(0);

        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        check(snapshot(window, dir, @"task-list-dark"), "snapshot: task list, sorted, dark");
        window.appearance = nil;
        /* The sandbox's tasks again, for the checks that read them back. */
        [library refresh_tasks];
        settle(0);

        /* Tick the first task's checkbox while sorted by Done, so that the
         * row has to move as well as change. */
        table.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"done" ascending:YES]];
        settle(0);
        NSInteger rows_shown = table.numberOfRows;
        [table selectRowIndexes:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0, 2)]
           byExtendingSelection:NO];
        NSArray<NSNumber *> *picked = [list selected_task_ids];
        Task *before = task_db_task_get(app->db, picked[0].longLongValue);
        NSButton *box = [table viewAtColumn:[table columnWithIdentifier:@"done"] row:0
                            makeIfNecessary:YES].subviews.firstObject;
        [box performClick:nil];
        settle(0);
        Task *after = task_db_task_get(app->db, picked[0].longLongValue);
        check(before->status != TASK_STATUS_DONE && after->status == TASK_STATUS_DONE,
              "list: the checkbox completes its task");
        NSArray<NSNumber *> *still = [list selected_task_ids];
        check(still.count == 2 && [still.firstObject isEqual:picked[1]] &&
              [still.lastObject isEqual:picked[0]] && table.numberOfRows == rows_shown,
              "list: ... in place: its row moves to the Done rows, still selected");
        check(heights_exact(), "list: ... and the rows are still as tall as their text");
        task_db_task_set_status(app->db, before->id, before->status);
        task_free(before);
        task_free(after);
        table.sortDescriptors = @[];
        settle(0);

        /* An outside change (another connection, as Notes) to a task that
         * is NOT selected: the pane refreshes and shows it, keeping the
         * selection and the scroll position. */
        [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)table.numberOfRows - 1]
           byExtendingSelection:NO];
        gint64 target = [list selected_task_ids].firstObject.longLongValue;   /* the last row */
        [table selectRowIndexes:[NSIndexSet indexSetWithIndex:1] byExtendingSelection:NO];
        [table scrollRowToVisible:table.numberOfRows - 1];
        settle(0);
        NSArray<NSNumber *> *kept   = [list selected_task_ids];
        NSPoint              origin = table.enclosingScrollView.contentView.bounds.origin;
        TaskDatabase *other = task_db_open(shared_db_file(app->db->shared), NULL);
        Task         *seen  = task_db_task_get(other, target);
        Task          moved = *seen;
        moved.title = (gchar *)"Renamed in another app";
        task_db_task_update(other, seen, &moved);
        task_db_close(other);
        check(task_app_check_outside(app), "list: (an outside change is noticed)");
        settle(0);
        /* The row text is drawn by its cell (task_list.m), not a text field:
         * read the cell's `text`. */
        BOOL (^drawn)(NSView *, NSString *) = ^BOOL(NSView *root, NSString *want) {
            NSMutableArray<NSView *> *todo = [NSMutableArray arrayWithObject:root];
            while (todo.count > 0) {
                NSView *view = todo.lastObject;
                [todo removeLastObject];
                if ([view respondsToSelector:NSSelectorFromString(@"text")]) {
                    id text = [view valueForKey:@"text"];
                    if ([text isKindOfClass:NSAttributedString.class] &&
                        [((NSAttributedString *)text).string containsString:want]) return YES;
                }
                [todo addObjectsFromArray:view.subviews];
            }
            return NO;
        };
        check(drawn(table, @"Renamed in another app") &&
              [[list selected_task_ids] isEqualToArray:kept] &&
              NSEqualPoints(table.enclosingScrollView.contentView.bounds.origin, origin),
              "list: an outside change shows up, keeping the selection and the scroll");
        Task *renamed = task_db_task_get(app->db, target);
        task_db_task_update(app->db, renamed, seen);
        task_free(renamed);
        task_free(seen);
        [library refresh_tasks];
        settle(0);

        /* Return opens the editor of the one selected task. */
        [table selectRowIndexes:[NSIndexSet indexSetWithIndex:1] byExtendingSelection:NO];
        NSUInteger open_before = visible_windows();
        [list keyDown:[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                                  modifierFlags:0 timestamp:0 windowNumber:window.windowNumber
                                        context:nil characters:@"\r"
                    charactersIgnoringModifiers:@"\r" isARepeat:NO keyCode:36]];
        check_spin(0.05);
        check(visible_windows() == open_before + 1, "list: Return opens the selected task's editor");

        if (planner_shown) {
            [library perform_command:TasksCommandToggleDayPlanner param:nil];
            check_spin(0.3);
        }
        [library close];
    }
    check_spin(0.2);
}

/*
 * check_kanban — the Kanban board, exercised without a real mouse: the
 * cards a view should show, the selection rules (mouse events handed
 * straight to the cards), the refresh that keeps every card's view and the
 * scroll position, a saved order moving the same views, the Done lane's cap
 * and link, the shadow setting, double-click, resizing, and release on
 * close.  Snapshots: kanban-selected, kanban-dark, kanban-flat,
 * kanban-flat-dark, kanban-done-link, kanban-empty.
 *
 * NOT covered here: the drag itself.  A dragging session needs a real
 * pointer; what a drop DOES is tested in tests/board_test.c.
 *
 * The Done link needs more completed tasks than the seed has, so six are
 * created and deleted again (tombstoned: no view shows them afterwards).
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder
 */
static void
check_kanban(TaskApp *app, NSString *dir)
{
    __weak NSView *weak_card = nil;

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow *window     = library.window;
        Class     card_class = NSClassFromString(@"TasksKanbanCard");
        BOOL      was_kanban = library.kanban_on;
        BOOL      was_shadow = task_config_get_bool(TASK_CFG_KANBAN_SHADOW);
        NSRect    was_frame  = window.frame;

        [library showWindow:nil];
        if (!was_kanban) [library perform_command:TasksCommandTogglePane param:nil];
        [window layoutIfNeeded];

        /* Every view of a class under the window, in board order: lane by
         * lane, top to bottom. */
        NSArray<NSView *> *(^views_of)(Class) = ^(Class wanted) {
            NSMutableArray<NSView *> *found = [NSMutableArray array];
            NSMutableArray<NSView *> *queue = [NSMutableArray arrayWithObject:window.contentView];
            while (queue.count > 0) {
                NSView *view = queue.firstObject;
                [queue removeObjectAtIndex:0];
                if ([view isKindOfClass:wanted] && !view.hiddenOrHasHiddenAncestor) {
                    [found addObject:view];
                }
                [queue addObjectsFromArray:view.subviews];
            }
            [found sortUsingComparator:^NSComparisonResult(NSView *a, NSView *b) {
                NSRect ra = [a convertRect:a.bounds toView:nil];
                NSRect rb = [b convertRect:b.bounds toView:nil];
                if (NSMinX(ra) != NSMinX(rb)) return NSMinX(ra) < NSMinX(rb) ? NSOrderedAscending
                                                                             : NSOrderedDescending;
                return NSMaxY(ra) > NSMaxY(rb) ? NSOrderedAscending : NSOrderedDescending;
            }];
            return found;
        };
        /* A mouse event over a card's text, handed to the card itself (the
         * window is not key, and must not be made key under the user). */
        void (^mouse)(NSView *, NSEventType, NSEventModifierFlags, NSInteger) =
            ^(NSView *card, NSEventType type, NSEventModifierFlags flags, NSInteger clicks) {
            NSRect   rect  = [card convertRect:card.bounds toView:nil];
            NSEvent *event = [NSEvent mouseEventWithType:type
                                                location:NSMakePoint(NSMaxX(rect) - 10.0, NSMidY(rect))
                                           modifierFlags:flags
                                               timestamp:NSProcessInfo.processInfo.systemUptime
                                            windowNumber:window.windowNumber
                                                 context:nil
                                             eventNumber:0
                                              clickCount:clicks
                                                pressure:1.0];
            if (type == NSEventTypeLeftMouseDown) [card mouseDown:event]; else [card mouseUp:event];
        };
        void (^click)(NSView *, NSEventModifierFlags) = ^(NSView *card, NSEventModifierFlags flags) {
            mouse(card, NSEventTypeLeftMouseDown, flags, 1);
            mouse(card, NSEventTypeLeftMouseUp, flags, 1);
        };

        /* The cards: one per row the window collected (All Tasks: fewer
         * than the cap are Done, so every task has a card). */
        task_pane_content_t *content = task_pane_collect(app, library.selection, NULL);
        NSUInteger           n_tasks = content->rows->count;
        task_pane_content_free(content);
        NSArray<NSView *> *cards = views_of(card_class);
        weak_card = cards.firstObject;
        check(n_tasks > 8 && cards.count == n_tasks, "kanban: one card per task of the view");
        if (cards.count <= 8) return;

        NSRect  first = [cards[0] convertRect:cards[0].bounds toView:nil];
        NSView *hit   = [window.contentView hitTest:
            [window.contentView.superview convertPoint:NSMakePoint(NSMidX(first), NSMidY(first))
                                              fromView:nil]];
        check(hit == cards[0], "kanban: a click on a card reaches the card");
        check(library.selected_task_ids.count == 0, "kanban: nothing is selected to begin with");

        /* Selection.  cards[0..] are the New lane, top to bottom. */
        NSUInteger new_count = 0;
        while (new_count < cards.count &&
               cards[new_count].superview == cards[0].superview) new_count++;
        NSView *other_lane_card = new_count < cards.count ? cards[new_count] : nil;

        click(cards[0], 0);
        NSNumber *id_0 = library.selected_task_ids.firstObject;
        check(library.selected_task_ids.count == 1, "kanban: a click selects one card");
        click(cards[2], NSEventModifierFlagCommand);
        check(library.selected_task_ids.count == 2 &&
              [library.selected_task_ids[0] isEqual:id_0],
              "kanban: a Command-click adds a card; ids come back in display order");
        click(cards[4], NSEventModifierFlagShift);
        check(library.selected_task_ids.count == 4,
              "kanban: a Shift-click selects the run from the last Command-clicked card");
        click(cards[3], NSEventModifierFlagCommand);
        check(library.selected_task_ids.count == 3, "kanban: a Command-click removes a selected card");
        if (other_lane_card != nil) {
            click(other_lane_card, NSEventModifierFlagShift);
            check(library.selected_task_ids.count == 4,
                  "kanban: a Shift-click in another lane only adds its card");
        }
        check(snapshot(window, dir, @"kanban-selected"), "snapshot: board with a selection");
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        check(snapshot(window, dir, @"kanban-dark"), "snapshot: board, dark");

        /* The shadow setting, changed the way Settings changes it. */
        task_config_set_bool(TASK_CFG_KANBAN_SHADOW, !was_shadow);
        task_app_notify_tasks(app);
        check(snapshot(window, dir, @"kanban-flat-dark"), "snapshot: board, dark, shadow toggled");
        window.appearance = nil;
        check(snapshot(window, dir, @"kanban-flat"), "snapshot: board, shadow toggled");
        task_config_set_bool(TASK_CFG_KANBAN_SHADOW, was_shadow);
        task_app_notify_tasks(app);

        mouse(cards[2], NSEventTypeLeftMouseDown, 0, 1);
        check(library.selected_task_ids.count == 4,
              "kanban: a press inside a multi-selection keeps it (a drag may follow)");
        mouse(cards[2], NSEventTypeLeftMouseUp, 0, 1);
        check(library.selected_task_ids.count == 1,
              "kanban: ... and the release collapses it to the clicked card");

        /* A refresh keeps every card's view, the selection and the scroll. */
        NSScrollView *scroll = cards[0].enclosingScrollView;
        [scroll.documentView scrollPoint:NSMakePoint(0.0, 90.0)];
        CGFloat scrolled_to = scroll.contentView.bounds.origin.y;
        [library full_refresh];
        check([views_of(card_class) isEqualToArray:cards],
              "kanban: a refresh with nothing changed keeps every card view");
        check(scrolled_to > 0.0 && scroll.contentView.bounds.origin.y == scrolled_to,
              "kanban: ... and the scroll position");
        check(library.selected_task_ids.count == 1, "kanban: ... and the selection");

        /* A saved order (what a drop writes) moves the SAME views. */
        click(cards[1], 0);
        NSNumber *id_1 = library.selected_task_ids.firstObject;
        int64_t swapped[] = { id_1.longLongValue, id_0.longLongValue };
        task_order_save(TASK_CFG_FAMILY_KANBAN, library.selection, swapped, 2);
        [library refresh_tasks];
        NSArray<NSView *> *reordered = views_of(card_class);
        check(reordered.count == cards.count && reordered[0] == cards[1] && reordered[1] == cards[0],
              "kanban: a saved order rearranges the existing card views");
        check([library.selected_task_ids isEqual:@[id_1]], "kanban: the selection follows its card");
        task_order_save(TASK_CFG_FAMILY_KANBAN, library.selection, NULL, 0);
        [library refresh_tasks];
        check([views_of(card_class) isEqualToArray:cards], "kanban: ... and back");

        /* The Done cap and its link. */
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        int64_t    extra[6];
        for (int i = 0; i < 6; i++) {
            int64_t list_id = ((const TaskList *)g_ptr_array_index(lists, 0))->id;
            extra[i] = task_db_task_create(app->db, list_id, 0, "ui-check: completed");
            task_db_task_set_status(app->db, extra[i], TASK_STATUS_DONE);
        }
        [library refresh_tasks];
        NSButton *link = nil;
        for (NSView *view in views_of(NSButton.class)) {
            if (view.enclosingScrollView == scroll) link = (NSButton *)view;
        }
        NSUInteger capped = views_of(card_class).count;
        check(link != nil && capped < n_tasks + 6 &&
              [link.title isEqualToString:[NSString stringWithFormat:@"Show all %lu completed",
                                           (unsigned long)(n_tasks + 6 - capped + 10)]],
              "kanban: more than ten Done tasks are capped, behind a link naming the total");
        check(snapshot(window, dir, @"kanban-done-link"), "snapshot: board with the Done link");
        [link performClick:nil];
        check(views_of(card_class).count == n_tasks + 6 &&
              [link.title isEqualToString:@"Show recent only"],
              "kanban: the link shows every completed task and offers the way back");
        [library select:(task_sel_t){ task_sel_list, ((const TaskList *)g_ptr_array_index(lists, 0))->id }];
        [library select:task_sel_default()];
        check(views_of(card_class).count == capped && library.selected_task_ids.count == 0,
              "kanban: changing view resets the expansion and the selection");
        for (int i = 0; i < 6; i++) task_db_task_delete(app->db, extra[i]);
        [library refresh_tasks];
        check(link.hidden && views_of(card_class).count == n_tasks,
              "kanban: the link goes when ten or fewer tasks are Done");

        /* An empty view: three lanes that say they take drops. */
        for (guint i = 0; i < lists->len; i++) {
            const TaskList *list = g_ptr_array_index(lists, i);
            if (strcmp(list->name, "Empty List") != 0) continue;
            [library select:(task_sel_t){ task_sel_list, list->id }];
            [library refresh_sidebar];
            check(views_of(card_class).count == 0 && snapshot(window, dir, @"kanban-empty"),
                  "snapshot: board of an empty list");
            [library select:task_sel_default()];
            [library refresh_sidebar];
        }
        task_ptr_array_free_lists(lists);

        /* Resizing the window re-lays the lanes. */
        cards = views_of(card_class);
        CGFloat card_width = NSWidth(cards[0].frame);
        [window setFrame:NSInsetRect(was_frame, 90.0, 0.0) display:YES];
        [window layoutIfNeeded];
        check(NSWidth(cards[0].frame) < card_width, "kanban: a narrower window narrows the lanes");
        [window setFrame:was_frame display:YES];
        [window layoutIfNeeded];
        check(NSWidth(cards[0].frame) == card_width, "kanban: ... and back");

        /* A double click opens the card's editor. */
        NSUInteger windows_before = visible_windows();
        mouse(cards[0], NSEventTypeLeftMouseDown, 0, 2);
        mouse(cards[0], NSEventTypeLeftMouseUp, 0, 2);
        check_spin(0.1);
        check(visible_windows() == windows_before + 1, "kanban: a double click opens the editor");
        tasks_editor_close_all();

        [library perform_command:TasksCommandTogglePane param:nil];
        [library perform_command:TasksCommandTogglePane param:nil];
        check(library.selected_task_ids.count == 0,
              "kanban: hiding the board drops its selection");
        if (!was_kanban) [library perform_command:TasksCommandTogglePane param:nil];
        [library close];
    }
    check_spin(0.2);
    check(weak_card == nil, "kanban: the cards are released with the window");
}

/*
 * send_escape — deliver an Escape key-down event to a window, as if the
 * user pressed the key with that window front.
 *
 * Inputs:
 *   win — the target window
 */
static void
send_escape(NSWindow *win)
{
    NSEvent *esc = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:win.windowNumber
                                     context:nil
                                 characters:@"\x1b"
                charactersIgnoringModifiers:@"\x1b"
                                   isARepeat:NO
                                     keyCode:53];
    [NSApp sendEvent:esc];
}

/*
 * check_editor_window — exercise editor semantics: dedup open, cancel-on-new
 * deletes the task, refresh-all closes an editor whose task is gone, an
 * outside change merged under an unsaved edit, and the "From note" line of
 * a Notes action item (snapshot editor-note-item).
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder
 */
static void
check_editor_window(TaskApp *app, NSString *dir)
{
    NSArray<NSNumber *> *ids = first_task_ids(app, 2);
    if (ids.count < 1) { printf("     skipping editor checks (no tasks)\n"); return; }

    /* Opening the same task twice must not create two windows. */
    tasks_editor_open(app, ids[0].longLongValue);
    tasks_editor_open(app, ids[0].longLongValue);
    check_spin(0.05);
    check(visible_windows() == 1, "editor: opening the same task twice shows one window");
    tasks_editor_close(ids[0].longLongValue);
    check_spin(0.1);
    check(visible_windows() == 0, "editor: tasks_editor_close closes the target");

    /* refresh_all must close an editor whose task has been deleted.  Use a
     * temporary task so the seed database is not permanently mutated. */
    {
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        if (lists->len > 0) {
            gint64 list_id  = ((const TaskList *)g_ptr_array_index(lists, 0))->id;
            gint64 temp_id  = task_db_task_create(app->db, list_id, 0, "refresh-test");
            tasks_editor_open(app, temp_id);
            check_spin(0.05);
            task_db_task_delete(app->db, temp_id);
            tasks_editor_refresh_all();
            check_spin(0.1);
            check(visible_windows() == 0, "editor: refresh_all closes an orphaned editor");
        }
        task_ptr_array_free_lists(lists);
    }

    /* Cancel on a new task: the task is deleted when the editor closes. */
    {
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        if (lists->len > 0) {
            gint64 list_id = ((const TaskList *)g_ptr_array_index(lists, 0))->id;
            gint64 new_id  = task_db_task_create(app->db, list_id, 0, "cancel-test");

            tasks_editor_open_new(app, new_id);
            check_spin(0.05);
            NSWindow *editor_win = nil;
            for (NSWindow *w in NSApp.windows) {
                if (w.visible) { editor_win = w; break; }
            }
            if (editor_win) {
                send_escape(editor_win);
                check_spin(0.2);
            }
            check(visible_windows() == 0, "editor: Cancel on a new task closes the editor");
            /* Verify the task is gone by trying to open it — open returns without
             * showing a window when the task does not exist. */
            tasks_editor_open(app, new_id);
            check_spin(0.05);
            check(visible_windows() == 0, "editor: Cancel on a new task deletes the task");
        }
        task_ptr_array_free_lists(lists);
    }

    /* An outside change while the editor has an unsaved edit: the typed
     * title wins, the field changed elsewhere takes the outside value, and
     * neither write is lost.  The "outside" writer is a second connection,
     * as Notes would be; the library window is what hears of the change. */
    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        [library showWindow:nil];
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        if (lists->len > 0) {
            gint64 list_id = ((const TaskList *)g_ptr_array_index(lists, 0))->id;
            gint64 id      = task_db_task_create(app->db, list_id, 0, "merge-test");

            tasks_editor_open(app, id);
            check_spin(0.05);
            NSWindow *editor_win = nil;
            for (NSWindow *w in NSApp.windows) {
                if (w.visible && [w.title isEqualToString:@"merge-test"]) editor_win = w;
            }
            NSTextField *title = (NSTextField *)find_text(editor_win.contentView, @"merge-test");
            title.stringValue = @"typed here";
            [NSNotificationCenter.defaultCenter
                postNotificationName:NSControlTextDidChangeNotification object:title];

            TaskDatabase *other = task_db_open(shared_db_file(app->db->shared), NULL);
            Task         *seen  = task_db_task_get(other, id);
            Task          moved = *seen;
            moved.priority = TRUE;
            moved.notes    = "written elsewhere";
            task_db_task_update(other, seen, &moved);
            task_free(seen);
            task_db_close(other);

            check(task_app_check_outside(app), "editor: (an outside change is noticed)");
            Task *now = task_db_task_get(app->db, id);
            check(now != NULL && strcmp(now->title, "typed here") == 0 && now->priority &&
                  strcmp(now->notes, "written elsewhere") == 0,
                  "editor: an outside change keeps the unsaved title and the outside fields");
            task_free(now);
            check(find_text(editor_win.contentView, @"typed here") != nil,
                  "editor: ... and the editor still shows what was typed");
            tasks_editor_close(id);
            check_spin(0.1);
            task_db_task_delete(app->db, id);
        }
        task_ptr_array_free_lists(lists);
        [library close];
    }

    /* A save that fails because another writer holds the file past the
     * busy timeout loses nothing: the edit stays in the editor, closing
     * is refused with a notice, and once the file is free the close saves
     * it.  The holder is a second connection in a write transaction. */
    @autoreleasepool {
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        if (lists->len > 0) {
            gint64 list_id = ((const TaskList *)g_ptr_array_index(lists, 0))->id;
            gint64 id      = task_db_task_create(app->db, list_id, 0, "busy-test");

            tasks_editor_open(app, id);
            check_spin(0.1);
            NSWindow *editor_win = nil;
            for (NSWindow *w in NSApp.windows) {
                if (w.visible && [w.title isEqualToString:@"busy-test"]) editor_win = w;
            }
            NSButton *favorite = find_button(editor_win.contentView, @"Favorite");

            TaskDatabase *holder = task_db_open(shared_db_file(app->db->shared), NULL);
            check(shared_db_begin(holder->shared, NULL) == 0, "editor: (another writer holds the file)");
            [favorite performClick:nil];
            Task *now = task_db_task_get(app->db, id);
            check(now != NULL && !now->pinned && favorite.state == NSControlStateValueOn &&
                  [[editor_win.windowController valueForKey:@"save_pending"] boolValue],
                  "editor: a save that fails keeps the edit, still pending");
            task_free(now);

            [editor_win performClose:nil];
            check_spin(0.3);
            check(editor_win.visible && editor_win.attachedSheet != nil,
                  "editor: ... closing is refused with a notice while it cannot be saved");
            if (editor_win.attachedSheet) [editor_win endSheet:editor_win.attachedSheet];
            check_spin(0.2);

            shared_db_rollback(holder->shared);
            task_db_close(holder);
            [editor_win performClose:nil];
            check_spin(0.2);
            now = task_db_task_get(app->db, id);
            check(!editor_win.visible && now != NULL && now->pinned,
                  "editor: ... and once the file is free, closing saves it");
            task_free(now);
            task_db_task_delete(app->db, id);
        }
        task_ptr_array_free_lists(lists);
    }

    /* A Notes action item: a note_task row links the task to a note, as
     * Notes writes it.  The editor names the note; a plain task has no
     * such line.  The rows are removed again (dropping the link tombstones
     * the throwaway task, as the shared schema's trigger does). */
    {
        GPtrArray *lists = task_db_lists(app->db, FALSE);
        if (lists->len > 0) {
            gint64   list_id = ((const TaskList *)g_ptr_array_index(lists, 0))->id;
            gint64   id      = task_db_task_create(app->db, list_id, 0, "Call the vendor back");
            sqlite3 *sq      = shared_db_handle(app->db->shared);
            char    *sql     = sqlite3_mprintf(
                "INSERT INTO notes (title, created_at, updated_at) "
                "  VALUES ('Weekly sync', 0, 0);"
                "INSERT INTO note_task (task_id, note_id, ord, seen_title) "
                "  VALUES (%lld, last_insert_rowid(), 0, 'Call the vendor back');",
                (long long)id);
            sqlite3_exec(sq, sql, NULL, NULL, NULL);
            sqlite3_free(sql);

            tasks_editor_open(app, id);
            check_spin(0.2);
            NSWindow *editor_win = nil;
            for (NSWindow *w in NSApp.windows) {
                if (w.visible && [w.title isEqualToString:@"Call the vendor back"]) editor_win = w;
            }
            NSTextField *source = find_text(editor_win.contentView, @"From note:");
            check(source != nil && !source.hiddenOrHasHiddenAncestor &&
                  [source.stringValue containsString:@"Weekly sync"],
                  "editor: a Notes action item names the note it was written in");
            check(snapshot(editor_win, dir, @"editor-note-item"), "editor: snapshot, note item");
            tasks_editor_close(id);
            check_spin(0.1);

            gint64 plain = first_task_ids(app, 1).firstObject.longLongValue;
            tasks_editor_open(app, plain);
            check_spin(0.2);
            BOOL shown = NO;
            for (NSWindow *w in NSApp.windows) {
                NSTextField *line = w.visible ? find_text(w.contentView, @"From note:") : nil;
                shown = shown || (line != nil && !line.hiddenOrHasHiddenAncestor);
            }
            check(!shown, "editor: a task not born in Notes has no such line");
            tasks_editor_close(plain);
            check_spin(0.1);

            sql = sqlite3_mprintf("DELETE FROM note_task WHERE task_id = %lld;"
                                  "DELETE FROM notes WHERE title = 'Weekly sync';",
                                  (long long)id);
            sqlite3_exec(sq, sql, NULL, NULL, NULL);
            sqlite3_free(sql);
        }
        task_ptr_array_free_lists(lists);
    }

    tasks_editor_close_all();
    check_spin(0.1);
}


/*
 * policy — one backup-policy value as the shared module applies it
 * (shared_backup_policy_get), as the Settings window shows it.
 *
 * Inputs:
 *   app — the application context
 *   key — a SHARED_SETTING_BACKUP_* integer key
 *
 * Output:
 *   the value.
 */
static int64_t
policy(TaskApp *app, const char *key)
{
    int64_t value = 0;
    shared_backup_policy_get(app->db->shared, key, &value, NULL);
    return value;
}

/*
 * check_settings — drive the Settings window through its own controls:
 * every Appearance box writes its key, Update records a verdict from the
 * background, a number is held to its range, a backup folder the core
 * refuses (a stand-in live data directory under dev/) is not kept,
 * Back Up Now writes a backup into a folder under dev/, and a window
 * closed with a probe in flight is released.  Every setting is put back.
 * The "copy" links are not pressed: they would replace the clipboard of
 * whoever is running this.
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder; the scratch folders are made beside it
 */
static void
check_settings(TaskApp *app, NSString *dir)
{
    NSFileManager *files   = NSFileManager.defaultManager;
    NSString      *parent  = dir.stringByDeletingLastPathComponent;
    NSString      *backups = [parent stringByAppendingPathComponent:@"ui-check-backups"];
    NSString      *xdg     = [parent stringByAppendingPathComponent:@"ui-check-xdg"];
    NSString      *live    = [xdg stringByAppendingPathComponent:@"tasks"];
    __weak NSWindow *weak_window = nil;

    /* The backup policy is the shared database's (backup.h).  It starts
     * switched off here, in the folder the seed chose (beside the sandbox
     * database), whatever an earlier run left, and is put back at the end. */
    int64_t was_enabled = policy(app, SHARED_SETTING_BACKUP_ENABLED);
    char  *seeded_dir  = NULL;
    shared_settings_get(app->db->shared, SHARED_SETTING_BACKUP_DIR, &seeded_dir, NULL);
    shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_ENABLED, "0", NULL);

    [files createDirectoryAtPath:backups withIntermediateDirectories:YES attributes:nil error:NULL];
    [files createDirectoryAtPath:live withIntermediateDirectories:YES attributes:nil error:NULL];

    @autoreleasepool {
        tasks_settings_window_open(app);
        check_spin(0.2);
        NSWindow *window = nil;
        for (NSWindow *open in NSApp.windows) {
            if (open.visible && [open.title isEqualToString:@"Settings"]) window = open;
        }
        weak_window = window;

        /* The first view, breadth first, that a test accepts. */
        NSView *(^find)(BOOL (^)(NSView *)) = ^NSView *(BOOL (^wanted)(NSView *)) {
            NSMutableArray<NSView *> *todo = [NSMutableArray arrayWithObject:window.contentView];
            while (todo.count > 0) {
                NSView *view = todo.firstObject;
                [todo removeObjectAtIndex:0];
                if (wanted(view)) return view;
                [todo addObjectsFromArray:view.subviews];
            }
            return nil;
        };
        NSButton *(^button)(NSString *) = ^NSButton *(NSString *title) {
            return find_button(window.contentView, title);
        };
        NSTextField *(^label)(NSString *) = ^NSTextField *(NSString *prefix) {
            return (NSTextField *)find(^BOOL(NSView *view) {
                return [view isKindOfClass:NSTextField.class] && !view.hiddenOrHasHiddenAncestor &&
                       [((NSTextField *)view).stringValue hasPrefix:prefix];
            });
        };

        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        check(snapshot(window, dir, @"settings-dark"), "settings: snapshot, dark");
        window.appearance = nil;

        /* Appearance: each box writes its key at once, both ways. */
        NSDictionary<NSString *, NSString *> *boxes = @{
            @"Show task titles in bold":                         @TASK_CFG_BOLD_TITLES,
            @"Show drop shadows on Kanban cards":                @TASK_CFG_KANBAN_SHADOW,
            @"Count only business days (Mon\u2013Fri) in the 3-Day Forecast": @TASK_CFG_FORECAST_BUSINESS_DAYS,
        };
        BOOL boxes_write = YES;
        for (NSString *title in boxes) {
            const char *key    = boxes[title].UTF8String;
            gboolean    before = task_config_get_bool(key);
            [button(title) performClick:nil];
            boxes_write = boxes_write && task_config_get_bool(key) != before;
            [button(title) performClick:nil];
            boxes_write = boxes_write && task_config_get_bool(key) == before;
        }
        check(boxes_write, "settings: every Appearance box writes its setting, on and off");

        /* Update: the check runs in the background and lands on the handle. */
        int64_t pressed = (int64_t)time(NULL);
        [button(@"Update") performClick:nil];
        check(!button(@"Update").enabled && label(@"Checking") != nil,
              "settings: Update is busy while the check runs");
        for (int i = 0; i < 100 && !button(@"Update").enabled; i++) check_spin(0.05);
        const TaskDbHealth *health = task_db_health(app->db);
        check(health != NULL && health->ok && health->when >= pressed,
              "settings: Update records a fresh verdict on the database");
        check(label(@"Healthy — checked") != nil && button(@"Update").enabled,
              "settings: ... and the plate says so");

        /* Backups: the switch, then a number held to its range. */
        int64_t interval = policy(app, SHARED_SETTING_BACKUP_INTERVAL_MIN);
        [button(@"Back up the database automatically") performClick:nil];
        check(policy(app, SHARED_SETTING_BACKUP_ENABLED) == 1 &&
              button(@"Back Up Now").enabled,
              "settings: the backup switch writes the shared setting and wakes the controls");
        check(label(@"⚠ alongside the database") != nil,
              "settings: a backup folder beside the database is flagged as such");

        NSTextField *interval_field = (NSTextField *)find(^BOOL(NSView *view) {
            return [view isKindOfClass:NSTextField.class] && ((NSTextField *)view).editable;
        });
        interval_field.stringValue = @"99999";
        [interval_field sendAction:interval_field.action to:interval_field.target];
        check(policy(app, SHARED_SETTING_BACKUP_INTERVAL_MIN) == SHARED_BACKUP_INTERVAL_MIN_MAX &&
              interval_field.integerValue == SHARED_BACKUP_INTERVAL_MIN_MAX,
              "settings: a typed interval is held to its range and written");
        interval_field.integerValue = (NSInteger)interval;
        [interval_field sendAction:interval_field.action to:interval_field.target];

        /* A folder the core refuses is not kept, and the refusal is shown.
         * The stand-in live directory is under dev/: XDG_DATA_HOME moves
         * what the fence calls live data for as long as it is set. */
        SEL use_folder = NSSelectorFromString(@"use_backup_folder:");
        setenv("XDG_DATA_HOME", xdg.fileSystemRepresentation, 1);
        [NSApp sendAction:use_folder to:window.delegate from:live];
        check_spin(0.3);
        char *kept = NULL;
        shared_settings_get(app->db->shared, SHARED_SETTING_BACKUP_DIR, &kept, NULL);
        check(g_strcmp0(kept, seeded_dir) == 0 && window.attachedSheet != nil,
              "settings: a backup folder inside live data is refused with a notice, not kept");
        free(kept);
        if (window.attachedSheet) [window endSheet:window.attachedSheet];
        check_spin(0.3);
        unsetenv("XDG_DATA_HOME");

        /* A folder away from the database: kept, no warning, backed up to. */
        [NSApp sendAction:use_folder to:window.delegate from:backups];
        check(label([@"Backing up to " stringByAppendingString:backups]) != nil &&
              label(@"⚠ alongside the database") == nil,
              "settings: a folder away from the database is used, without the warning");
        NSPredicate *named = [NSPredicate predicateWithFormat:@"SELF BEGINSWITH %@",
                              @SHARED_DB_BACKUP_PREFIX];
        [button(@"Back Up Now") performClick:nil];
        for (int i = 0; i < 100 && label(@"Backed up to") == nil; i++) check_spin(0.05);
        NSArray<NSString *> *made = [[files contentsOfDirectoryAtPath:backups error:NULL]
            filteredArrayUsingPredicate:named];
        check(label(@"Backed up to") != nil && made.count == 1,
              "settings: Back Up Now writes one backup there through the shared module");
        [button(@"Back Up Now") performClick:nil];
        for (int i = 0; i < 100 && [[files contentsOfDirectoryAtPath:backups error:NULL]
                                        filteredArrayUsingPredicate:named].count < 2; i++) {
            check_spin(0.05);
        }
        check_spin(0.2);
        check([[files contentsOfDirectoryAtPath:backups error:NULL]
                  filteredArrayUsingPredicate:named].count == 2 && !app->backup_running,
              "settings: Back Up Now again makes a second, new backup");
        check(snapshot(window, dir, @"settings-backups"), "settings: snapshot, backups on");

        /* The last failure the module recorded is shown, and a backup that
         * then succeeds (the module removes the record) takes it away. */
        SEL refresh = NSSelectorFromString(@"refresh_backup");
        shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_LAST_ERROR,
                            "the disk was full", NULL);
        [NSApp sendAction:refresh to:window.delegate from:nil];
        check(label(@"Last backup failed: the disk was full") != nil,
              "settings: the last backup's failure (backup_last_error) is shown");
        [button(@"Back Up Now") performClick:nil];
        for (int i = 0; i < 100 && (app->backup_running ||
                                    label(@"Last backup failed") != nil); i++) {
            check_spin(0.05);
        }
        check(label(@"Last backup failed") == nil,
              "settings: ... until a backup succeeds");

        /* Put the settings back, then close with a probe still in flight. */
        shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_DIR, seeded_dir, NULL);
        [button(@"Back up the database automatically") performClick:nil];
        check(policy(app, SHARED_SETTING_BACKUP_ENABLED) == 0,
              "settings: (backups off again)");
        shared_settings_set(app->db->shared, SHARED_SETTING_BACKUP_ENABLED,
                            was_enabled ? "1" : "0", NULL);
        [button(@"Update") performClick:nil];
        [window close];
    }
    check_spin(0.5);
    check(weak_window == nil, "settings: a window closed during a check is released");
    [files removeItemAtPath:backups error:NULL];
    [files removeItemAtPath:xdg error:NULL];
    free(seeded_dir);
}


/*
 * day_text_fields — every text field under a view.
 *
 * Inputs:
 *   root — the view
 *   out  — receives the fields
 */
static void
day_text_fields(NSView *root, NSMutableArray<NSTextField *> *out)
{
    if ([root isKindOfClass:NSTextField.class]) [out addObject:(NSTextField *)root];
    for (NSView *v in root.subviews) day_text_fields(v, out);
}

/*
 * check_day_planner_calendar — the planner with Google Calendar events
 * from the sync's cache: two overlapping meetings and an all-day event
 * beside two sessions, one of them overlapping a meeting.  The sandbox's
 * schedule, cache and switch are put back afterwards.
 *
 * Inputs:
 *   app — the application context
 *   dir — the snapshot folder
 */
static void
check_day_planner_calendar(TaskApp *app, NSString *dir)
{
    NSArray<NSNumber *> *ids = first_task_ids(app, 2);
    if (ids.count < 2) {
        check(NO, "planner: the sandbox has two tasks to schedule");
        return;
    }
    gint64 a = ids[0].longLongValue, b = ids[1].longLongValue;
    Task  *ta = task_db_task_get(app->db, a), *tb = task_db_task_get(app->db, b);
    gint64 a_start = ta->scheduled_start, b_start = tb->scheduled_start;
    gint   a_dur   = ta->scheduled_duration, b_dur = tb->scheduled_duration;
    task_free(ta);
    task_free(tb);

    /* The meetings sit around the current hour, which is where the planner
     * opens on today (clamped to the hours it draws). */
    gint64 lo, hi;
    task_day_bounds(0, &lo, &hi);
    gint64 base = lo + (gint64)CLAMP((time(NULL) - lo) / 3600, tasks_look_day_planner_start_h + 1,
                                     tasks_look_day_planner_end_h - 4) * 3600;
    GPtrArray      *cals = g_ptr_array_new();
    gcal_calendar_t work = { "work@ui-check", "Work", "#0b8043", TRUE, TRUE };
    g_ptr_array_add(cals, &work);
    gcal_store_set_calendars(app->db, cals);
    g_ptr_array_free(cals, TRUE);

    GPtrArray   *evs  = g_ptr_array_new();
    gcal_event_t sync = { NULL, "e1", "Team sync", NULL, base, base + 3600, FALSE };
    gcal_event_t one  = { NULL, "e2", "1:1 with Sam", NULL, base + 1800,
                          base + 5400, FALSE };
    gcal_event_t off  = { NULL, "e3", "Company offsite", NULL, lo, hi, TRUE };
    g_ptr_array_add(evs, &sync);
    g_ptr_array_add(evs, &one);
    g_ptr_array_add(evs, &off);
    gcal_store_set_events(app->db, "work@ui-check", evs);
    g_ptr_array_free(evs, TRUE);

    task_db_task_set_scheduled(app->db, a, base + 1800, 60);
    task_db_task_set_scheduled(app->db, b, base + 2 * 3600, 90);
    gboolean was_on = task_config_get_bool(TASK_CFG_GCAL_ENABLED);
    task_config_set_bool(TASK_CFG_GCAL_ENABLED, TRUE);

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow *window = library.window;
        [library showWindow:nil];
        BOOL was_hidden = library.day_planner.hidden;
        if (was_hidden) {
            [library perform_command:TasksCommandToggleDayPlanner param:nil];
            check_spin(0.5);
        }
        TasksDayPlanner *planner = library.day_planner;
        [planner reload];
        [planner scroll_to_now];

        NSUInteger n_events = 0, n_tasks = 0, side_by_side = 0;
        for (id item in planner.items) {
            if ([[item valueForKey:@"is_event"] boolValue]) n_events++;
            else                                            n_tasks++;
            if ([[item valueForKey:@"columns"] unsignedIntegerValue] > 1) side_by_side++;
        }
        check(n_events == 2 && n_tasks == 2, "planner: timed events drawn beside the sessions");
        check(side_by_side == 3, "planner: the two meetings and the session they overlap share the width");

        NSMutableArray<NSTextField *> *fields = [NSMutableArray array];
        day_text_fields(planner, fields);
        BOOL strip = NO;
        for (NSTextField *f in fields) {
            if (!f.hidden && [f.stringValue isEqualToString:@"Company offsite"]) strip = YES;
        }
        check(strip, "planner: the all-day event is in the strip under the date");

        check(snapshot(window, dir, @"day-planner-calendar"), "snapshot: planner with calendar events");
        window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        check(snapshot(window, dir, @"day-planner-calendar-dark"),
              "snapshot: planner with calendar events, dark");
        window.appearance = nil;

        task_config_set_bool(TASK_CFG_GCAL_ENABLED, FALSE);
        [planner reload];
        n_events = 0;
        for (id item in planner.items) {
            if ([[item valueForKey:@"is_event"] boolValue]) n_events++;
        }
        check(n_events == 0, "planner: switched off, no events are drawn");

        if (was_hidden) {
            [library perform_command:TasksCommandToggleDayPlanner param:nil];
            check_spin(0.5);
        }
        [window close];
        check_spin(0.2);
    }

    task_db_task_set_scheduled(app->db, a, a_start, a_dur);
    task_db_task_set_scheduled(app->db, b, b_start, b_dur);
    GPtrArray *none = g_ptr_array_new();
    gcal_store_keep_events(app->db, none);
    gcal_store_set_calendars(app->db, none);
    g_ptr_array_free(none, TRUE);
    task_config_set_bool(TASK_CFG_GCAL_ENABLED, was_on);
}

/*
 * main — run the checks.
 *
 * Inputs:
 *   argc — argument count
 *   argv — argv[1] is the snapshot folder; argv[2], optional, is how many
 *          seconds to stay alive afterwards
 *
 * Output:
 *   0 when every check passed, 1 when one failed, 2 when it could not run.
 */
int
main(int argc, const char *argv[])
{
    @autoreleasepool {
        const char *sandbox = getenv(TASK_DATA_DIR_ENV);
        const char *shared  = getenv(SHARED_DB_ENV_VAR);
        if (argc < 2 || sandbox == NULL || sandbox[0] != '/' ||
            !check_inside(shared, sandbox)) {
            fprintf(stderr, "ui_check: run `make ui-check` (it sets %s and %s)\n",
                    TASK_DATA_DIR_ENV, SHARED_DB_ENV_VAR);
            return 2;
        }
        NSString *dir = @(argv[1]);
        [NSFileManager.defaultManager createDirectoryAtPath:dir
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:NULL];

        /* An accessory app: windows work, but no Dock icon appears and the
         * user's front app keeps the keyboard. */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
        /* Snapshots are light whatever the system is set to; the dark one
         * overrides its own window. */
        NSApp.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];

        if (!task_config_init()) {
            fprintf(stderr, "ui_check: cannot use %s as the data directory\n", sandbox);
            return 2;
        }
        task_core_views_init();
        TaskApp *app  = task_app_new();
        char    *path = shared_db_path(NULL);
        app->db = path != NULL && g_file_test(path, G_FILE_TEST_EXISTS)
            ? task_db_open(path, NULL) : NULL;
        free(path);
        if (app->db == NULL) {
            fprintf(stderr, "ui_check: no sandbox database; run `make seed` first\n");
            return 2;
        }

        /* One call per group of checks; each builds and closes its own
         * windows, so a package can add its own here without touching the
         * others. */
        snapshot_windows(app, dir);
        check_utilities(app);
        check_row_text(app);
        check_compact(app, dir);
        check_status_message(app);
        check_open_database(app);
        check_menu_bar();
        check_lists_bar(app, dir);
        check_task_list(app, dir);
        check_kanban(app, dir);
        check_editor_window(app, dir);
        check_settings(app, dir);
        check_day_planner_calendar(app, dir);
        check_memory_and_ownership(app);

        if (argc > 2) {
            printf("     holding for %s s as pid %d\n", argv[2], getpid());
            fflush(stdout);
            check_spin(atof(argv[2]));
        }

        task_app_free(app);
        task_config_close();
        return check_result();
    }
}
