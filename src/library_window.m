#import "library_window.h"
#import "day_planner.h"
#import "editor_window.h"
#import "kanban.h"
#import "look.h"
#import "lists_bar.h"
#import "task_list.h"
#import "ui_util.h"

#include "task_config.h"
#include "task_ops.h"
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
static NSString * const search_item_ident    = @"tasks.search";

/* The search field's words.  The placeholder says what is searched: the box
 * narrows the SELECTED view, it does not sweep the database. */
static NSString * const search_placeholder   = @"Search this view";
static NSString * const search_tooltip       =
    @"Search the selected view's task titles, notes and subtasks.\n"
    @"\"quoted words\" matches the phrase; -word leaves out tasks containing it.";

@interface TasksLibraryWindow () <NSWindowDelegate, NSToolbarDelegate,
                                  NSToolbarItemValidation, NSSearchFieldDelegate,
                                  NSSplitViewDelegate>
@property (nonatomic, strong) TasksListsBar *lists_bar;
@property (nonatomic, strong) TasksTaskList *task_list;
@property (nonatomic, strong) TasksKanban *kanban;
@property (nonatomic, strong) TasksDayPlanner *day_planner;
@property (nonatomic, strong) NSSplitView         *main_split;
@property (nonatomic)         BOOL                 day_planner_animating;
@property (nonatomic, strong) NSLayoutConstraint  *dp_min_width;
@property (nonatomic, strong) NSTextField *status_left;    /* "<view> - N tasks"        */
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
/* Timer that fires after tasks_look_status_hold to begin the fade; at most
 * one at a time (invalidated when the next message arrives). */
@property (nonatomic, strong) NSTimer *status_timer;
/* Subscriptions to the app's three events; 0 once dropped. */
@property (nonatomic, assign) guint listen_changed;
@property (nonatomic, assign) guint listen_tasks;
@property (nonatomic, assign) guint listen_status;
/* Whether the sidebar had a Favorites row at its last rebuild: the light
 * task event rebuilds the sidebar only when that changes. */
@property (nonatomic, assign) BOOL had_pinned;
@property (nonatomic, assign) BOOL is_shut_down;

/* Declared here because the C listeners below call it before its
 * definition; documented where it is defined. */
- (void)show_status_message:(NSString *)message;
- (void)update_window_title;
@end

/* ── App-event listeners (C callbacks; user_data is the window) ─────── */

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
 * on_app_tasks — the app's light event: the task pane only.  One exception:
 * a favourite flag can add or remove the sidebar's Favorites row, so the
 * sidebar is rebuilt when that row's existence changed.
 *
 * Inputs:
 *   app  — the application context
 *   data — the TasksLibraryWindow, unretained
 */
static void
on_app_tasks(TaskApp *app, gpointer data)
{
    TasksLibraryWindow *window = (__bridge TasksLibraryWindow *)data;

    if ((BOOL)task_db_has_pinned(app->db) != window.had_pinned) [window refresh_sidebar];
    [window refresh_tasks];
    if (!window.day_planner.hidden) [window.day_planner reload];
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

/*
 * TasksSplitHost — the plain container of the window's main split view,
 * which keeps the split view exactly its own size.  The split view has no
 * autoresizing link to it (see build_layout), so it is sized here.
 */
@interface TasksSplitHost : NSView
@end

@implementation TasksSplitHost

/*
 * setFrameSize: — the container was resized: make the split view fill it.
 *
 * Inputs:
 *   size — the new size
 */
- (void)
setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    self.subviews.firstObject.frame = self.bounds;
}

/*
 * didAddSubview: — the split view arrived: make it fill the container.
 *
 * Inputs:
 *   view — the subview added
 */
- (void)
didAddSubview:(NSView *)view
{
    [super didAddSubview:view];
    view.frame = self.bounds;
}

@end

