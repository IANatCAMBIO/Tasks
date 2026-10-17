#import "library_window.h"
#import "day_planner.h"
#import "editor_window.h"
#import "kanban.h"
#import "look.h"
#import "favorites_bar.h"
#import "notify_window.h"
#import "settings_window.h"
#import "sidebar.h"
#import "task_list.h"
#import "ui_util.h"

#include "task_config.h"
#include "task_ops.h"
#include "task_order.h"
#include "task_switch.h"
#include "task_view.h"

/*
 * The shell: the window's own furniture (toolbar, search field, status
 * bar, Compact mode's floating buttons), the shared state and its single
 * writers, the refresh orchestration, command dispatch and validation, the
 * app-event listeners and the teardown order.  The contract with the panes
 * is in library_window.h.
 */

/* The toolbar's own identifiers (command items use the command table's). */
static NSString * const toolbar_ident        = @"tasks.library";
static NSString * const search_item_ident    = @"search";   /* as Notes names its */

/* The search field's words.  The placeholder says what is searched: the box
 * narrows the SELECTED view, it does not sweep the database. */
static NSString * const search_placeholder   = @"Search this view";
static NSString * const search_tooltip       =
    @"Search the selected view's task titles, notes and subtasks.\n"
    @"\"quoted words\" matches the phrase; -word leaves out tasks containing it.";

@interface TasksLibraryWindow () <NSWindowDelegate, NSToolbarDelegate, NSSearchFieldDelegate>
@property (nonatomic, strong) TasksSidebar      *sidebar;
@property (nonatomic, strong) NSSplitViewController *split;      /* sidebar | pane | planner */
@property (nonatomic, strong) NSSplitViewItem   *sidebar_item;
@property (nonatomic, strong) NSSplitViewItem   *planner_item;
@property (nonatomic, strong) TasksFavoritesBar *favorites_bar;
@property (nonatomic, strong) NSLayoutConstraint *favorites_height; /* 0 while hidden */
@property (nonatomic, strong) TasksTaskList *task_list;
@property (nonatomic, strong) TasksKanban *kanban;
@property (nonatomic, strong) TasksDayPlanner *day_planner;
@property (nonatomic, strong) NSTextField *status_left;    /* "<view>" or "#<tag>"      */
@property (nonatomic, strong) NSTextField *status_right;   /* the latest event message  */
@property (nonatomic, weak) NSSearchField *search_field;   /* owned by its toolbar item */
/* Compact mode's floating controls: the plate, and the command buttons on
 * it (kept so that their state can be refreshed; see windowDidUpdate:). */
@property (nonatomic, strong) NSView *float_bar;
@property (nonatomic, copy) NSArray<NSButton *> *float_buttons;
/* YES while a Compact toggle's slide and resize are running; a second
 * toggle is ignored until they finish. */
@property (nonatomic, assign) BOOL compact_animating;
/* Bumped by every status message, so only the latest one's timer fades it. */
@property (nonatomic, assign) NSUInteger status_message_id;
/* Timer that fires after shared_look_status_hold to begin the fade; at most
 * one at a time (invalidated when the next message arrives). */
@property (nonatomic, strong) NSTimer *status_timer;
/* Subscriptions to the app's four events; 0 once dropped. */
@property (nonatomic, assign) guint listen_changed;
@property (nonatomic, assign) guint listen_tasks;
@property (nonatomic, assign) guint listen_notes;
@property (nonatomic, assign) guint listen_status;
/* The tags as the sidebar and the Favorites bar last showed them
 * (tag_rows): the light task event rebuilds them only when that changes. */
@property (nonatomic, copy)   NSArray<NSArray *> *shown_tags;
@property (nonatomic, assign) BOOL is_shut_down;

/* Declared here because the C listeners below call it before its
 * definition; documented where it is defined. */
- (void)show_status_message:(NSString *)message;
- (void)update_window_title;
- (NSSize)content_min_size;
@end

/* ── App-event listeners (C callbacks; user_data is the window) ─────── */

/*
 * tag_rows — every tag a task carries, as the sidebar and the Favorites
 * bar show it: its id, its emoji and its favorite flag, alphabetically.
 * What decides
 * whether a task change (a tag first used, or used for the last time) or
 * an Edit Tag needs them rebuilt.
 *
 * Inputs:
 *   app — the application context
 *
 * Output:
 *   one @[@(id), emoji, @(favorite)] per tag.
 */
static NSArray<NSArray *> *
tag_rows(TaskApp *app)
{
    GPtrArray                *tags = task_db_tags(app->db, TASK_TAGS_CARRIED);
    NSMutableArray<NSArray *> *rows = [NSMutableArray arrayWithCapacity:tags->len];

    for (guint i = 0; i < tags->len; i++) {
        const TaskTag *tag = g_ptr_array_index(tags, i);
        [rows addObject:@[@(tag->id), tasks_string(tag->emoji), @(tag->favorite)]];
    }
    task_ptr_array_free_tags(tags);
    return rows;
}

/*
 * on_app_changed — the app's FULL refresh event.
 *
 * Inputs:
 *   app  — the application context (unused)
 *   data — the TasksLibraryWindow, unretained; valid because the window
 *          unsubscribes in shut_down before it can go away
 */
static void
on_app_changed(TaskApp *app, gpointer data)
{
    [(__bridge TasksLibraryWindow *)data full_refresh];
}

/*
 * on_app_tasks — the app's light event: the task pane, the day planner and
 * the reminders' titles.  One exception: a task change can add or remove
 * a tag (the first task given a new one, the last one losing it), so the
 * sidebar and the Favorites bar are rebuilt when the tags changed; else
 * only the sidebar's counts are re-read.
 *
 * Inputs:
 *   app  — the application context
 *   data — the TasksLibraryWindow, unretained
 */
static void
on_app_tasks(TaskApp *app, gpointer data)
{
    TasksLibraryWindow *window = (__bridge TasksLibraryWindow *)data;

    if (![tag_rows(app) isEqualToArray:window.shown_tags]) {
        [window refresh_sidebar];
    } else {
        [window.sidebar refresh_counts];
    }
    [window refresh_tasks];
    if (window.day_planner_shown) [window.day_planner reload];
    tasks_notify_retitle();
}

/*
 * on_app_notes — the app's notes event: Notes' data changed elsewhere, so
 * the open editors' "From note" lines are re-read.
 *
 * Inputs:
 *   app  — the application context (unused)
 *   data — the TasksLibraryWindow, unretained (unused)
 */
static void
on_app_notes(TaskApp *app, gpointer data)
{
    (void)app;
    (void)data;
    tasks_editor_refresh_sources();
}

/*
 * on_app_status — a one-line event message for the status bar.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   message — the message (borrowed)
 *   data    — the TasksLibraryWindow, unretained
 */
static void
on_app_status(TaskApp *app, const gchar *message, gpointer data)
{
    [(__bridge TasksLibraryWindow *)data show_status_message:tasks_string(message)];
}

/* ── Compact mode's plate ────────────────────────────────────────────── */

/*
 * TasksFloatPlate — the plate behind Compact mode's floating buttons: a
 * rounded rectangle in the window's background colour with a hairline
 * border, so the buttons read as controls and not as part of the rows they
 * sit over.  It is drawn here rather than given to a layer because both
 * colours are dynamic: drawRect: runs again when the appearance changes,
 * where a layer's colours would stay as they were set.
 */
@interface TasksFloatPlate : NSView
@end

@implementation TasksFloatPlate

/*
 * drawRect: — fill and outline the plate.
 *
 * Inputs:
 *   dirty — the area to redraw (unused: the plate is small and drawn whole)
 */
- (void)
drawRect:(NSRect)dirty
{
    /* Half a point in, so the 1 pt outline falls on whole pixels. */
    NSBezierPath *plate =
        [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 0.5, 0.5)
                                        xRadius:tasks_look_float_radius
                                        yRadius:tasks_look_float_radius];

    [NSColor.windowBackgroundColor setFill];
    [plate fill];
    [NSColor.separatorColor setStroke];
    [plate stroke];
}

@end

/* ── Saved sizes ─────────────────────────────────────────────────────── */

/*
 * saved_size — a window content size kept in the config as a width key and
 * a height key (each has a default in the config's registry).
 *
 * Inputs:
 *   w_key — the width's key
 *   h_key — the height's key
 *
 * Output:
 *   the size in points.
 */
static NSSize
saved_size(const char *w_key, const char *h_key)
{
    return NSMakeSize(task_config_get_int(w_key), task_config_get_int(h_key));
}

/*
 * save_size — remember a window content size under a width key and a
 * height key.
 *
 * Inputs:
 *   w_key — the width's key
 *   h_key — the height's key
 *   size  — the size in points
 */
static void
save_size(const char *w_key, const char *h_key, NSSize size)
{
    task_config_set_int(w_key, (gint)size.width);
    task_config_set_int(h_key, (gint)size.height);
}

