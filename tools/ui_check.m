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
#import "editor_window.h"
#import "library_window.h"
#import "look.h"
#import "row_text.h"
#import "settings_window.h"
#import "sidebar.h"
#import "ui_util.h"

#include "check.h"

#include "core_views.h"
#include "task_config.h"
#include "task_paths.h"

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>

/* How many cycles each half of the growth check runs. */
#define CYCLES 20

/* Heap growth allowed per cycle across the second run (see "ABOUT THE HEAP
 * BUDGET" above).  A cycle is 4 windows and about 6 pane refreshes.  The
 * skeleton measured about 8 KB; a cycle against real task rows, kanban
 * cards and attributed-string caches measures about 20 KB.  Budget is set
 * at 24 KB: above the measured baseline, below a genuine leak. */
#define HEAP_BUDGET_PER_CYCLE (24 * 1024)

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
        check(find_view(content, TasksSidebar.class).hiddenOrHasHiddenAncestor,
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
        check(find_view(content, TasksSidebar.class).hiddenOrHasHiddenAncestor,
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
              find_view(content, TasksSidebar.class).hiddenOrHasHiddenAncestor &&
              !find_view(content, plate_class).hiddenOrHasHiddenAncestor,
              "compact: a launch in Compact opens Compact, at that size");

        [library perform_command:TasksCommandToggleCompact param:nil];
        check_spin(SLIDE_SECONDS);

        check(!task_config_get_bool(TASK_CFG_COMPACT) && window.toolbar.visible &&
              find_view(content, plate_class).hidden,
              "compact: leaving shows the toolbar and hides the floating controls");
        check(!find_view(content, TasksSidebar.class).hiddenOrHasHiddenAncestor &&
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
 * onwards: a switch, the default question, the refusals, and back.  The
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
    NSString      *home    = tasks_string(app->db->path);
    NSString      *folder  = [sandbox stringByAppendingPathComponent:@"ui-check"];
    NSString      *other   = [folder stringByAppendingPathComponent:@TASK_DB_FILENAME];
    NSString      *junk    = [folder stringByAppendingPathComponent:@"junk.db"];
    NSString      *live    = [folder stringByAppendingPathComponent:@"xdg/tasks/tasks.db"];

    /* A second database holding one task; a file that is not a database. */
    [files removeItemAtPath:folder error:NULL];
    [files createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:NULL];
    TaskDatabase *db = task_db_open(other.fileSystemRepresentation, NULL);
    task_db_task_create(db, task_db_list_create(db, "Elsewhere", ""), 0, "The only task");
    task_db_close(db);
    [@"not a database" writeToFile:junk atomically:YES encoding:NSUTF8StringEncoding error:NULL];

    @autoreleasepool {
        TasksLibraryWindow *library = [[TasksLibraryWindow alloc] init_with_app:app];
        NSWindow *window = library.window;
        [library showWindow:nil];
        tasks_editor_open(app, first_task_ids(app, 1).firstObject.longLongValue);
        check_spin(0.2);

        [library open_database:junk];
        check_spin(0.3);
        check(window.attachedSheet != nil && [tasks_string(app->db->path) isEqualToString:home],
              "open database: a file that is not a database is refused with an alert");
        check(secondary_windows(window) == 1, "open database: ... and the editors stay open");
        end_sheet(window, NSAlertFirstButtonReturn);

        setenv("XDG_DATA_HOME", [folder stringByAppendingPathComponent:@"xdg"].fileSystemRepresentation, 1);
        [library open_database:live];
        check_spin(0.3);
        check(find_text(window.attachedSheet.contentView, @"refused") != nil &&
              [tasks_string(app->db->path) isEqualToString:home],
              "open database: a path in the live data directory shows the core's refusal");
        end_sheet(window, NSAlertFirstButtonReturn);
        unsetenv("XDG_DATA_HOME");

        [library open_database:other];
        check_spin(0.3);
        check([tasks_string(app->db->path) isEqualToString:other] && secondary_windows(window) == 0,
              "open database: a sound database is switched to, the editors closed first");
        check(find_text(window.contentView, @"All Tasks - 1 task") != nil,
              "open database: ... and the panes show its tasks");
        check(find_text(window.attachedSheet.contentView, @"default database") != nil,
              "open database: ... and a tasks.db is offered as the default");
        end_sheet(window, NSAlertSecondButtonReturn);
        gchar *db_dir = task_config_get(TASK_CFG_DB_DIR);
        check(db_dir == NULL, "open database: declining leaves the default alone");
        g_free(db_dir);

        [library open_database:other];
        check_spin(0.3);
        check(window.attachedSheet == nil && find_text(window.contentView, @"already open") != nil,
              "open database: the open database again only says so");

        [library open_database:home];
        check_spin(0.3);
        check([tasks_string(app->db->path) isEqualToString:home] && window.attachedSheet == nil,
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
        if (argc < 2 || sandbox == NULL || sandbox[0] == '\0') {
            fprintf(stderr, "ui_check: run `make ui-check` (it sets %s)\n", TASK_DATA_DIR_ENV);
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
        gchar   *path = task_db_default_path();
        app->db = g_file_test(path, G_FILE_TEST_EXISTS) ? task_db_open(path, NULL) : NULL;
        g_free(path);
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