@implementation TasksLibraryWindow

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
    /* Whether the planner shows decides the window's least width, so it
     * is settled before the layout is applied. */
    self.day_planner.hidden = !task_config_get_bool(TASK_CFG_DAY_PLANNER_VISIBLE);
    /* The size first, then the place: placing keeps the window on a
     * screen, which depends on how big it is. */
    [self apply_compact_layout_at:size animated:NO];
    [self place_window];

    self.listen_changed = task_app_listen_changed(app, on_app_changed, (__bridge void *)self);
    self.listen_tasks   = task_app_listen_tasks(app, on_app_tasks, (__bridge void *)self);
    self.listen_status  = task_app_listen_status(app, on_app_status, (__bridge void *)self);

    [self apply_pane_mode];
    [self refresh_sidebar];
    [self update_window_title];
    [self refresh_tasks];

    if (!self.day_planner.hidden) {
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
 * status_label — one of the status bar's two labels.
 *
 * Inputs:
 *   alignment — left for the count, right for the event message
 *
 * Output:
 *   new label, small and secondary, truncating rather than wrapping.
 */
static NSTextField *
status_label(NSTextAlignment alignment)
{
    NSTextField *label = [NSTextField labelWithString:@""];

    label.font          = [NSFont systemFontOfSize:tasks_look_status_font];
    label.textColor     = NSColor.secondaryLabelColor;
    label.alignment     = alignment;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

/*
 * pin_edges — make a view fill its superview.
 *
 * Inputs:
 *   view — a view already added to its superview
 */
static void
pin_edges(NSView *view)
{
    NSView *host = view.superview;

    view.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [view.leadingAnchor constraintEqualToAnchor:host.leadingAnchor],
        [view.trailingAnchor constraintEqualToAnchor:host.trailingAnchor],
        [view.topAnchor constraintEqualToAnchor:host.topAnchor],
        [view.bottomAnchor constraintEqualToAnchor:host.bottomAnchor],
    ]];
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
    self.float_buttons = @[
        tasks_command_button(TasksCommandNewTask, self),
        tasks_command_button(TasksCommandDeleteTask, self),
    ];

    NSStackView *buttons = [NSStackView stackViewWithViews:self.float_buttons];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing     = tasks_look_float_pad;
    buttons.edgeInsets  = NSEdgeInsetsMake(tasks_look_float_pad, tasks_look_float_pad,
                                           tasks_look_float_pad, tasks_look_float_pad);

    /* The plate takes its size from the buttons pinned inside it. */
    self.float_bar = [[TasksFloatPlate alloc] initWithFrame:NSZeroRect];
    self.float_bar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.float_bar addSubview:buttons];
    pin_edges(buttons);

    [pane addSubview:self.float_bar positioned:NSWindowAbove relativeTo:nil];
    [NSLayoutConstraint activateConstraints:@[
        [self.float_bar.trailingAnchor constraintEqualToAnchor:pane.trailingAnchor
                                                      constant:-tasks_look_float_margin],
        [self.float_bar.bottomAnchor constraintEqualToAnchor:pane.bottomAnchor
                                                    constant:-tasks_look_float_margin],
    ]];
}

/*
 * build_layout — the window's content: a split view running from the
 * toolbar to the status bar's separator, its left half a lists bar over the
 * task pane and its right half the day planner, which therefore stands the
 * full height.  The task pane holds BOTH presentations, the list and the
 * board, of which apply_pane_mode shows one.  NSStackView collapses the bar
 * when hidden, so the pane fills the left half.
 */