/* One handler table a command may be in, with the object its handlers
 * take as their target: the window's own (shell_commands, at the end of
 * this file), the sidebar's and the task list's. */
typedef struct {
    const shared_command_handler_t *table;
    size_t                          n;
    __unsafe_unretained id          target;     /* owned by the window */
} command_table_t;
#define COMMAND_TABLE_COUNT 3

static const shared_command_handler_t *shell_commands(size_t *n);

@implementation TasksLibraryWindow {
    command_table_t _command_tables[COMMAND_TABLE_COUNT];
}

/* ── Construction ────────────────────────────────────────────────────── */

/*
 * init_with_app: — build the window and show the default selection.
 * Inputs/output are documented in library_window.h.
 *
 * The window opens at the size it had when it last closed IN THE MODE IT
 * OPENS IN: a Compact window comes back at the Compact size.
 */
- (instancetype)
init_with_app:(TaskApp *)app
{
    NSSize size = task_config_get_bool(TASK_CFG_COMPACT)
        ? saved_size(TASK_CFG_COMPACT_W, TASK_CFG_COMPACT_H)
        : saved_size(TASK_CFG_WIN_W, TASK_CFG_WIN_H);
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _app         = app;
    _selection   = task_sel_default();
    _kanban_on   = task_config_get_bool(TASK_CFG_KANBAN_VIEW);

    window.title        = @TASKS_APP_NAME;
    window.toolbarStyle = NSWindowToolbarStyleExpanded;
    window.delegate     = self;
    [self build_layout];
    [self build_toolbar];
    /* The size first, then the place: placing keeps the window on a
     * screen, which depends on how big it is. */
    [self apply_compact_layout_at:size animated:NO];
    [self place_window];

    self.listen_changed = task_app_listen_changed(app, on_app_changed, (__bridge void *)self);
    self.listen_tasks   = task_app_listen_tasks(app, on_app_tasks, (__bridge void *)self);
    self.listen_notes   = task_app_listen_notes(app, on_app_notes, (__bridge void *)self);
    self.listen_status  = task_app_listen_status(app, on_app_status, (__bridge void *)self);

    [self apply_pane_mode];
    [self refresh_sidebar];
    [self update_window_title];
    [self refresh_tasks];

    if (self.day_planner_shown) {
        [self.day_planner reload];
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.day_planner scroll_to_now];
        });
    }

    /* The keyboard starts in the task pane, not in the search field. */
    window.initialFirstResponder = (NSView *)[self visible_pane];
    return self;
}

/*
 * dealloc — nothing to release here; teardown is shut_down's job and must
 * already have happened.  The app's listener lists hold an unretained
 * pointer to this object, and by now the TaskApp may be gone, so a window
 * that reaches dealloc without shut_down is a programming error worth
 * stopping on.
 */
- (void)
dealloc
{
    NSAssert(_is_shut_down, @"TasksLibraryWindow released without shut_down");
}

/*
 * place_window — put the window where it was last time (win_x / win_y in
 * the config), kept on a screen; centred on first run.
 */
- (void)
place_window
{
    gchar *x = task_config_get(TASK_CFG_WIN_X);
    gchar *y = task_config_get(TASK_CFG_WIN_Y);

    if (x != NULL && y != NULL) {
        NSRect frame = self.window.frame;
        frame.origin = NSMakePoint(atoi(x), atoi(y));
        [self.window setFrame:[self.window constrainFrameRect:frame toScreen:self.window.screen]
                      display:NO];
    } else {
        [self.window center];
    }
    g_free(x);
    g_free(y);
}

/*
 * build_float_bar: — Compact mode's floating controls: New Task and Delete
 * Task on a plate, tasks_look_float_margin in from the bottom-right corner
 * of the task pane and above both of its presentations.  Hidden until
 * apply_compact_layout_at:animated: shows it.
 *
 * Inputs:
 *   pane — the task pane's container, the list and the board already in it
 */
- (void)
build_float_bar:(NSView *)pane
{
    self.float_buttons = @[ shared_command_button(@"new-task"),
                            shared_command_button(@"delete-task") ];

    NSStackView *buttons = [NSStackView stackViewWithViews:self.float_buttons];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing     = tasks_look_float_pad;
    buttons.edgeInsets  = NSEdgeInsetsMake(tasks_look_float_pad, tasks_look_float_pad,
                                           tasks_look_float_pad, tasks_look_float_pad);

    /* The plate takes its size from the buttons pinned inside it. */
    self.float_bar = [[TasksFloatPlate alloc] initWithFrame:NSZeroRect];
    self.float_bar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.float_bar addSubview:buttons];
    buttons.translatesAutoresizingMaskIntoConstraints = NO;
    shared_ui_pin_edges(buttons, self.float_bar);

    [pane addSubview:self.float_bar positioned:NSWindowAbove relativeTo:nil];
    [NSLayoutConstraint activateConstraints:@[
        [self.float_bar.trailingAnchor constraintEqualToAnchor:pane.trailingAnchor
                                                      constant:-tasks_look_float_margin],
        [self.float_bar.bottomAnchor constraintEqualToAnchor:pane.bottomAnchor
                                                    constant:-tasks_look_float_margin],
    ]];
}

/*
 * build_layout — the window's content: the Favorites bar across the whole
 * width under the toolbar; below it one split view controller (as Notes'
 * library has) down to the status bar's separator: the tag sidebar at the
 * left, the task pane, and the day planner at the right.  The task pane
 * holds BOTH presentations, the list and the board, of which
 * apply_pane_mode shows one.  A hidden bar has no height; a hidden
 * sidebar or planner is a collapsed split item, so what is left fills the
 * room, and both slide the one way the split view controller slides.
 */
