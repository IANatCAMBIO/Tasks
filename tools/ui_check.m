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
 *     dark, list and board, an editor, Settings);
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

#import "editor_window.h"
#import "library_window.h"
#import "settings_window.h"
#import "ui_util.h"

#include "check.h"

#include "core_views.h"
#include "task_config.h"
#include "task_order.h"
#include "task_paths.h"

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <malloc/malloc.h>

/* How many cycles each half of the growth check runs. */
#define CYCLES 20

/* Heap growth allowed per cycle across the second run (see "ABOUT THE HEAP
 * BUDGET" above).  A cycle is 4 windows and about 6 pane refreshes; the
 * skeleton measures about 8 KB. */
#define HEAP_BUDGET_PER_CYCLE (16 * 1024)

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
    NSImage *icon = tasks_icon(@"add", 24.0);
    NSBitmapImageRep *rep = (NSBitmapImageRep *)icon.representations.firstObject;
    check(icon != nil && NSEqualSizes(icon.size, NSMakeSize(24, 24)),
          "utilities: a bundled icon comes back at its drawing size");
    check(icon.representations.count == 1 && rep.pixelsWide == 48,
          "utilities: ... holding one small bitmap, not the 512 px source");
    check(tasks_icon(@"add", 24.0) == icon, "utilities: icons are cached");
    check(tasks_icon(@"no-such-icon", 24.0) == nil, "utilities: a missing icon is nil");

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

        snapshot_windows(app, dir);
        check_utilities(app);
        check_memory_and_ownership(app);
        check_kanban(app, dir);

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