- (void)
build_layout
{
    NSView *content = self.window.contentView;

    self.lists_bar = [[TasksListsBar alloc] init_with_library:self];
    self.task_list     = [[TasksTaskList alloc] init_with_library:self];
    self.kanban        = [[TasksKanban alloc] init_with_library:self];

    NSView *pane = [[NSView alloc] initWithFrame:NSZeroRect];
    [pane addSubview:self.task_list];
    [pane addSubview:self.kanban];
    pin_edges(self.task_list);
    pin_edges(self.kanban);
    [self build_float_bar:pane];

    [self.lists_bar.heightAnchor
        constraintEqualToConstant:tasks_look_lists_bar_h].active = YES;

    /* The split's left half: the lists bar over the task pane.  A plain
     * container owns the stack so the split view manages a frame-based
     * subview, as it does the day planner. */
    NSStackView *left_stack = [NSStackView stackViewWithViews:@[self.lists_bar, pane]];
    left_stack.orientation  = NSUserInterfaceLayoutOrientationVertical;
    left_stack.spacing      = 0;
    left_stack.distribution = NSStackViewDistributionFill;

    NSView *left = [[NSView alloc] initWithFrame:NSZeroRect];
    [left addSubview:left_stack];
    pin_edges(left_stack);

    self.day_planner = [[TasksDayPlanner alloc] init_with_library:self];

    NSSplitView *split = [[NSSplitView alloc] initWithFrame:NSZeroRect];
    split.vertical     = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    split.delegate     = self;
    [split addSubview:left];
    [split addSubview:self.day_planner];
    self.main_split = split;

    /* Minimum width for the day planner.  Stored so it can be deactivated
     * during open/close animations (a live constraint fights NSSplitView's
     * own frame management and desynchs the divider tracker). */
    self.dp_min_width = [self.day_planner.widthAnchor
        constraintGreaterThanOrEqualToConstant:tasks_look_day_planner_min_w];
    self.dp_min_width.active = YES;

    NSBox *rule   = [[NSBox alloc] initWithFrame:NSZeroRect];
    rule.boxType  = NSBoxSeparator;
    self.status_left  = status_label(NSTextAlignmentCenter);
    self.status_right = status_label(NSTextAlignmentRight);

    /* The split view sits in a container that sizes it by hand (TasksSplitHost)
     * and carries NO autoresizing link to it.  Pinned to the window by
     * constraints, or tied to its container by a flexible autoresizing mask,
     * NSSplitView turns its subviews' frames into required width
     * constraints, and the window then takes the size they add up to as its
     * least size: once the day planner was shown the window could grow but
     * never shrink again.  The container is constrained as the split view
     * used to be; the window's least size is content_min_size alone. */
    NSView *split_host = [[TasksSplitHost alloc] initWithFrame:NSZeroRect];
    split.translatesAutoresizingMaskIntoConstraints = YES;
    split.autoresizingMask = NSViewNotSizable;
    [split_host addSubview:split];

    for (NSView *v in @[split_host, rule, self.status_left, self.status_right]) {
        v.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:v];
    }
    [NSLayoutConstraint activateConstraints:@[
        [split_host.topAnchor constraintEqualToAnchor:content.topAnchor],
        [split_host.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [split_host.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [split_host.bottomAnchor constraintEqualToAnchor:rule.topAnchor],

        [rule.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [rule.heightAnchor constraintEqualToConstant:1.0],
        [rule.bottomAnchor constraintEqualToAnchor:content.bottomAnchor
                                          constant:-tasks_look_status_h],

        [self.status_left.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
        [self.status_left.leadingAnchor
            constraintGreaterThanOrEqualToAnchor:content.leadingAnchor
                                        constant:tasks_look_status_inset],
        [self.status_left.trailingAnchor
            constraintLessThanOrEqualToAnchor:self.status_right.leadingAnchor
                                     constant:-tasks_look_status_inset],
        [self.status_left.centerYAnchor constraintEqualToAnchor:content.bottomAnchor
                                                       constant:-tasks_look_status_h / 2.0],
        [self.status_right.trailingAnchor constraintEqualToAnchor:content.trailingAnchor
                                                         constant:-tasks_look_status_inset],
        [self.status_right.centerYAnchor constraintEqualToAnchor:self.status_left.centerYAnchor],
    ]];
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
        tasks_command_spec(TasksCommandNewTask)->ident,
        tasks_command_spec(TasksCommandDeleteTask)->ident,
        NSToolbarSpaceItemIdentifier,
        tasks_command_spec(TasksCommandToggleCompleted)->ident,
        tasks_command_spec(TasksCommandTogglePane)->ident,
        tasks_command_spec(TasksCommandToggleDayPlanner)->ident,
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
        item.preferredWidthForSearchField = tasks_look_search_w;
        [field.widthAnchor constraintLessThanOrEqualToConstant:tasks_look_search_w].active = YES;
        self.search_field = field;
        return item;
    }

    const tasks_command_spec_t *spec = tasks_command_with_ident(ident);
    return spec ? tasks_command_toolbar_item(spec->command, self) : nil;
}

/*
 * validateToolbarItem: — AppKit asks whether a toolbar button is enabled;
 * the answer also refreshes its icon and tooltip, which is how a toggling
 * button comes to show the action it now offers.
 *
 * Inputs:
 *   item — a command button made by tasks_command_toolbar_item
 *
 * Output:
 *   YES to enable it.
 */
- (BOOL)
validateToolbarItem:(NSToolbarItem *)item
{
    TasksCommandState *state = [self state_of_command:(TasksCommand)item.tag];

    tasks_command_apply(state, item);
    return state.enabled;
}

/*
 * acts_on_group — whether a command's parameter is the group it acts on.
 * Such a command sent without one (from the main menu) means the group
 * selected in the sidebar (commands.h).
 *
 * Inputs:
 *   command — the command
 *
 * Output:
 *   YES for Rename Group... and Remove Group.
 */
static BOOL
acts_on_group(TasksCommand command)
{
    return command == TasksCommandRenameGroup || command == TasksCommandDeleteGroup;
}

/*
 * validateMenuItem: — AppKit asks whether a command's menu item is enabled;
 * the answer also sets its title and check mark.  An item carrying a
 * parameter keeps the title it was built with (commands.h).  A group
 * command carrying none needs a group to be selected.
 *
 * Inputs:
 *   item — a command item made by tasks_command_menu_item
 *
 * Output:
 *   YES to enable it.
 */
- (BOOL)
validateMenuItem:(NSMenuItem *)item
{
    TasksCommand       command = (TasksCommand)item.tag;
    TasksCommandState *state   = [self state_of_command:command];

    if (item.representedObject) {
        state.title = item.title;
    } else if (acts_on_group(command) && self.selection.kind != task_sel_group) {
        state.enabled = NO;
    }
    tasks_command_apply(state, item);
    return state.enabled;
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
 * "Tasks — Name" for a list, group or view; the plain app name
 * otherwise.
 */
- (void)
update_window_title
{
    NSString   *suffix = nil;
    task_sel_t  sel    = _selection;

    if (sel.kind == task_sel_list) {
        TaskList *list = task_db_list_get(self.app->db, sel.id);
        if (list) { suffix = tasks_string(list->name); task_list_free(list); }
    } else if (sel.kind == task_sel_group) {
        TaskGroup *group = task_db_group_get(self.app->db, sel.id);
        if (group) { suffix = tasks_string(group->name); task_group_free(group); }
    } else if (sel.kind == task_sel_view) {
        const TaskView *view = task_view_nth((guint)sel.id);
        if (view) suffix = tasks_string(view->label);
    }

    self.window.title = suffix
        ? [@TASKS_APP_NAME stringByAppendingFormat:@" \xe2\x80\x94 %@", suffix]
        : @TASKS_APP_NAME;
}

/*
 * select: — the lists bar selection changed.
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
        [self.lists_bar update_selection];
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
 * refresh_sidebar — rebuild the lists bar from the database.
 */
- (void)
refresh_sidebar
{
    if (self.is_shut_down) return;

    [self.lists_bar reload];
    self.had_pinned = task_db_has_pinned(self.app->db);
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
    if (!self.day_planner.hidden) [self.day_planner reload];
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
 * for tasks_look_status_hold, then fade it out over tasks_look_status_fade
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
    self.status_timer = [NSTimer scheduledTimerWithTimeInterval:tasks_look_status_hold
                                                        repeats:NO
                                                          block:^(NSTimer *t) {
        TasksLibraryWindow *s = ws;
        if (!s || s.status_message_id != message_id) return;
        s.status_timer = nil;
        [s set_status_alpha:0.0 over:tasks_look_status_fade then:^{
            TasksLibraryWindow *faded = ws;
            if (!faded || faded.status_message_id != message_id) return;
            faded.status_right.stringValue = @"";
        }];
    }];
}

/* ── Split view delegate ─────────────────────────────────────────────── */

/*
 * splitView:constrainMinCoordinate:ofSubviewAt: — the left (task pane)
 * minimum width.
 *
 * Inputs:
 *   split — the main split view
 *   coord — AppKit's proposed minimum (unused)
 *   idx   — divider index (0)
 *
 * Output:
 *   the minimum X coordinate of the divider.
 */
- (CGFloat)
splitView:(NSSplitView *)split
constrainMinCoordinate:(CGFloat)coord
ofSubviewAt:(NSInteger)idx
{
    return tasks_look_pane_min_w;
}

/*
 * splitView:constrainMaxCoordinate:ofSubviewAt: — the day planner minimum
 * width enforced as a maximum divider position.
 *
 * Inputs:
 *   split — the main split view
 *   coord — AppKit's proposed maximum (unused)
 *   idx   — divider index (0)
 *
 * Output:
 *   the maximum X coordinate of the divider.
 */
- (CGFloat)
splitView:(NSSplitView *)split
constrainMaxCoordinate:(CGFloat)coord
ofSubviewAt:(NSInteger)idx
{
    if (self.day_planner_animating)
        return NSWidth(split.bounds) - split.dividerThickness;
    return NSWidth(split.bounds) - tasks_look_day_planner_min_w - split.dividerThickness;
}

/*
 * splitView:resizeSubviewsWithOldSize: — keep the day planner at its current
 * width when the window is resized; the task pane absorbs the delta.
 *
 * Inputs:
 *   split   — the main split view
 *   old_sz  — the split view's previous size (unused; we use current bounds)
 */
- (void)
splitView:(NSSplitView *)split
resizeSubviewsWithOldSize:(NSSize)old_sz
{
    NSView  *left   = split.subviews[0];
    NSView  *right  = split.subviews[1];
    CGFloat  total  = NSWidth(split.bounds);
    CGFloat  div_t  = split.dividerThickness;

    if (right.hidden) {
        left.frame = NSMakeRect(0, 0, total, NSHeight(split.bounds));
        return;
    }

    CGFloat right_w = NSWidth(right.frame);
    CGFloat left_w  = total - right_w - div_t;

    /* Clamp so neither side goes below its minimum. */
    if (left_w < tasks_look_pane_min_w) {
        left_w  = tasks_look_pane_min_w;
        right_w = total - left_w - div_t;
    }
    if (right_w < tasks_look_day_planner_min_w) {
        right_w = tasks_look_day_planner_min_w;
        left_w  = total - right_w - div_t;
    }

    left.frame  = NSMakeRect(0,              0, left_w,  NSHeight(split.bounds));
    right.frame = NSMakeRect(left_w + div_t, 0, right_w, NSHeight(split.bounds));
}

/* ── Task context menu ───────────────────────────────────────────────── */

/*
 * titled_item — a command item wearing its own title (a destination list
 * or group).
 *
 * Inputs:
 *   command — the command
 *   param   — its parameter; not nil, which is also what makes validation
 *             keep the title
 *   title   — the item's title
 *
 * Output:
 *   new menu item targeting this window.
 */
- (NSMenuItem *)
titled_item:(TasksCommand)command param:(NSNumber *)param title:(NSString *)title
{
    NSMenuItem *item = tasks_command_menu_item(command, param, self);

    item.title = title;
    return item;
}

/*
 * add_list_destinations: — the lists the selected tasks can move to: every
 * list, less the one a single selected task is already in.
 *
 * Inputs:
 *   menu — the submenu to add to
 */
- (void)
add_list_destinations:(NSMenu *)menu
{
    NSArray<NSNumber *> *ids = [self selected_task_ids];
    Task      *only  = ids.count == 1 ? task_db_task_get(self.app->db, ids[0].longLongValue) : NULL;
    GPtrArray *lists = task_db_lists(self.app->db, FALSE);

    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        if (only != NULL && list->id == only->list_id) continue;
        NSString *title = *list->emoji != '\0'
            ? [NSString stringWithFormat:@"%@  %@", tasks_string(list->emoji), tasks_string(list->name)]
            : tasks_string(list->name);
        [menu addItem:[self titled_item:TasksCommandMoveToList param:@(list->id) title:title]];
    }
    task_ptr_array_free_lists(lists);
    task_free(only);
}

/*
 * add_group_destinations: — the groups the selected list can move to.
 *
 * Inputs:
 *   menu — the submenu to add to
 */
- (void)
add_group_destinations:(NSMenu *)menu
{
    GPtrArray *groups = task_db_groups(self.app->db);

    for (guint i = 0; i < groups->len; i++) {
        const TaskGroup *group = g_ptr_array_index(groups, i);
        [menu addItem:[self titled_item:TasksCommandMoveToGroup param:@(group->id)
                                  title:tasks_string(group->name)]];
    }
    task_ptr_array_free_groups(groups);
}

/*
 * fill_menu:with_destinations_of: — rebuild a "Move to" submenu.
 * Inputs are documented in library_window.h.
 */
- (void)
fill_menu:(NSMenu *)menu with_destinations_of:(TasksCommand)command
{
    [menu removeAllItems];
    if (self.is_shut_down) return;

    if (command == TasksCommandMoveToList) {
        [self add_list_destinations:menu];
    } else if (command == TasksCommandMoveToGroup) {
        [self add_group_destinations:menu];
    }
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
        [menu addItem:tasks_command_menu_item(TasksCommandTaskInfo, nil, self)];
        [menu addItem:NSMenuItem.separatorItem];
    }

    if (task != NULL) {
        [menu addItem:tasks_command_flag_item(TasksCommandMarkDone,
                                              task->status != TASK_STATUS_DONE, NO, self)];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:tasks_command_flag_item(TasksCommandSetPinned, !task->pinned, NO, self)];
        [menu addItem:tasks_command_flag_item(TasksCommandSetPriority, !task->priority, NO, self)];
    } else {
        [menu addItem:tasks_command_flag_item(TasksCommandMarkDone, YES, YES, self)];
        [menu addItem:tasks_command_flag_item(TasksCommandMarkDone, NO, YES, self)];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:tasks_command_flag_item(TasksCommandSetPinned, YES, YES, self)];
        [menu addItem:tasks_command_flag_item(TasksCommandSetPinned, NO, YES, self)];
        [menu addItem:tasks_command_flag_item(TasksCommandSetPriority, YES, YES, self)];
        [menu addItem:tasks_command_flag_item(TasksCommandSetPriority, NO, YES, self)];
    }
    [menu addItem:NSMenuItem.separatorItem];

    /* Move to List: left out for a lone subtask (subtasks travel with their
     * parent) and when there is nowhere to move to. */
    if (task == NULL || task->parent_id == 0) {
        NSMenu *move = [[NSMenu alloc] initWithTitle:@""];
        [self fill_menu:move with_destinations_of:TasksCommandMoveToList];
        if (move.numberOfItems > 0) {
            NSMenuItem *holder = [[NSMenuItem alloc]
                initWithTitle:tasks_command_spec(TasksCommandMoveToList)->title
                       action:NULL
                keyEquivalent:@""];
            holder.submenu = move;
            [menu addItem:holder];
            [menu addItem:NSMenuItem.separatorItem];
        }
    }

    [menu addItem:tasks_command_menu_item(TasksCommandDeleteTask, nil, self)];
    task_free(task);
    return menu;
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * run_command: — the action every command control sends.
 * Inputs are documented in library_window.h.
 *
 * A group command whose control names no group (the main menu's) is given
 * the group selected in the sidebar; validateMenuItem: has it disabled
 * when there is none.
 */
- (void)
run_command:(id)sender
{
    TasksCommand command = (TasksCommand)[sender tag];
    NSNumber    *param   = [sender isKindOfClass:NSMenuItem.class]
        ? ((NSMenuItem *)sender).representedObject : nil;

    if (param == nil && acts_on_group(command)) {
        if (self.selection.kind != task_sel_group) return;
        param = @(self.selection.id);
    }
    [self perform_command:command param:param];
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
 * new_task — create an empty task in the selected list and open its editor
 * (the variant with Cancel, which deletes the task again).  A view or a
 * group has no one list to create it in, and the status bar says which.
 */
- (void)
new_task
{
    int64_t list_id = task_sel_list_id(self.selection);
    if (list_id == 0) {
        task_app_status(self.app, "Select a list first \xe2\x80\x94 %s",
                        self.selection.kind == task_sel_group
                            ? "a group has no one list to create the task in"
                            : "tasks cannot be created in the virtual views");
        return;
    }

    int64_t task_id = task_db_task_create(self.app->db, list_id, 0, "New Task");
    if (task_id == 0) {
        task_app_status(self.app, "Could not create the task");
        return;
    }
    [self full_refresh];
    tasks_editor_open_new(self.app, task_id);
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
    tasks_ui_confirm(self.window, title, @"You cannot undo this action.", @"Delete", YES,
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
 * clear_completed — ask, then remove the selected list's completed tasks.
 */
- (void)
clear_completed
{
    int64_t list_id = task_sel_list_id(self.selection);
    TaskList *list  = list_id != 0 ? task_db_list_get(self.app->db, list_id) : NULL;
    if (list == NULL) {
        task_app_status(self.app, "Select a list to clear its completed tasks");
        return;
    }
    NSString *title = [NSString stringWithFormat:@"Remove all completed tasks from “%@”?",
                       tasks_string(list->name)];
    task_list_free(list);

    __weak TasksLibraryWindow *ws = self;
    tasks_ui_confirm(self.window, title, @"You cannot undo this action.", @"Clear", YES,
                     ^(BOOL confirmed) {
        TasksLibraryWindow *s = ws;
        if (!s || s.is_shut_down || !confirmed) return;
        guint cleared = task_ops_clear_completed(s.app, list_id);
        [s full_refresh];
        task_app_status(s.app, "Cleared %u completed task%s", cleared, plural(cleared));
    });
}

/*
 * set_flag:on: — apply one of the context menu's flags to every selected
 * task, then refresh everything (the Favorites row and the sort order can
 * both change) and say what was done — or that it was not saved.
 *
 * Inputs:
 *   command — TasksCommandMarkDone, TasksCommandSetPinned or
 *             TasksCommandSetPriority
 *   on      — the direction.  For MarkDone: YES is Done, NO is In Progress
 *             (the checkbox's two verbs — a task that was ticked has been
 *             worked on, so it does not go back to New)
 */
- (void)
set_flag:(TasksCommand)command on:(BOOL)on
{
    NSArray<NSNumber *> *ids    = [self selected_task_ids];
    unsigned long        failed = 0;

    for (NSNumber *task_id in ids) {
        int64_t  id = task_id.longLongValue;
        gboolean ok;
        if (command == TasksCommandMarkDone) {
            ok = task_db_task_set_status(self.app->db, id,
                                         on ? TASK_STATUS_DONE : TASK_STATUS_IN_PROGRESS);
        } else if (command == TasksCommandSetPinned) {
            ok = task_db_task_set_pinned(self.app->db, id, on);
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
    } else if (command == TasksCommandMarkDone) {
        task_app_status(self.app, "Marked %lu task%s %s", count, plural(count),
                        on ? "complete" : "incomplete");
    } else if (command == TasksCommandSetPinned) {
        task_app_status(self.app, "%s %lu task%s",
                        on ? "Added to Favorites" : "Removed from Favorites", count, plural(count));
    } else {
        task_app_status(self.app, "%s high priority on %lu task%s", on ? "Set" : "Cleared",
                        count, plural(count));
    }
}

/*
 * move_to_list: — move every selected top-level task to another list.
 * Subtasks travel with their parents; one selected on its own cannot move.
 *
 * Inputs:
 *   list_id — the destination list
 */
- (void)
move_to_list:(int64_t)list_id
{
    NSUInteger moved = 0;

    for (NSNumber *task_id in [self selected_task_ids]) {
        if (task_ops_move_to_list(self.app, task_id.longLongValue, list_id)) moved++;
    }
    if (moved == 0) {
        task_app_status(self.app, "Nothing to move (subtasks move with their parent task)");
        return;
    }
    [self full_refresh];
    task_app_status(self.app, "Moved %lu task%s", (unsigned long)moved, plural(moved));
}

/*
 * toggle_sidebar — slide the lists bar in or out and remember the choice.
 */
- (void)
toggle_sidebar
{
    BOOL show = self.lists_bar.hidden;

    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.15;
        self.lists_bar.animator.hidden = !show;
    }];
    task_config_set_bool(TASK_CFG_SIDEBAR_VISIBLE, show);
}

/*
 * content_min_size — the least content size of the mode in force: the task
 * pane alone in Compact; in full mode the pane plus the day planner when
 * that is shown.
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
    if (!self.day_planner.hidden) {
        width += self.main_split.dividerThickness + tasks_look_day_planner_min_w;
    }
    return NSMakeSize(width, tasks_look_window_min_h);
}

/*
 * toggle_day_planner — slide the day planner panel in or out and remember
 * the choice.  Animates by moving the NSSplitView divider so the layout
 * change and the visual reveal happen in a single pass.  The window's
 * least width follows: the planner's minimum counts only while it shows.
 */
- (void)
toggle_day_planner
{
    BOOL show = self.day_planner.hidden; /* YES → we are about to show it */

    __weak TasksLibraryWindow *ws  = self;
    CGFloat split_w = NSWidth(self.main_split.bounds);
    CGFloat div_t   = self.main_split.dividerThickness;

    /* Suspend the min-width constraint for the duration of the animation.
     * NSSplitView and Auto Layout both set subview frames; a live required
     * constraint desynchs NSSplitView's internal divider tracker, leaving
     * dragging broken until the next window resize forces a re-sync. */
    self.dp_min_width.active = NO;

    if (show) {
        /* Reveal: unhide at zero width then slide the divider in from the
         * right.  Delegate is cleared just for the instant start-position
         * call so the max-coordinate constraint doesn't clamp it. */
        self.day_planner.hidden     = NO;
        self.day_planner_animating  = YES;
        self.window.contentMinSize  = [self content_min_size];

        id saved = self.main_split.delegate;
        self.main_split.delegate = nil;
        [self.main_split setPosition:split_w - div_t ofDividerAtIndex:0];
        self.main_split.delegate = saved;

        gint saved_w = task_config_get_int(TASK_CFG_DAY_PLANNER_W);
        CGFloat dp_w = ((CGFloat)saved_w >= tasks_look_day_planner_min_w)
                       ? (CGFloat)saved_w : tasks_look_day_planner_w;
        CGFloat target = split_w - dp_w - div_t;
        if (target < tasks_look_pane_min_w) target = tasks_look_pane_min_w;

        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.22;
            [[ws.main_split animator] setPosition:target ofDividerAtIndex:0];
        } completionHandler:^{
            ws.day_planner_animating  = NO;
            ws.dp_min_width.active    = YES;
            [ws.day_planner reload];
            [ws.day_planner scroll_to_now];
        }];
    } else {
        /* Hide: save the current width, slide the divider to the right
         * edge, then set hidden (instant at that point, no visible snap). */
        task_config_set_int(TASK_CFG_DAY_PLANNER_W, (gint)NSWidth(self.day_planner.frame));

        self.day_planner_animating = YES;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
            ctx.duration = 0.22;
            [[ws.main_split animator] setPosition:split_w - div_t ofDividerAtIndex:0];
        } completionHandler:^{
            ws.day_planner_animating  = NO;
            ws.day_planner.hidden     = YES;
            ws.dp_min_width.active    = YES;
            ws.window.contentMinSize  = [ws content_min_size];
            [ws.main_split adjustSubviews];
        }];
    }

    task_config_set_bool(TASK_CFG_DAY_PLANNER_VISIBLE, show);
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
 * In full mode the sidebar follows the sidebar_visible setting; in Compact
 * mode it is always hidden.  The resize keeps the window's top-left corner
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
    BOOL   collapse = compact || !task_config_get_bool(TASK_CFG_SIDEBAR_VISIBLE);
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
        self.lists_bar.hidden = collapse;
        [self.window setFrame:frame display:NO];
        return;
    }

    self.compact_animating = YES;
    __weak TasksLibraryWindow *ws = self;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        self.lists_bar.animator.hidden = collapse;
        [self.window.animator setFrame:frame display:YES];
    } completionHandler:^{
        ws.compact_animating = NO;
    }];
}