- (void)
build_layout
{
    NSView *content = self.window.contentView;

    self.sidebar       = [[TasksSidebar alloc] init_with_library:self];
    self.favorites_bar = [[TasksFavoritesBar alloc] init_with_library:self];
    self.task_list     = [[TasksTaskList alloc] init_with_library:self];
    _command_tables[0].table = shell_commands(&_command_tables[0].n);
    _command_tables[0].target = self;
    _command_tables[1].table = tasks_sidebar_commands(&_command_tables[1].n);
    _command_tables[1].target = self.sidebar;
    _command_tables[2].table = tasks_task_list_commands(&_command_tables[2].n);
    _command_tables[2].target = self.task_list;
    self.kanban        = [[TasksKanban alloc] init_with_library:self];

    NSView *pane = [[NSView alloc] initWithFrame:NSZeroRect];
    [pane addSubview:self.task_list];
    [pane addSubview:self.kanban];
    for (NSView *v in @[self.task_list, self.kanban]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        shared_ui_pin_edges(v, pane);
    }
    [self build_float_bar:pane];


    self.day_planner = [[TasksDayPlanner alloc] init_with_library:self];

    NSBox *rule   = [[NSBox alloc] initWithFrame:NSZeroRect];
    rule.boxType  = NSBoxSeparator;

    /* The sidebar, the task pane and the day planner in one split view
     * controller: the system's thin divider, drag and collapse slide for
     * both side panes; each holds its width, the task pane absorbs a
     * resize.  A side pane's width is saved as it is dragged. */
    SharedSplitController *outer = [[SharedSplitController alloc] init];
    __weak TasksLibraryWindow *ws = self;
    outer.width_changed = ^(NSInteger index, NSInteger width) {
        const char *key = index == 0 ? TASK_CFG_SIDEBAR_WIDTH : TASK_CFG_DAY_PLANNER_W;
        if (width == task_config_get_int(key)) return;
        task_config_set_int(key, (gint)width);
        ws.window.contentMinSize = [ws content_min_size];
    };
    self.split        = outer;
    self.sidebar_item = shared_ui_pane_item(self.sidebar, shared_look_sidebar_min_w);
    self.sidebar_item.canCollapse     = YES;
    self.sidebar_item.holdingPriority = NSLayoutPriorityDefaultLow + 1;
    self.planner_item = shared_ui_pane_item(self.day_planner, tasks_look_day_planner_min_w);
    self.planner_item.canCollapse     = YES;
    self.planner_item.holdingPriority = NSLayoutPriorityDefaultLow + 1;
    [self.split addSplitViewItem:self.sidebar_item];
    [self.split addSplitViewItem:shared_ui_pane_item(pane, tasks_look_pane_min_w)];
    [self.split addSplitViewItem:self.planner_item];
    self.split.splitView.dividerStyle = NSSplitViewDividerStyleThin;
    NSView *panes = self.split.view;

    for (NSView *v in @[self.favorites_bar, panes, rule]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:v];
    }
    /* The status band: from under the rule to the window's bottom edge,
     * its labels centred on their capitals (as Notes'). */
    NSLayoutGuide *band = [[NSLayoutGuide alloc] init];
    [content addLayoutGuide:band];
    self.status_left  = shared_ui_status_label(content, band.centerYAnchor, NSTextAlignmentCenter);
    self.status_right = shared_ui_status_label(content, band.centerYAnchor, NSTextAlignmentRight);
    self.favorites_height = [self.favorites_bar.heightAnchor
                                constraintEqualToConstant:shared_look_favorites_bar_h + 1.0];  /* + its rule */
    NSLayoutYAxisAnchor *below_bar = self.favorites_bar.bottomAnchor;
    [NSLayoutConstraint activateConstraints:@[
        [self.favorites_bar.topAnchor constraintEqualToAnchor:content.topAnchor],
        [self.favorites_bar.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [self.favorites_bar.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        self.favorites_height,

        [panes.topAnchor constraintEqualToAnchor:below_bar],
        [panes.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [panes.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [panes.bottomAnchor constraintEqualToAnchor:rule.topAnchor],

        [rule.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [rule.heightAnchor constraintEqualToConstant:1.0],
        [rule.bottomAnchor constraintEqualToAnchor:content.bottomAnchor
                                          constant:-shared_look_status_h],

        [self.status_left.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
        [self.status_left.leadingAnchor
            constraintGreaterThanOrEqualToAnchor:content.leadingAnchor
                                        constant:shared_look_status_inset],
        [self.status_left.trailingAnchor
            constraintLessThanOrEqualToAnchor:self.status_right.leadingAnchor
                                     constant:-shared_look_status_inset],
        [band.topAnchor constraintEqualToAnchor:rule.bottomAnchor],
        [band.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
        [self.status_right.trailingAnchor constraintEqualToAnchor:content.trailingAnchor
                                                         constant:-shared_look_status_inset],
    ]];

    /* The side panes' widths, set once the split view has its frame (a
     * position set before layout is clamped to a zero width); then each
     * collapsed if it was left hidden, so that showing it again brings
     * that width back. */
    [content layoutSubtreeIfNeeded];
    NSSplitView *split = self.split.splitView;
    [split setPosition:[self sidebar_extent] ofDividerAtIndex:0];
    [split setPosition:NSWidth(split.bounds) - [self planner_extent] - split.dividerThickness
      ofDividerAtIndex:1];
    self.sidebar_item.collapsed = !task_config_get_bool(TASK_CFG_SIDEBAR_VISIBLE);
    self.planner_item.collapsed = !task_config_get_bool(TASK_CFG_DAY_PLANNER_VISIBLE);
}

/* ── Toolbar ─────────────────────────────────────────────────────────── */

/*
 * build_toolbar — the window's toolbar: icon-only command buttons from the
 * command table, then the search field at the right.
 */
- (void)
build_toolbar
{
    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:toolbar_ident];

    toolbar.delegate                = self;
    toolbar.displayMode             = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO;
    self.window.toolbar = toolbar;
}

/*
 * toolbar_idents — the toolbar's items, left to right.
 *
 * Output:
 *   item identifiers.
 */
static NSArray<NSToolbarItemIdentifier> *
toolbar_idents(void)
{
    return @[
        @"toggle-sidebar",
        NSToolbarSpaceItemIdentifier,
        @"new-task",
        @"delete-task",
        NSToolbarSpaceItemIdentifier,
        @"toggle-completed",
        @"toggle-pane",
        @"toggle-day-planner",
        NSToolbarFlexibleSpaceItemIdentifier,
        search_item_ident,
    ];
}

/*
 * toolbarDefaultItemIdentifiers: — the items shown.
 *
 * Inputs:
 *   toolbar — the window's toolbar (unused)
 *
 * Output:
 *   toolbar_idents().
 */
- (NSArray<NSToolbarItemIdentifier> *)
toolbarDefaultItemIdentifiers:(NSToolbar *)toolbar
{
    return toolbar_idents();
}

/*
 * toolbarAllowedItemIdentifiers: — the same set: the toolbar is not
 * customisable.
 *
 * Inputs:
 *   toolbar — the window's toolbar (unused)
 *
 * Output:
 *   toolbar_idents().
 */
- (NSArray<NSToolbarItemIdentifier> *)
toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar
{
    return toolbar_idents();
}

/*
 * toolbar:itemForItemIdentifier:willBeInsertedIntoToolbar: — make one item:
 * the search field, or a command button from the command table.
 *
 * Inputs:
 *   toolbar — the window's toolbar (unused)
 *   ident   — which item
 *   flag    — whether it is about to be inserted (unused)
 *
 * Output:
 *   the item, or nil for an identifier that is not ours.
 */
- (NSToolbarItem *)
toolbar:(NSToolbar *)toolbar
itemForItemIdentifier:(NSToolbarItemIdentifier)ident
willBeInsertedIntoToolbar:(BOOL)flag
{
    if ([ident isEqualToString:search_item_ident]) {
        NSSearchToolbarItem *item = [[NSSearchToolbarItem alloc] initWithItemIdentifier:ident];
        NSSearchField *field      = item.searchField;

        field.placeholderString = search_placeholder;
        field.toolTip           = search_tooltip;
        field.target            = self;
        field.action            = @selector(search_changed:);
        field.delegate          = self;
        /* No focus ring on a toolbar field: a focused field is slightly
         * wider, which can push toolbar items into the overflow menu. */
        field.focusRingType     = NSFocusRingTypeNone;
        item.preferredWidthForSearchField = shared_look_toolbar_search_w;
        [field.widthAnchor constraintLessThanOrEqualToConstant:shared_look_toolbar_search_w].active = YES;
        self.search_field = field;
        return item;
    }

    return shared_command_find(ident) ? shared_command_toolbar_item(ident) : nil;
}

/*
 * validateUserInterfaceItem: — the answer also sets the item's title,
 * icon, tooltip and check mark, which is how a toggling control comes to
 * show the action it now offers.  An item carrying a parameter keeps the
 * title it was built with (shared_commands.h).
 * Inputs/output are documented in library_window.h.
 */
- (BOOL)
validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (self.is_shut_down) return NO;
    for (int i = 0; i < COMMAND_TABLE_COUNT; i++) {
        BOOL handled = NO;
        BOOL enabled = shared_command_validate(_command_tables[i].table, _command_tables[i].n,
                                               _command_tables[i].target, item, &handled);
        if (handled) return enabled;
    }
    return NO;
}

/* ── Search ──────────────────────────────────────────────────────────── */

/*
 * search_changed: — the search field's text changed (AppKit sends this once
 * typing pauses, at once on Return, and when the field is cleared):
 * re-parse the query and redraw the task pane through it.  Only the task
 * pane is rebuilt, never the sidebar.
 *
 * Inputs:
 *   sender — the search field
 */
- (void)
search_changed:(NSSearchField *)sender
{
    task_search_free((TaskSearch *)_search);
    _search = task_search_parse(sender.stringValue.UTF8String);
    [self refresh_tasks];
}

/*
 * clear_search — empty the search field and drop the filter, by the same
 * path as the user clearing the field: ONE place knows how a search ends.
 */
- (void)
clear_search
{
    self.search_field.stringValue = @"";
    [self search_changed:self.search_field];
}

/*
 * focus_task_pane — give the keyboard to the task pane on screen.
 */
- (void)
focus_task_pane
{
    [self.window makeFirstResponder:(NSView *)[self visible_pane]];
}

/*
 * control:textView:doCommandBySelector: — Escape in the search field clears
 * the search and gives the keyboard back to the task pane.
 *
 * Inputs:
 *   control  — the search field
 *   text     — its field editor (unused)
 *   selector — the editing command being performed
 *
 * Output:
 *   YES when the command was handled here.
 */
- (BOOL)
control:(NSControl *)control
textView:(NSTextView *)text
doCommandBySelector:(SEL)selector
{
    if (selector != @selector(cancelOperation:)) return NO;

    [self clear_search];
    [self focus_task_pane];
    return YES;
}

/* ── Shared state ────────────────────────────────────────────────────── */

/*
 * manual_sort_live — whether an order may be saved right now.
 * Output is documented in library_window.h.
 */
- (BOOL)
manual_sort_live
{
    return self.search == NULL;
}

/*
 * visible_pane — the task pane on screen.
 *
 * Output:
 *   the board while Kanban View is on, else the list.
 */
- (id<TasksTaskPane>)
visible_pane
{
    return self.kanban_on ? self.kanban : self.task_list;
}

/*
 * selected_task_ids — the visible pane's selection.
 * Output is documented in library_window.h.
 */
- (NSArray<NSNumber *> *)
selected_task_ids
{
    return [[self visible_pane] selected_task_ids];
}

/*
 * selection_changed — the visible pane's selection changed under the user:
 * the status bar's event message says how many tasks are selected.
 * Documented in library_window.h.
 */
- (void)
selection_changed
{
    if (self.is_shut_down) return;
    NSUInteger count = [self selected_task_ids].count;
    if (count == 0) return;
    task_app_status(self.app, "%lu task%s selected", (unsigned long)count, count == 1 ? "" : "s");
}

/*
 * apply_pane_mode — show exactly one of the two task panes.  The single
 * place that answers "which pane is on screen".  The pane being hidden
 * gives up its drag and its rows, so a command can never act on a
 * selection the user cannot see.
 */
- (void)
apply_pane_mode
{
    NSView<TasksTaskPane> *shown  = self.kanban_on ? self.kanban : self.task_list;
    NSView<TasksTaskPane> *hidden = self.kanban_on ? self.task_list : self.kanban;

    [hidden cancel_drag];
    [hidden show_rows:NULL];
    hidden.hidden = YES;
    shown.hidden  = NO;
}

/* ── Refresh ─────────────────────────────────────────────────────────── */

/*
 * update_window_title — set the window title from the current selection:
 * "Tasks — Name" for a view or a tag; the plain app name
 * otherwise.
 */
- (void)
update_window_title
{
    NSString   *suffix = nil;
    task_sel_t  sel    = _selection;

    if (sel.kind == task_sel_view) {
        const TaskView *view = task_view_nth((guint)sel.id);
        if (view) suffix = tasks_string(view->label);
    } else if (sel.kind == task_sel_tag) {
        TaskTag *tag = task_db_tag_get(self.app->db, sel.id);
        if (tag) {
            suffix = tasks_tag_title(tag);
            task_tag_free(tag);
        }
    }

    self.window.title = suffix
        ? [@TASKS_APP_NAME stringByAppendingFormat:@" \xe2\x80\x94 %@", suffix]
        : @TASKS_APP_NAME;
}

/*
 * select: — the sidebar or the Favorites bar selection changed.
 * Inputs are documented in library_window.h.
 */
- (void)
select:(task_sel_t)selection
{
    if (selection.kind != _selection.kind || selection.id != _selection.id) {
        _selection = selection;
        [self.task_list view_changed];
        [self.kanban view_changed];
        [self update_window_title];
        [self.sidebar update_selection];
        [self.favorites_bar update_selection];
    }
    [self refresh_tasks];
}

/*
 * refresh_tasks — rebuild the visible task pane and the status bar's count.
 */
- (void)
refresh_tasks
{
    if (self.is_shut_down) return;

    task_pane_content_t *content = task_pane_collect(self.app, self.selection, self.search);

    self.status_left.stringValue = tasks_string(content->status_text);
    task_row_list_t *rows = content->rows;
    content->rows = NULL;                    /* the pane owns them from here */
    task_pane_content_free(content);
    [[self visible_pane] show_rows:rows];
}

/*
 * refresh_sidebar — rebuild the sidebar and the Favorites bar from the
 * database.  A tag no task carries any more has gone for good (its id is
 * never reused), so its saved orders are dropped (task_order_drop).
 */
- (void)
refresh_sidebar
{
    if (self.is_shut_down) return;

    NSArray<NSArray *> *rows = tag_rows(self.app);
    NSMutableSet       *live = [NSMutableSet set];
    for (NSArray *row in rows) [live addObject:row[0]];
    for (NSArray *row in self.shown_tags) {
        if (![live containsObject:row[0]]) {
            task_order_drop((task_sel_t){ task_sel_tag, [row[0] longLongValue] });
        }
    }
    self.shown_tags = rows;

    [self.sidebar reload];
    [self.favorites_bar reload];
}

/*
 * full_refresh — the sidebar, the task pane, the day planner and every
 * open editor.
 */
- (void)
full_refresh
{
    [self refresh_sidebar];
    [self refresh_tasks];
    if (self.day_planner_shown) [self.day_planner reload];
    tasks_editor_refresh_all();
}

/*
 * set_status_alpha:over:then: — set the event message's opacity through its
 * animator.  EVERY change goes this way, even an instant one: a new value
 * set through the animator replaces a fade in progress, where a plain
 * assignment would be drawn over by the fade's remaining frames.
 *
 * Inputs:
 *   alpha    — 1 to show the message, 0 to hide it
 *   duration — seconds the change takes; 0 for at once
 *   done     — called when the change has finished or been replaced; may
 *              be nil
 */
- (void)
set_status_alpha:(CGFloat)alpha over:(NSTimeInterval)duration then:(void (^)(void))done
{
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        context.duration = duration;
        self.status_right.animator.alphaValue = alpha;
    } completionHandler:done];
}

/*
 * show_status_message: — put an event message in the status bar, hold it
 * for shared_look_status_hold, then fade it out over shared_look_status_fade
 * and clear it.  A newer message replaces it at once, at full strength,
 * whatever stage this one had reached.
 *
 * Inputs:
 *   message — the message
 */
- (void)
show_status_message:(NSString *)message
{
    /* Cancel the outstanding hold timer, releasing its captured state. */
    [self.status_timer invalidate];
    self.status_timer = nil;

    self.status_message_id++;
    self.status_right.stringValue = message;
    [self set_status_alpha:1.0 over:0.0 then:nil];

    NSUInteger message_id = self.status_message_id;
    __weak TasksLibraryWindow *ws = self;
    self.status_timer = [NSTimer scheduledTimerWithTimeInterval:shared_look_status_hold
                                                        repeats:NO
                                                          block:^(NSTimer *t) {
        TasksLibraryWindow *s = ws;
        if (!s || s.status_message_id != message_id) return;
        s.status_timer = nil;
        [s set_status_alpha:0.0 over:shared_look_status_fade then:^{
            TasksLibraryWindow *faded = ws;
            if (!faded || faded.status_message_id != message_id) return;
            faded.status_right.stringValue = @"";
        }];
    }];
}

/* ── Task context menu ───────────────────────────────────────────────── */

/*
 * fill_menu:with_destinations_of: — rebuild the "Add Tag" submenu: every
 * tag, less those a single selected task already carries.
 * Inputs are documented in library_window.h.
 */
- (void)
fill_menu:(NSMenu *)menu with_destinations_of:(NSString *)ident
{
    [menu removeAllItems];
    if (self.is_shut_down || ![ident isEqualToString:@"add-tag"]) return;

    NSArray<NSNumber *> *ids  = [self selected_task_ids];
    GPtrArray *carried = ids.count == 1 ? task_db_task_tags(self.app->db, ids[0].longLongValue)
                                        : NULL;
    GPtrArray *tags    = task_db_tags(self.app->db, TASK_TAGS_ALL);

    for (guint i = 0; i < tags->len; i++) {
        const TaskTag *tag = g_ptr_array_index(tags, i);
        BOOL           has = NO;
        for (guint k = 0; carried != NULL && k < carried->len && !has; k++) {
            has = ((const TaskTag *)g_ptr_array_index(carried, k))->id == tag->id;
        }
        if (has) continue;
        /* Its parameter (never nil) is also what makes validation keep
         * the tag's name as its title. */
        NSMenuItem *item = shared_command_menu_item(ident, @(tag->id));
        item.title = tasks_tag_title(tag);
        [menu addItem:item];
    }
    task_ptr_array_free_tags(tags);
    task_ptr_array_free_tags(carried);
}

/*
 * task_context_menu — the context menu for the current selection.
 * Output is documented in library_window.h.
 *
 * A single task gets only the direction that applies to it; several (their
 * states may be mixed) get both directions of each flag.
 */
- (NSMenu *)
task_context_menu
{
    NSArray<NSNumber *> *ids = [self selected_task_ids];
    if (ids.count == 0) return nil;

    BOOL  single = ids.count == 1;
    Task *task   = single ? task_db_task_get(self.app->db, ids[0].longLongValue) : NULL;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];

    if (single) {
        [menu addItem:shared_command_menu_item(@"task-info", nil)];
        [menu addItem:NSMenuItem.separatorItem];
    }

    if (task != NULL) {
        [menu addItem:tasks_command_flag_item(@"mark-done", task->status != TASK_STATUS_DONE, NO)];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:tasks_command_flag_item(@"set-priority", !task->priority, NO)];
    } else {
        [menu addItem:tasks_command_flag_item(@"mark-done", YES, YES)];
        [menu addItem:tasks_command_flag_item(@"mark-done", NO, YES)];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:tasks_command_flag_item(@"set-priority", YES, YES)];
        [menu addItem:tasks_command_flag_item(@"set-priority", NO, YES)];
    }
    [menu addItem:NSMenuItem.separatorItem];

    /* Add Tag: every tag, left out when there is none to add. */
    NSMenu *tags = [[NSMenu alloc] initWithTitle:@""];
    [self fill_menu:tags with_destinations_of:@"add-tag"];
    if (tags.numberOfItems > 0) {
        NSMenuItem *holder = [[NSMenuItem alloc]
            initWithTitle:shared_command_find(@"add-tag")->title
                   action:NULL
            keyEquivalent:@""];
        holder.submenu = tags;
        [menu addItem:holder];
        [menu addItem:NSMenuItem.separatorItem];
    }

    [menu addItem:shared_command_menu_item(@"delete-task", nil)];
    task_free(task);
    return menu;
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * run_command: — carry out an item's command.
 * Inputs are documented in library_window.h.
 */
- (void)
run_command:(id)sender
{
    if (self.is_shut_down) return;
    for (int i = 0; i < COMMAND_TABLE_COUNT; i++) {
        if (shared_command_run(_command_tables[i].table, _command_tables[i].n,
                               _command_tables[i].target, sender)) break;
    }
    [self.window.toolbar validateVisibleItems];
}

/*
 * plural — "" for one, "s" otherwise.
 *
 * Inputs:
 *   count — how many
 *
 * Output:
 *   the plural suffix.
 */
static const char *
plural(NSUInteger count)
{
    return count == 1 ? "" : "s";
}

/*
 * create_task_tagged:quick: — create an empty task, with a tag or none,
 * and open its editor (the variant with Cancel, which deletes the task
 * again).
 *
 * Inputs:
 *   tag   — the tag's name (made when no task carries it yet), or NULL for
 *           none: the task is listed under All Tasks alone
 *   quick — YES for Quick Task: the editor opens in the corner of the
 *           screen the user is on, and the app comes forward with it (the
 *           shortcut may have been pressed in another app)
 */
- (void)
create_task_tagged:(const char *)tag quick:(BOOL)quick
{
    int64_t task_id = task_db_task_create(self.app->db, 0, "New Task", tag);
    if (task_id == 0) {
        task_app_status(self.app, "Could not create the task");
        return;
    }
    [self full_refresh];
    tasks_editor_open_new(self.app, task_id, quick);
    if (quick) [NSApp activate];
}