/*
 * toggle_compact — View > Compact Controls / Full Controls: switch modes.
 * Each mode has its own remembered window size: the size being left is
 * saved for the next time that mode is entered, and the window goes to the
 * size remembered for the mode being entered (compact_w/h for Compact,
 * compact_pre_w/h — the size it had before going Compact — for full).
 * Leaving Compact always brings the sidebar back, and records that.
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
        tasks_command_apply([self state_of_command:(TasksCommand)button.tag], button);
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
    tasks_ui_notice(self.window, @"Database Integrity Check",
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
    tasks_ui_confirm(self.window, @"Make this the default database?",
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
            tasks_ui_notice(s.window, @"Could not make it the default",
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
    tasks_ui_notice(self.window, @"Could not open the database",
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
    tasks_ui_pick_path(self.window, TasksPickFile, @"Choose a shared Notes and Tasks database to open.",
                       @"Open", @[@"db"], nil, ^(NSString *path) {
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
 * perform_shell_command:param: — carry out one of the window's own
 * commands.  Every toggle FLIPS its flag: the control that sent it shows
 * the action, not the state, so there is nothing to read back from it.
 *
 * Inputs:
 *   command — a TasksCommandOwnerShell command
 *   param   — its parameter, or nil
 */
- (void)
perform_shell_command:(TasksCommand)command param:(NSNumber *)param
{
    switch (command) {
    case TasksCommandNewTask:
        [self new_task];
        break;
    case TasksCommandDeleteTask:
        [self delete_tasks];
        break;
    case TasksCommandTaskInfo:
        if ([self selected_task_ids].count > 0) {
            tasks_editor_open(self.app, [self selected_task_ids][0].longLongValue);
        }
        break;
    case TasksCommandClearCompleted:
        [self clear_completed];
        break;
    case TasksCommandOpenDatabase:
        [self pick_database];
        break;
    case TasksCommandFind:
        [self.window makeFirstResponder:self.search_field];
        break;
    case TasksCommandToggleCompleted:
        task_config_set_bool(TASK_CFG_SHOW_COMPLETED,
                             !task_config_get_bool(TASK_CFG_SHOW_COMPLETED));
        [self refresh_tasks];
        break;
    case TasksCommandToggleSidebar:
        [self toggle_sidebar];
        break;
    case TasksCommandToggleDayPlanner:
        [self toggle_day_planner];
        break;
    case TasksCommandToggleCompact:
        [self toggle_compact];
        break;
    case TasksCommandTogglePane:
        _kanban_on = !_kanban_on;
        task_config_set_bool(TASK_CFG_KANBAN_VIEW, _kanban_on);
        [self apply_pane_mode];
        [self refresh_tasks];
        /* The keyboard follows the pane: the one it was in is now hidden. */
        [self focus_task_pane];
        break;
    case TasksCommandMarkDone:
    case TasksCommandSetPinned:
    case TasksCommandSetPriority:
        [self set_flag:command on:param.boolValue];
        break;
    case TasksCommandMoveToList:
        [self move_to_list:param.longLongValue];
        break;
    default:
        break;                      /* not one of the window's own commands */
    }
    [self.window.toolbar validateVisibleItems];
}