/*
 * new_task — New Task: a task with the selected tag or, when a view is
 * selected, without one (task_sel_new_task_tag).
 */
- (void)
new_task
{
    gchar *tag = task_sel_new_task_tag(self.app, self.selection);
    [self create_task_tagged:tag quick:NO];
    g_free(tag);
}

/*
 * quick_task — Quick Task (the menu item or the system-wide shortcut): a
 * task without a tag whatever is selected (it is listed under All Tasks
 * alone), its editor in the lower-right corner of the screen.
 */
- (void)
quick_task
{
    [self create_task_tagged:NULL quick:YES];
}

/*
 * delete_tasks — ask, then delete the selected tasks (with their subtasks).
 * Each task's editor is closed first, so its final save cannot land on a
 * deleted row.
 */
- (void)
delete_tasks
{
    NSArray<NSNumber *> *ids = [self selected_task_ids];
    if (ids.count == 0) {
        task_app_status(self.app, "Select a task to delete");
        return;
    }

    NSString *title;
    if (ids.count == 1) {
        Task *task = task_db_task_get(self.app->db, ids[0].longLongValue);
        if (task == NULL) return;
        title = [NSString stringWithFormat:@"Delete “%@”%@?",
                 tasks_string(*task->title != '\0' ? task->title : "Untitled Task"),
                 task->parent_id == 0 ? @" and its subtasks" : @""];
        task_free(task);
    } else {
        title = [NSString stringWithFormat:@"Delete the %lu selected tasks and their subtasks?",
                 (unsigned long)ids.count];
    }

    __weak TasksLibraryWindow *ws = self;
    shared_ui_confirm(self.window, title, @"You cannot undo this action.", @"Delete", YES,
                     ^(BOOL confirmed) {
        TasksLibraryWindow *s = ws;
        if (!s || s.is_shut_down || !confirmed) return;
        for (NSNumber *task_id in ids) {
            tasks_editor_close(task_id.longLongValue);
            task_db_task_delete(s.app->db, task_id.longLongValue);
        }
        [s full_refresh];
        task_app_status(s.app, "Deleted %lu task%s", (unsigned long)ids.count, plural(ids.count));
    });
}

/*
 * clear_completed — ask, then remove the completed tasks carrying the
 * selected tag (from every tag they carry: they are deleted).
 */
- (void)
clear_completed
{
    int64_t  tag_id = task_sel_tag_id(self.selection);
    TaskTag *tag    = tag_id != 0 ? task_db_tag_get(self.app->db, tag_id) : NULL;
    if (tag == NULL) {
        task_app_status(self.app, "Select a tag to clear its completed tasks");
        return;
    }
    NSString *title = [NSString stringWithFormat:@"Delete every completed task tagged “%@”?",
                       tasks_string(tag->name)];
    task_tag_free(tag);

    __weak TasksLibraryWindow *ws = self;
    shared_ui_confirm(self.window, title, @"You cannot undo this action.", @"Clear", YES,
                     ^(BOOL confirmed) {
        TasksLibraryWindow *s = ws;
        if (!s || s.is_shut_down || !confirmed) return;
        guint cleared = task_ops_clear_completed(s.app, tag_id);
        [s full_refresh];
        task_app_status(s.app, "Cleared %u completed task%s", cleared, plural(cleared));
    });
}

/*
 * set_flag:on: — apply one of the context menu's flags to every selected
 * task, then refresh everything (the sort order can change) and say what
 * was done — or that it was not saved.
 *
 * Inputs:
 *   ident   — "mark-done" or "set-priority"
 *   on      — the direction.  For mark-done: YES is Done, NO is In Progress
 *             (the checkbox's two verbs — a task that was ticked has been
 *             worked on, so it does not go back to New)
 */
- (void)
set_flag:(NSString *)ident on:(BOOL)on
{
    BOOL done = [ident isEqualToString:@"mark-done"];
    NSArray<NSNumber *> *ids    = [self selected_task_ids];
    unsigned long        failed = 0;

    for (NSNumber *task_id in ids) {
        int64_t  id = task_id.longLongValue;
        gboolean ok;
        if (done) {
            ok = task_db_task_set_status(self.app->db, id,
                                         on ? TASK_STATUS_DONE : TASK_STATUS_IN_PROGRESS);
        } else {
            ok = task_db_task_set_priority(self.app->db, id, on);
        }
        if (!ok) failed++;
    }
    [self full_refresh];

    unsigned long count = (unsigned long)ids.count;
    if (failed > 0) {
        /* Refreshes again, to what the database holds. */
        gchar *what = g_strdup_printf("The change to %lu task%s", failed, plural(failed));
        task_app_write_failed(self.app, what);
        g_free(what);
    } else if (done) {
        task_app_status(self.app, "Marked %lu task%s %s", count, plural(count),
                        on ? "complete" : "incomplete");
    } else {
        task_app_status(self.app, "%s high priority on %lu task%s", on ? "Set" : "Cleared",
                        count, plural(count));
    }
}

/*
 * add_tag: — give every selected task a tag (Add Tag in the context menu),
 * then say how many it was added to — or that it was not saved.
 *
 * Inputs:
 *   tag_id — the tag
 */
- (void)
add_tag:(int64_t)tag_id
{
    TaskTag *tag = task_db_tag_get(self.app->db, tag_id);
    if (tag == NULL) return;

    NSArray<NSNumber *> *ids    = [self selected_task_ids];
    gboolean             began  = task_db_transaction_begin(self.app->db);
    gboolean             ok     = TRUE;
    for (NSNumber *task_id in ids) {
        ok = task_db_task_tag_add(self.app->db, task_id.longLongValue, tag->name) != 0 && ok;
    }
    if (began) ok = task_db_transaction_end(self.app->db, ok);
    [self full_refresh];

    unsigned long count = (unsigned long)ids.count;
    if (!ok) {
        gchar *what = g_strdup_printf("The tag \xe2\x80\x9c%s\xe2\x80\x9d", tag->name);
        task_app_write_failed(self.app, what);
        g_free(what);
    } else {
        task_app_status(self.app, "Tagged %lu task%s \xe2\x80\x9c%s\xe2\x80\x9d", count,
                        plural(count), tag->name);
    }
    task_tag_free(tag);
}

/*
 * slide:size:to:show:animated: — show or hide a panel by its extent: a
 * hidden panel has none (the sidebar no width, the Favorites bar no
 * height), so its neighbours take the room.
 *
 * Inputs:
 *   view     — the panel
 *   size     — the constraint holding its width or height
 *   extent   — its extent while shown
 *   show     — YES to show it
 *   animated — YES to slide it
 */
- (void)
slide:(NSView *)view size:(NSLayoutConstraint *)size to:(CGFloat)extent show:(BOOL)show
     animated:(BOOL)animated
{
    CGFloat target = show ? extent : 0.0;

    if (!animated) {
        view.hidden   = !show;
        size.constant = target;
        return;
    }
    view.hidden = NO;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.15;
        ctx.allowsImplicitAnimation = YES;
        size.animator.constant = target;
    } completionHandler:^{
        /* From the extent in force, not this slide's direction: a slide
         * the other way may have begun since, and it decides. */
        view.hidden = size.constant == 0.0;
    }];
}

/*
 * show_sidebar:animated: — show or hide the tag sidebar: its split item
 * collapses (the split view controller slides it and remembers its width).
 *
 * Inputs:
 *   show     — YES to show it
 *   animated — YES to slide it
 */
- (void)
show_sidebar:(BOOL)show animated:(BOOL)animated
{
    if (animated) {
        self.sidebar_item.animator.collapsed = !show;
    } else {
        self.sidebar_item.collapsed = !show;
    }
}

/*
 * sidebar_extent — the shown sidebar's width: the one it was last dragged
 * to, at least shared_look_sidebar_min_w; shared_look_sidebar_w until the
 * first drag.
 *
 * Output:
 *   the width, in points.
 */
- (CGFloat)
sidebar_extent
{
    gint saved = task_config_get_int(TASK_CFG_SIDEBAR_WIDTH);
    if (saved == 0) return shared_look_sidebar_w;
    return MAX((CGFloat)saved, shared_look_sidebar_min_w);
}

/*
 * planner_extent — the shown day planner's width: the one it was last
 * dragged to, at least tasks_look_day_planner_min_w; tasks_look_day_planner_w
 * until the first drag.
 *
 * Output:
 *   the width, in points.
 */
- (CGFloat)
planner_extent
{
    gint saved = task_config_get_int(TASK_CFG_DAY_PLANNER_W);
    if (saved == 0) return tasks_look_day_planner_w;
    return MAX((CGFloat)saved, tasks_look_day_planner_min_w);
}

/*
 * day_planner_shown — see library_window.h.
 */
- (BOOL)
day_planner_shown
{
    return !self.planner_item.collapsed;
}

/*
 * show_favorites_bar:animated: — show or hide the Favorites bar.
 *
 * Inputs:
 *   show     — YES to show it
 *   animated — YES to slide it
 */
- (void)
show_favorites_bar:(BOOL)show animated:(BOOL)animated
{
    [self slide:self.favorites_bar size:self.favorites_height to:shared_look_favorites_bar_h + 1.0
           show:show animated:animated];
}

/*
 * toggle_sidebar — slide the tag sidebar in or out and remember the choice.
 * The window's least width follows; a window too narrow for the sidebar
 * and the pane together widens, keeping its left edge.
 */
- (void)
toggle_sidebar
{
    BOOL show = self.sidebar_item.collapsed;

    task_config_set_bool(TASK_CFG_SIDEBAR_VISIBLE, show);
    NSSize least = [self content_min_size];
    self.window.contentMinSize = least;
    NSRect content = [self.window contentRectForFrameRect:self.window.frame];
    if (NSWidth(content) < least.width) {
        content.size.width = least.width;
        [self.window setFrame:[self.window frameRectForContentRect:content] display:YES animate:YES];
    }
    [self show_sidebar:show animated:YES];
}

/*
 * toggle_favorites_bar — show or hide the Favorites bar and remember the
 * choice.
 */
- (void)
toggle_favorites_bar
{
    BOOL show = self.favorites_height.constant == 0.0;

    [self show_favorites_bar:show animated:YES];
    task_config_set_bool(TASK_CFG_FAVORITES_BAR_VISIBLE, show);
}

/*
 * content_min_size — the least content size of the mode in force: the task
 * pane alone in Compact; in full mode the pane plus the sidebar and the
 * day planner, each while it is shown.
 *
 * Output:
 *   the size, for the window's contentMinSize.
 */
- (NSSize)
content_min_size
{
    if (task_config_get_bool(TASK_CFG_COMPACT)) {
        return NSMakeSize(tasks_look_pane_min_w, tasks_look_compact_min_h);
    }
    CGFloat width = tasks_look_pane_min_w;
    if (task_config_get_bool(TASK_CFG_SIDEBAR_VISIBLE)) {
        width += [self sidebar_extent] + self.split.splitView.dividerThickness;
    }
    if (task_config_get_bool(TASK_CFG_DAY_PLANNER_VISIBLE)) {
        width += self.split.splitView.dividerThickness + tasks_look_day_planner_min_w;
    }
    return NSMakeSize(width, tasks_look_window_min_h);
}

/*
 * toggle_day_planner — slide the day planner in or out and remember the
 * choice: its split item collapses, the same slide as the sidebar's (the
 * split view controller remembers its width).  The window's least width
 * follows: the planner's minimum counts only while it shows.
 */
- (void)
toggle_day_planner
{
    BOOL show = self.planner_item.collapsed;
    task_config_set_bool(TASK_CFG_DAY_PLANNER_VISIBLE, show);
    self.window.contentMinSize = [self content_min_size];
    self.planner_item.animator.collapsed = !show;
    if (show) {
        [self.day_planner reload];
        [self.day_planner scroll_to_now];
    }
}

/* ── Compact mode ────────────────────────────────────────────────────── */

/*
 * apply_compact_layout_at:animated: — THE applier of Compact mode: make
 * the window match the compact_layout setting, at a given content size.
 * Everything that differs between the two modes is set here and nowhere
 * else — the toolbar, the floating controls, the sidebar, the minimum
 * size, the search (which Compact mode ends, because the field that would
 * show and clear it is hidden) and the window's size.
 *
 * In full mode the sidebar and the Favorites bar follow their settings; in
 * Compact mode both are always hidden.  The resize keeps the window's top-left corner
 * where it is, so the title bar does not move under the pointer.
 *
 * Inputs:
 *   content  — the content size to give the window (it is held to the
 *              mode's minimum)
 *   animated — YES to slide the sidebar and resize the window together
 *              (a toggle); NO to set everything at once (at launch)
 */
- (void)
apply_compact_layout_at:(NSSize)content animated:(BOOL)animated
{
    BOOL   compact  = task_config_get_bool(TASK_CFG_COMPACT);
    BOOL   no_side  = compact || !task_config_get_bool(TASK_CFG_SIDEBAR_VISIBLE);
    BOOL   no_bar   = compact || !task_config_get_bool(TASK_CFG_FAVORITES_BAR_VISIBLE);
    NSSize least    = [self content_min_size];
    NSRect old      = self.window.frame;

    self.window.toolbar.visible = !compact;
    self.window.contentMinSize  = least;
    self.float_bar.hidden       = !compact;
    if (compact && self.search != NULL) [self clear_search];

    /* The frame is worked out AFTER the toolbar has come or gone, because
     * the toolbar is part of the frame and not of the content. */
    NSRect frame = [self.window frameRectForContentRect:
        NSMakeRect(0, 0, MAX(content.width, least.width), MAX(content.height, least.height))];
    frame.origin = NSMakePoint(NSMinX(old), NSMaxY(old) - NSHeight(frame));
    frame = [self.window constrainFrameRect:frame toScreen:self.window.screen];

    if (!animated) {
        [self show_favorites_bar:!no_bar animated:NO];
        [self show_sidebar:!no_side animated:NO];
        [self.window setFrame:frame display:NO];
        return;
    }

    self.compact_animating = YES;
    __weak TasksLibraryWindow *ws = self;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        [self.window.animator setFrame:frame display:YES];
    } completionHandler:^{
        ws.compact_animating = NO;
    }];
    [self show_sidebar:!no_side animated:YES];
    [self show_favorites_bar:!no_bar animated:YES];
}

/*
 * toggle_compact — View > Compact Controls / Full Controls: switch modes.
 * Each mode has its own remembered window size: the size being left is
 * saved for the next time that mode is entered, and the window goes to the
 * size remembered for the mode being entered (compact_w/h for Compact,
 * compact_pre_w/h — the size it had before going Compact — for full).
 * Leaving Compact always brings the sidebar and the Favorites bar back,
 * and records that.
 */
- (void)
toggle_compact
{
    if (self.compact_animating) return;

    BOOL   compact = !task_config_get_bool(TASK_CFG_COMPACT);
    NSSize now     = self.window.contentView.frame.size;

    if (compact) {
        save_size(TASK_CFG_COMPACT_PRE_W, TASK_CFG_COMPACT_PRE_H, now);
    } else {
        save_size(TASK_CFG_COMPACT_W, TASK_CFG_COMPACT_H, now);
        task_config_set_bool(TASK_CFG_SIDEBAR_VISIBLE, TRUE);
        task_config_set_bool(TASK_CFG_FAVORITES_BAR_VISIBLE, TRUE);
    }
    task_config_set_bool(TASK_CFG_COMPACT, compact);

    NSSize target = compact ? saved_size(TASK_CFG_COMPACT_W, TASK_CFG_COMPACT_H)
                            : saved_size(TASK_CFG_COMPACT_PRE_W, TASK_CFG_COMPACT_PRE_H);
    [self apply_compact_layout_at:target animated:YES];
}

/*
 * windowDidUpdate: — AppKit has finished handling an event: the moment it
 * re-validates toolbar items.  The floating buttons are ordinary buttons,
 * which nothing validates, so while they are showing they are brought up
 * to date here from the same state_of_command: the toolbar uses.
 *
 * Inputs:
 *   note — NSWindowDidUpdateNotification (unused)
 */