/*
 * perform_command:param: — dispatch a command to its owner.
 * Inputs are documented in library_window.h.
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
    const tasks_command_spec_t *spec = tasks_command_spec(command);
    if (spec == NULL || self.is_shut_down) return;

    switch (spec->owner) {
    case TasksCommandOwnerShell:
        [self perform_shell_command:command param:param];
        break;
    case TasksCommandOwnerSidebar:
        [self.lists_bar perform_command:command param:param];
        break;
    case TasksCommandOwnerTaskList:
        [self.task_list perform_command:command param:param];
        break;
    case TasksCommandOwnerApp:
        break;                      /* the app delegate's; never sent here */
    }
}

/*
 * update_shell_state:for_command: — the current look of one of the
 * window's own commands.
 *
 * Inputs:
 *   state   — the table's default state, adjusted in place
 *   command — a TasksCommandOwnerShell command
 */
- (void)
update_shell_state:(TasksCommandState *)state for_command:(TasksCommand)command
{
    NSUInteger selected = [self selected_task_ids].count;

    switch (command) {
    case TasksCommandDeleteTask:
        /* Not while text is being edited, in any window: its shortcut is
         * also the text system's "delete to the start of the line", and a
         * disabled menu item lets the key through to the field. */
        state.enabled = selected > 0 &&
                        ![NSApp.keyWindow.firstResponder isKindOfClass:NSText.class];
        if (selected > 1) {
            state.title = [NSString stringWithFormat:@"Delete %lu Tasks", (unsigned long)selected];
        }
        break;
    case TasksCommandTaskInfo:
        state.enabled = selected == 1;
        break;
    case TasksCommandMarkDone:
    case TasksCommandSetPinned:
    case TasksCommandSetPriority:
    case TasksCommandMoveToList:
        state.enabled = selected > 0;
        break;
    case TasksCommandClearCompleted:
        state.enabled = task_sel_list_id(self.selection) != 0;
        break;
    /* The toggles: the command table holds each one's first face; only the
     * other face is spelled here. */
    case TasksCommandToggleCompleted:
        if (!task_config_get_bool(TASK_CFG_SHOW_COMPLETED)) {
            state.title   = @"Show Completed";
            state.icon    = @"eye";
            state.tooltip = @"Show completed tasks";
        }
        break;
        break;
    case TasksCommandToggleSidebar:
        if (self.lists_bar.hidden) {
            state.title   = @"Show Lists Bar";
            state.tooltip = @"Show the lists bar";
        }
        break;
    case TasksCommandToggleDayPlanner:
        if (self.day_planner.hidden) {
            state.title   = @"Show Day Planner";
            state.tooltip = @"Show the day planner";
        }
        break;
    case TasksCommandTogglePane:
        if (self.kanban_on) {
            state.title   = @"List View";
            state.icon    = @"list.bullet";
            state.tooltip = @"Show the tasks as a list";
        }
        break;
    case TasksCommandToggleCompact:
        if (task_config_get_bool(TASK_CFG_COMPACT)) state.title = @"Full Controls";
        /* A full-screen window has no size of its own to shrink or restore. */
        state.enabled = (self.window.styleMask & NSWindowStyleMaskFullScreen) == 0;
        break;
    case TasksCommandFind:
        /* Compact mode hides the search field with the toolbar. */
        state.enabled = !task_config_get_bool(TASK_CFG_COMPACT);
        break;
    default:
        break;                      /* New Task, Open Database: always available */
    }
}