- (void)
windowDidUpdate:(NSNotification *)note
{
    if (self.float_bar.hidden) return;

    for (NSButton *button in self.float_buttons) {
        shared_command_apply([self state_of_command:shared_command_ident(button)], button);
    }
}

/* ── The database ────────────────────────────────────────────────────── */

/*
 * report_database_health — check the open database and say what was found.
 * Documented in library_window.h.
 */
- (void)
report_database_health
{
    if (task_db_health_check(self.app->db)) {
        task_app_status(self.app, "DB at %s loaded, integrity check passed",
                        shared_db_file(self.app->db->shared));
        return;
    }
    const TaskDbHealth *health = task_db_health(self.app->db);
    NSString *lead = health->ran ? @"The database integrity check found issues:"
                                 : @"The database integrity check did not complete:";
    shared_ui_notice(self.window, @"Database Integrity Check",
                    [NSString stringWithFormat:@"%@\n\n%@", lead,
                        tasks_string(health->detail != NULL ? health->detail : "?")],
                    nil);
}

/*
 * offer_default_database: — after a database was opened by hand, ask
 * whether both apps should open it at every launch.  Nothing is asked when
 * it already is the default (task_switch_can_be_default).  "Make Default"
 * asks the shared module, and a refusal (a sandboxed run, while
 * $CAMBIO_SHARED_DB is set) is shown in the module's words.  Declining
 * leaves it open for this session only.
 *
 * Inputs:
 *   path — the database just opened
 */
- (void)
offer_default_database:(NSString *)path
{
    if (!task_switch_can_be_default(path.fileSystemRepresentation)) return;

    __weak TasksLibraryWindow *ws = self;
    shared_ui_confirm(self.window, @"Make this the default database?",
                     [NSString stringWithFormat:
                         @"%@\n\nNotes and " @TASKS_APP_NAME @" will open it at every launch.  "
                         @"Cancel keeps it open in " @TASKS_APP_NAME @" for this session only.",
                         path],
                     @"Make Default", NO, ^(BOOL confirmed) {
        TasksLibraryWindow *s = ws;
        if (!s || s.is_shut_down || !confirmed) return;
        /* Only if it is still the open database: another may have been
         * opened while the question was up. */
        const char *file = path.fileSystemRepresentation;
        if (strcmp(shared_db_file(s.app->db->shared), file) != 0) return;
        char *why = NULL;
        if (shared_db_set_default_path(file, &why) == 0) {
            task_app_status(s.app, "Default database set to %s", file);
        } else {
            shared_ui_notice(s.window, @"Could not make it the default",
                            tasks_string(why), nil);
        }
        free(why);
    });
}

/*
 * open_database: — switch the running app to another database file.
 * Inputs and the full behaviour are documented in library_window.h.
 */
- (void)
open_database:(NSString *)path
{
    if (self.is_shut_down) return;

    const char *file = path.fileSystemRepresentation;
    char       *why  = NULL;
    task_switch_check_t verdict = task_switch_check(self.app, file, &why);

    if (verdict == task_switch_already_open) {
        task_app_status(self.app, "%s is already open", file);
        return;
    }
    if (verdict == task_switch_can_open) {
        /* The editors close first: each flushes its pending save, which
         * must land in the database the task came from. */
        tasks_editor_close_all();
        if (task_switch_database(self.app, file, &why)) {
            /* The selection named a row of the old database. */
            [self select:task_sel_default()];
            task_app_notify_changed(self.app);
            [self report_database_health];
            [self offer_default_database:path];
            return;
        }
    }
    shared_ui_notice(self.window, @"Could not open the database",
                    [NSString stringWithFormat:@"%@\n\n%@", path, tasks_string(why)], nil);
    free(why);
}

/*
 * pick_database — File > Open Database File...: let the user pick a .db
 * file, then switch to it.
 */
- (void)
pick_database
{
    __weak TasksLibraryWindow *ws = self;
    shared_ui_pick_path(SharedPickOpenFile, @"Choose a shared Notes and Tasks database to open.",
                        @"Open", @[[UTType typeWithFilenameExtension:@"db"]], nil, ^(NSString *path) {
        if (!path) return;
        /* On the next pass: the panel's sheet is still coming down, and
         * what follows may put up a sheet of its own. */
        dispatch_async(dispatch_get_main_queue(), ^{
            [ws open_database:path];
        });
    });
}

/* ── Command dispatch ────────────────────────────────────────────────── */

/*
 * toggle_pane — the list or the Kanban board, remembered.
 */
- (void)
toggle_pane
{
    _kanban_on = !_kanban_on;
    task_config_set_bool(TASK_CFG_KANBAN_VIEW, _kanban_on);
    [self apply_pane_mode];
    [self refresh_tasks];
    /* The keyboard follows the pane: the one it was in is now hidden. */
    [self focus_task_pane];
}

/*
 * perform_command:param: — a command without an item.
 * Inputs are documented in library_window.h.
 */
- (void)
perform_command:(NSString *)ident param:(id)param
{
    [self run_command:shared_command_menu_item(ident, param)];
}

/*
 * state_of_command: — how a command looks right now.
 * Inputs/output are documented in library_window.h.
 */
- (SharedCommandState *)
state_of_command:(NSString *)ident
{
    SharedCommandState *state = nil;
    for (int i = 0; i < COMMAND_TABLE_COUNT && state == nil && !self.is_shut_down; i++) {
        state = shared_command_state(_command_tables[i].table, _command_tables[i].n,
                                     _command_tables[i].target, ident);
    }
    if (state == nil) {
        state = shared_command_default_state(ident);
        state.enabled = NO;
    }
    return state;
}

/* ── Lifetime ────────────────────────────────────────────────────────── */

/*
 * windowWillClose: — the window is closing: tear everything down while the
 * database is still open.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [self shut_down];
}

/*
 * save_geometry — remember the sidebar's width and the window's place and
 * size for the next launch.  The size always goes to win_w/h; a window
 * closing in Compact mode also saves it as the Compact size, which is what
 * the next launch (still in Compact mode) opens at.  A full-screen window
 * saves no place or size: the screen's are not the window's own.
 */
- (void)
save_geometry
{
    if (self.window.styleMask & NSWindowStyleMaskFullScreen) return;

    NSRect frame   = self.window.frame;
    NSSize content = self.window.contentView.frame.size;

    task_config_set_int(TASK_CFG_WIN_X, (gint)frame.origin.x);
    task_config_set_int(TASK_CFG_WIN_Y, (gint)frame.origin.y);
    save_size(TASK_CFG_WIN_W, TASK_CFG_WIN_H, content);
    if (task_config_get_bool(TASK_CFG_COMPACT)) {
        save_size(TASK_CFG_COMPACT_W, TASK_CFG_COMPACT_H, content);
    }
}

/*
 * shut_down — tear the window's connections down, in order.
 * The order and the reasons are documented in library_window.h.
 */
- (void)
shut_down
{
    if (self.is_shut_down) return;
    self.is_shut_down = YES;

    [self save_geometry];

    task_app_unlisten(self.app, self.listen_changed);
    task_app_unlisten(self.app, self.listen_tasks);
    task_app_unlisten(self.app, self.listen_notes);
    task_app_unlisten(self.app, self.listen_status);
    self.listen_changed = self.listen_tasks = self.listen_notes = self.listen_status = 0;
    self.status_message_id++;       /* a pending status fade finds nothing to do */
    [self.status_timer invalidate];
    self.status_timer = nil;

    [self.task_list cancel_drag];
    [self.kanban cancel_drag];
    tasks_editor_close_all();

    [self.sidebar shut_down];
    [self.favorites_bar shut_down];
    [self.task_list shut_down];
    [self.kanban shut_down];
    [self.day_planner shut_down];

    task_search_free((TaskSearch *)_search);
    _search = NULL;
}

@end

/* ── The window's command handlers ───────────────────────────────────── */
/* Each takes this window as its target.  A toggle's validate spells only
 * the face the table does not hold (commands.h). */

/*
 * run_new_task — New Task.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_new_task(id target, id sender)
{
    [(TasksLibraryWindow *)target new_task];
}

/*
 * run_quick_task — Quick Task.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_quick_task(id target, id sender)
{
    [(TasksLibraryWindow *)target quick_task];
}

/*
 * run_delete_task — Delete Task: the selected tasks, after a confirm.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_delete_task(id target, id sender)
{
    [(TasksLibraryWindow *)target delete_tasks];
}

/*
 * validate_delete_task — one or more tasks selected, and no text being
 * edited in any window: the shortcut is also the text system's "delete to
 * the start of the line", and a disabled menu item lets the key through
 * to the field.  Several tasks are counted in the title.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: whether the command may run.
 */
static BOOL
validate_delete_task(id target, id item, SharedCommandState *state)
{
    NSUInteger selected = [(TasksLibraryWindow *)target selected_task_ids].count;
    if (selected > 1) {
        state.title = [NSString stringWithFormat:@"Delete %lu Tasks", (unsigned long)selected];
    }
    return selected > 0 && ![NSApp.keyWindow.firstResponder isKindOfClass:NSText.class];
}

/*
 * run_task_info — Info…: the editor for the one selected task.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_task_info(id target, id sender)
{
    TasksLibraryWindow  *lib = target;
    NSArray<NSNumber *> *ids = [lib selected_task_ids];
    if (ids.count > 0) tasks_editor_open(lib.app, ids[0].longLongValue);
}

/*
 * validate_task_info — exactly one task selected.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look (unused)
 *
 * Output: whether the command may run.
 */
static BOOL
validate_task_info(id target, id item, SharedCommandState *state)
{
    return [(TasksLibraryWindow *)target selected_task_ids].count == 1;
}

/*
 * validate_selected — at least one task selected (the flags, Add Tag).
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look (unused)
 *
 * Output: whether the command may run.
 */
static BOOL
validate_selected(id target, id item, SharedCommandState *state)
{
    return [(TasksLibraryWindow *)target selected_task_ids].count > 0;
}

/*
 * run_clear_completed — Clear Completed Tasks: the selected tag's.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_clear_completed(id target, id sender)
{
    [(TasksLibraryWindow *)target clear_completed];
}

/*
 * validate_clear_completed — a tag is selected.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look (unused)
 *
 * Output: whether the command may run.
 */
static BOOL
validate_clear_completed(id target, id item, SharedCommandState *state)
{
    return task_sel_tag_id(((TasksLibraryWindow *)target).selection) != 0;
}

/*
 * run_open_database — Open Database File…
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_open_database(id target, id sender)
{
    [(TasksLibraryWindow *)target pick_database];
}

/*
 * run_find — Find…: the keyboard into the search field.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_find(id target, id sender)
{
    TasksLibraryWindow *lib = target;
    [lib.window makeFirstResponder:lib.search_field];
}

/*
 * validate_not_compact — not in Compact mode, which hides the toolbar
 * with its search field, and the Favorites bar whatever its setting.
 *
 * Inputs:
 *   target — the window (unused)
 *   item   — the item (unused)
 *   state  — the command's look (unused)
 *
 * Output: whether the command may run.
 */
static BOOL
validate_not_compact(id target, id item, SharedCommandState *state)
{
    return !task_config_get_bool(TASK_CFG_COMPACT);
}

/*
 * run_toggle_completed — Hide / Show Completed.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_toggle_completed(id target, id sender)
{
    task_config_set_bool(TASK_CFG_SHOW_COMPLETED, !task_config_get_bool(TASK_CFG_SHOW_COMPLETED));
    [(TasksLibraryWindow *)target refresh_tasks];
}

/*
 * validate_toggle_completed — the other face while they are hidden.
 *
 * Inputs:
 *   target — the window (unused)
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: YES.
 */
static BOOL
validate_toggle_completed(id target, id item, SharedCommandState *state)
{
    if (!task_config_get_bool(TASK_CFG_SHOW_COMPLETED)) {
        shared_command_state_set(state, @"Show Completed", @"eye", NO);
        state.tooltip = @"Show completed tasks";
    }
    return YES;
}

/*
 * run_toggle_sidebar — Hide / Show Sidebar.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_toggle_sidebar(id target, id sender)
{
    [(TasksLibraryWindow *)target toggle_sidebar];
}

/*
 * validate_toggle_sidebar — the other face while it is collapsed.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: YES.
 */
static BOOL
validate_toggle_sidebar(id target, id item, SharedCommandState *state)
{
    if (((TasksLibraryWindow *)target).sidebar_item.collapsed) {
        shared_command_state_set(state, @"Show Sidebar", nil, NO);
        state.tooltip = @"Show the sidebar";
    }
    return YES;
}

/*
 * run_toggle_favorites_bar — Hide / Show Favorites Bar.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_toggle_favorites_bar(id target, id sender)
{
    [(TasksLibraryWindow *)target toggle_favorites_bar];
}

/*
 * validate_toggle_favorites_bar — the other face while it is hidden; not
 * in Compact mode.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: whether the command may run.
 */
static BOOL
validate_toggle_favorites_bar(id target, id item, SharedCommandState *state)
{
    if (((TasksLibraryWindow *)target).favorites_height.constant == 0.0) {
        shared_command_state_set(state, @"Show Favorites Bar", nil, NO);
    }
    return validate_not_compact(target, item, state);
}

/*
 * run_toggle_day_planner — Hide / Show Day Planner.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_toggle_day_planner(id target, id sender)
{
    [(TasksLibraryWindow *)target toggle_day_planner];
}

/*
 * validate_toggle_day_planner — the other face while it is hidden.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: YES.
 */
static BOOL
validate_toggle_day_planner(id target, id item, SharedCommandState *state)
{
    if (!((TasksLibraryWindow *)target).day_planner_shown) {
        shared_command_state_set(state, @"Show Day Planner", nil, NO);
        state.tooltip = @"Show the day planner";
    }
    return YES;
}

/*
 * run_toggle_compact — Compact Controls / Full Controls.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_toggle_compact(id target, id sender)
{
    [(TasksLibraryWindow *)target toggle_compact];
}

/*
 * validate_toggle_compact — the other face in Compact mode; not in full
 * screen, where the window has no size of its own to shrink or restore.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: whether the command may run.
 */
static BOOL
validate_toggle_compact(id target, id item, SharedCommandState *state)
{
    if (task_config_get_bool(TASK_CFG_COMPACT)) shared_command_state_set(state, @"Full Controls", nil, NO);
    return (((TasksLibraryWindow *)target).window.styleMask & NSWindowStyleMaskFullScreen) == 0;
}

/*
 * run_toggle_pane — Kanban View / List View.
 *
 * Inputs:
 *   target — the window
 *   sender — the item (unused)
 */
static void
run_toggle_pane(id target, id sender)
{
    [(TasksLibraryWindow *)target toggle_pane];
}

/*
 * validate_toggle_pane — the other face while the board is up.
 *
 * Inputs:
 *   target — the window
 *   item   — the item (unused)
 *   state  — the command's look, adjusted here
 *
 * Output: YES.
 */
static BOOL
validate_toggle_pane(id target, id item, SharedCommandState *state)
{
    if (((TasksLibraryWindow *)target).kanban_on) {
        shared_command_state_set(state, @"List View", @"list.bullet", NO);
        state.tooltip = @"Show the tasks as a list";
    }
    return YES;
}

/*
 * run_set_flag — Mark Complete / Incomplete, Set / Clear High Priority:
 * the item's parameter is the direction.
 *
 * Inputs:
 *   target — the window
 *   sender — the item
 */
static void
run_set_flag(id target, id sender)
{
    [(TasksLibraryWindow *)target set_flag:shared_command_ident(sender)
                                        on:[shared_command_param(sender) boolValue]];
}

/*
 * run_add_tag — Add Tag: the item's parameter is the tag.
 *
 * Inputs:
 *   target — the window
 *   sender — the item
 */
static void
run_add_tag(id target, id sender)
{
    [(TasksLibraryWindow *)target add_tag:[shared_command_param(sender) longLongValue]];
}

/*
 * shell_commands — the window's handler table.
 *
 * Inputs:
 *   n — receives how many
 *
 * Output: the table (static).
 */
static const shared_command_handler_t *
shell_commands(size_t *n)
{
    static const shared_command_handler_t table[] = {
        { @"new-task",             run_new_task,             NULL                          },
        { @"quick-task",           run_quick_task,           NULL                          },
        { @"delete-task",          run_delete_task,          validate_delete_task          },
        { @"task-info",            run_task_info,            validate_task_info            },
        { @"clear-completed",      run_clear_completed,      validate_clear_completed      },
        { @"open-database",        run_open_database,        NULL                          },
        { @"find",                 run_find,                 validate_not_compact          },
        { @"toggle-completed",     run_toggle_completed,     validate_toggle_completed     },
        { @"toggle-sidebar",       run_toggle_sidebar,       validate_toggle_sidebar       },
        { @"toggle-favorites-bar", run_toggle_favorites_bar, validate_toggle_favorites_bar },
        { @"toggle-day-planner",   run_toggle_day_planner,   validate_toggle_day_planner   },
        { @"toggle-compact",       run_toggle_compact,       validate_toggle_compact       },
        { @"toggle-pane",          run_toggle_pane,          validate_toggle_pane          },
        { @"mark-done",            run_set_flag,             validate_selected             },
        { @"set-priority",         run_set_flag,             validate_selected             },
        { @"add-tag",              run_add_tag,              validate_selected             },
    };
    *n = sizeof table / sizeof table[0];
    return table;
}