/*
 * state_of_command: — how a command looks right now.
 * Inputs/output are documented in library_window.h.
 */
- (TasksCommandState *)
state_of_command:(TasksCommand)command
{
    TasksCommandState *state = tasks_command_default_state(command);
    const tasks_command_spec_t *spec = tasks_command_spec(command);

    if (spec == NULL || self.is_shut_down) {
        state.enabled = NO;
        return state;
    }
    switch (spec->owner) {
    case TasksCommandOwnerShell:
        [self update_shell_state:state for_command:command];
        break;
    case TasksCommandOwnerSidebar:
        [self.lists_bar update_state:state for_command:command];
        break;
    case TasksCommandOwnerTaskList:
        [self.task_list update_state:state for_command:command];
        break;
    case TasksCommandOwnerApp:
        break;
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
    task_app_unlisten(self.app, self.listen_status);
    self.listen_changed = self.listen_tasks = self.listen_status = 0;
    self.status_message_id++;       /* a pending status fade finds nothing to do */
    [self.status_timer invalidate];
    self.status_timer = nil;

    [self.task_list cancel_drag];
    [self.kanban cancel_drag];
    tasks_editor_close_all();

    [self.lists_bar shut_down];
    [self.task_list shut_down];
    [self.kanban shut_down];
    [self.day_planner shut_down];

    task_search_free((TaskSearch *)_search);
    _search = NULL;
}

@end
