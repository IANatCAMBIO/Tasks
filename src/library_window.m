#import "library_window.h"
#import "editor_window.h"
#import "kanban.h"
#import "look.h"
#import "sidebar.h"
#import "task_list.h"
#import "ui_util.h"

#include "task_config.h"
#include "task_ops.h"

/*
 * FOUNDATION SKELETON of the shell (work package "shell": see
 * porting/INTERFACES.md).  What is here is real and is the contract the
 * panes are built against: the shared state and its single writers, the
 * refresh orchestration, the command dispatch and validation, the event
 * listeners and the teardown order.  What the shell's owner still has to
 * add is listed in INTERFACES.md (Compact mode, Open Database, the status
 * message's fade, the first-run choice); the two commands not yet carried
 * out are disabled, never silently inert.
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

/* Initial sidebar width, until the sidebar fits itself to its content. */
static const CGFloat sidebar_initial_w = 200.0;

@interface TasksLibraryWindow () <NSWindowDelegate, NSToolbarDelegate,
                                  NSToolbarItemValidation, NSSearchFieldDelegate>
@property (nonatomic, strong) NSSplitViewController *split_vc;   /* sidebar | task pane */
@property (nonatomic, strong) NSSplitViewItem *sidebar_item;
@property (nonatomic, strong) TasksSidebar *sidebar;
@property (nonatomic, strong) TasksTaskList *task_list;
@property (nonatomic, strong) TasksKanban *kanban;
@property (nonatomic, strong) NSTextField *status_left;    /* "<view> - N tasks"        */
@property (nonatomic, strong) NSTextField *status_right;   /* the latest event message  */
@property (nonatomic, weak) NSSearchField *search_field;   /* owned by its toolbar item */
/* Bumped by every status message, so only the latest one's timer clears it. */
@property (nonatomic, assign) NSUInteger status_message_id;
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

@implementation TasksLibraryWindow

/* ── Construction ────────────────────────────────────────────────────── */

/*
 * init_with_app: — build the window and show the default selection.
 * Inputs/output are documented in library_window.h.
 */
- (instancetype)
init_with_app:(TaskApp *)app
{
    NSRect content = NSMakeRect(0, 0, task_config_get_int(TASK_CFG_WIN_W),
                                task_config_get_int(TASK_CFG_WIN_H));
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:content
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _app         = app;
    _selection   = task_sel_default();
    _manual_sort = task_config_get_bool(TASK_CFG_MANUAL_SORT);
    _kanban_on   = task_config_get_bool(TASK_CFG_KANBAN_VIEW);

    window.title        = @TASKS_APP_NAME;
    window.minSize      = NSMakeSize(tasks_look_window_min_w, tasks_look_window_min_h);
    window.toolbarStyle = NSWindowToolbarStyleExpanded;
    window.delegate     = self;
    [self place_window];
    [self build_layout];
    [self build_toolbar];

    self.listen_changed = task_app_listen_changed(app, on_app_changed, (__bridge void *)self);
    self.listen_tasks   = task_app_listen_tasks(app, on_app_tasks, (__bridge void *)self);
    self.listen_status  = task_app_listen_status(app, on_app_status, (__bridge void *)self);

    [self apply_pane_mode];
    [self refresh_sidebar];
    [self refresh_tasks];
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
 * build_layout — the window's content: a split view (sidebar | task pane)
 * over a separator and the status bar.  The task pane holds BOTH
 * presentations, the list and the board, of which apply_pane_mode shows
 * one; neither is ever reparented.
 */
- (void)
build_layout
{
    NSView *content = self.window.contentView;

    self.sidebar   = [[TasksSidebar alloc] init_with_library:self];
    self.task_list = [[TasksTaskList alloc] init_with_library:self];
    self.kanban    = [[TasksKanban alloc] init_with_library:self];

    NSView *pane = [[NSView alloc] initWithFrame:NSZeroRect];
    [pane addSubview:self.task_list];
    [pane addSubview:self.kanban];
    pin_edges(self.task_list);
    pin_edges(self.kanban);

    /* Bare view controllers wrap the existing views; a split view
     * controller gives collapse, minimum thickness and animation. */
    NSViewController *sidebar_vc = [[NSViewController alloc] init];
    sidebar_vc.view = self.sidebar;
    NSViewController *pane_vc = [[NSViewController alloc] init];
    pane_vc.view = pane;

    self.sidebar_item = [NSSplitViewItem splitViewItemWithViewController:sidebar_vc];
    self.sidebar_item.canCollapse      = YES;
    self.sidebar_item.minimumThickness = tasks_look_sidebar_min_w;
    /* Window resizes go to the task pane, not the sidebar. */
    self.sidebar_item.holdingPriority  = NSLayoutPriorityDefaultLow + 1;
    self.sidebar_item.collapsed        = !task_config_get_bool(TASK_CFG_SIDEBAR_VISIBLE);

    self.split_vc = [[NSSplitViewController alloc] init];
    self.split_vc.splitView.vertical     = YES;
    self.split_vc.splitView.dividerStyle = NSSplitViewDividerStyleThin;
    [self.split_vc addSplitViewItem:self.sidebar_item];
    [self.split_vc addSplitViewItem:[NSSplitViewItem splitViewItemWithViewController:pane_vc]];

    NSView *split = self.split_vc.view;
    NSBox  *rule  = [[NSBox alloc] initWithFrame:NSZeroRect];
    rule.boxType  = NSBoxSeparator;
    self.status_left  = status_label(NSTextAlignmentLeft);
    self.status_right = status_label(NSTextAlignmentRight);

    for (NSView *view in @[split, rule, self.status_left, self.status_right]) {
        view.translatesAutoresizingMaskIntoConstraints = NO;
        [content addSubview:view];
    }
    [NSLayoutConstraint activateConstraints:@[
        [split.topAnchor constraintEqualToAnchor:content.topAnchor],
        [split.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [split.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [split.bottomAnchor constraintEqualToAnchor:rule.topAnchor],

        [rule.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [rule.heightAnchor constraintEqualToConstant:1.0],
        [rule.bottomAnchor constraintEqualToAnchor:content.bottomAnchor
                                          constant:-tasks_look_status_h],

        [self.status_left.leadingAnchor constraintEqualToAnchor:content.leadingAnchor
                                                       constant:tasks_look_status_inset],
        [self.status_left.centerYAnchor constraintEqualToAnchor:content.bottomAnchor
                                                       constant:-tasks_look_status_h / 2.0],
        [self.status_right.trailingAnchor constraintEqualToAnchor:content.trailingAnchor
                                                         constant:-tasks_look_status_inset],
        [self.status_right.centerYAnchor constraintEqualToAnchor:self.status_left.centerYAnchor],
        [self.status_right.leadingAnchor
            constraintGreaterThanOrEqualToAnchor:self.status_left.trailingAnchor
                                        constant:tasks_look_status_inset],
        [self.sidebar.widthAnchor constraintGreaterThanOrEqualToConstant:tasks_look_sidebar_min_w],
    ]];

    gint saved_w = task_config_get_int(TASK_CFG_SIDEBAR_WIDTH);
    [self.split_vc.splitView setPosition:saved_w > 0 ? saved_w : sidebar_initial_w
                        ofDividerAtIndex:0];
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
 * toolbar_idents — the toolbar's items, left to right.  The task verbs
 * lead; the space separates what ACTS on a task from what changes how the
 * pane is SHOWN.
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
        tasks_command_spec(TasksCommandToggleSidebar)->ident,
        tasks_command_spec(TasksCommandToggleCompleted)->ident,
        tasks_command_spec(TasksCommandToggleSort)->ident,
        tasks_command_spec(TasksCommandTogglePane)->ident,
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
 * validateMenuItem: — AppKit asks whether a command's menu item is enabled;
 * the answer also sets its title and check mark.  An item carrying a
 * parameter keeps the title it was built with (commands.h).
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
    TasksCommandState *state = [self state_of_command:(TasksCommand)item.tag];

    if (item.representedObject) state.title = item.title;
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

    control.stringValue = @"";
    [self search_changed:(NSSearchField *)control];
    [self.window makeFirstResponder:self.kanban_on ? self.kanban : self.task_list];
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
    return self.manual_sort && self.search == NULL;
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
 * select: — the sidebar selection changed.
 * Inputs are documented in library_window.h.
 */
- (void)
select:(task_sel_t)selection
{
    if (selection.kind != _selection.kind || selection.id != _selection.id) {
        _selection = selection;
        [self.task_list view_changed];
        [self.kanban view_changed];
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
 * refresh_sidebar — rebuild the sidebar from the database.
 */
- (void)
refresh_sidebar
{
    if (self.is_shut_down) return;

    [self.sidebar reload];
    self.had_pinned = task_db_has_pinned(self.app->db);
}

/*
 * full_refresh — the sidebar, the task pane and every open editor.
 */
- (void)
full_refresh
{
    [self refresh_sidebar];
    [self refresh_tasks];
    tasks_editor_refresh_all();
}

/*
 * show_status_message: — put an event message in the status bar and clear
 * it after the hold time, unless a newer message has replaced it.
 *
 * Inputs:
 *   message — the message
 */
- (void)
show_status_message:(NSString *)message
{
    NSUInteger message_id = ++self.status_message_id;
    self.status_right.stringValue = message;

    __weak TasksLibraryWindow *ws = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(tasks_look_status_hold * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        TasksLibraryWindow *s = ws;
        if (!s || s.status_message_id != message_id) return;   /* superseded */
        s.status_right.stringValue = @"";
    });
}

/* ── Task context menu ───────────────────────────────────────────────── */

/*
 * titled_item — a command item wearing its own title (a direction of a
 * flag, a destination list).
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
 * move_menu_excluding: — the "Move to List" submenu: every list but one.
 *
 * Inputs:
 *   list_id — the list to leave out (a single selection's own list), or 0
 *
 * Output:
 *   the submenu; empty when there is nowhere to move to.
 */
- (NSMenu *)
move_menu_excluding:(int64_t)list_id
{
    NSMenu    *menu  = [[NSMenu alloc] initWithTitle:@""];
    GPtrArray *lists = task_db_lists(self.app->db, FALSE);

    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        if (list->id == list_id) continue;
        NSString *title = *list->emoji != '\0'
            ? [NSString stringWithFormat:@"%@  %@", tasks_string(list->emoji), tasks_string(list->name)]
            : tasks_string(list->name);
        [menu addItem:[self titled_item:TasksCommandMoveToList param:@(list->id) title:title]];
    }
    task_ptr_array_free_lists(lists);
    return menu;
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
        BOOL to_done = task->status != TASK_STATUS_DONE;
        [menu addItem:[self titled_item:TasksCommandMarkDone param:@(to_done)
                                  title:to_done ? @"Mark Complete" : @"Mark Incomplete"]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self titled_item:TasksCommandSetPinned param:@(!task->pinned)
                                  title:task->pinned ? @"Remove from Favorites" : @"Add to Favorites"]];
        [menu addItem:[self titled_item:TasksCommandSetPriority param:@(!task->priority)
                                  title:task->priority ? @"Clear High Priority" : @"Set High Priority"]];
    } else {
        [menu addItem:[self titled_item:TasksCommandMarkDone param:@YES title:@"Mark All Complete"]];
        [menu addItem:[self titled_item:TasksCommandMarkDone param:@NO title:@"Mark All Incomplete"]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self titled_item:TasksCommandSetPinned param:@YES title:@"Add All to Favorites"]];
        [menu addItem:[self titled_item:TasksCommandSetPinned param:@NO title:@"Remove All from Favorites"]];
        [menu addItem:[self titled_item:TasksCommandSetPriority param:@YES title:@"Set All High Priority"]];
        [menu addItem:[self titled_item:TasksCommandSetPriority param:@NO title:@"Clear All High Priority"]];
    }
    [menu addItem:NSMenuItem.separatorItem];

    /* Move to List: left out for a lone subtask (subtasks travel with their
     * parent) and when there is nowhere to move to. */
    if (task == NULL || task->parent_id == 0) {
        NSMenu *move = [self move_menu_excluding:task != NULL ? task->list_id : 0];
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
 */
- (void)
run_command:(id)sender
{
    NSNumber *param = [sender isKindOfClass:NSMenuItem.class]
        ? ((NSMenuItem *)sender).representedObject : nil;

    [self perform_command:(TasksCommand)[sender tag] param:param];
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
 * both change) and say what was done.
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
    NSArray<NSNumber *> *ids = [self selected_task_ids];

    for (NSNumber *task_id in ids) {
        int64_t id = task_id.longLongValue;
        if (command == TasksCommandMarkDone) {
            task_db_task_set_status(self.app->db, id,
                                    on ? TASK_STATUS_DONE : TASK_STATUS_IN_PROGRESS);
        } else if (command == TasksCommandSetPinned) {
            task_db_task_set_pinned(self.app->db, id, on);
        } else {
            task_db_task_set_priority(self.app->db, id, on);
        }
    }
    [self full_refresh];

    unsigned long count = (unsigned long)ids.count;
    if (command == TasksCommandMarkDone) {
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
 * toggle_sidebar — slide the sidebar in or out and remember the choice.
 */
- (void)
toggle_sidebar
{
    BOOL show = self.sidebar_item.collapsed;

    self.sidebar_item.animator.collapsed = !show;
    task_config_set_bool(TASK_CFG_SIDEBAR_VISIBLE, show);
}

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
    case TasksCommandFind:
        [self.window makeFirstResponder:self.search_field];
        break;
    case TasksCommandToggleCompleted:
        task_config_set_bool(TASK_CFG_SHOW_COMPLETED,
                             !task_config_get_bool(TASK_CFG_SHOW_COMPLETED));
        [self refresh_tasks];
        break;
    case TasksCommandToggleSort:
        _manual_sort = !_manual_sort;
        task_config_set_bool(TASK_CFG_MANUAL_SORT, _manual_sort);
        [self refresh_tasks];
        break;
    case TasksCommandToggleSidebar:
        [self toggle_sidebar];
        break;
    case TasksCommandTogglePane:
        _kanban_on = !_kanban_on;
        task_config_set_bool(TASK_CFG_KANBAN_VIEW, _kanban_on);
        [self apply_pane_mode];
        [self refresh_tasks];
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
        /* Open Database and Compact Controls: not in the skeleton, and
         * disabled by state_of_command: so this is never reached for them. */
        break;
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
        [self.sidebar perform_command:command param:param];
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
            state.icon    = @"visible";
            state.tooltip = @"Show completed tasks";
        }
        break;
    case TasksCommandToggleSort:
        if (self.manual_sort) {
            state.title   = @"Automatic Sorting";
            state.icon    = @"automatic";
            state.tooltip = @"Switch to automatic sorting";
        }
        /* Two reasons the sort control can be unavailable, and each gets
         * its own tooltip: one control greyed for two unrelated causes is
         * only honest if it says which is in force. */
        state.enabled = !self.kanban_on && self.search == NULL;
        if (self.kanban_on) {
            state.tooltip = @"The Kanban board is always drag-sorted — "
                            @"turn Kanban View off to change list sorting";
        } else if (self.search != NULL) {
            state.tooltip = @"A search hides rows, and saving an order from a filtered list "
                            @"would lose the hidden tasks' places — clear the search box "
                            @"to drag-sort again";
        }
        break;
    case TasksCommandToggleSidebar:
        if (self.sidebar_item.collapsed) {
            state.title   = @"Show Sidebar";
            state.tooltip = @"Show the lists pane";
        }
        break;
    case TasksCommandTogglePane:
        if (self.kanban_on) {
            state.title   = @"List View";
            state.icon    = @"menu";
            state.tooltip = @"Show the tasks as a list";
        }
        break;
    case TasksCommandOpenDatabase:
    case TasksCommandToggleCompact:
        state.enabled = NO;         /* not in the skeleton (see the file comment) */
        break;
    default:
        break;                      /* New Task, Find: always available */
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
        [self.sidebar update_state:state for_command:command];
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
 * save_geometry — remember the window's place and size, and the sidebar's
 * width, for the next launch.
 */
- (void)
save_geometry
{
    NSRect frame   = self.window.frame;
    NSSize content = self.window.contentView.frame.size;

    task_config_set_int(TASK_CFG_WIN_X, (gint)frame.origin.x);
    task_config_set_int(TASK_CFG_WIN_Y, (gint)frame.origin.y);
    task_config_set_int(TASK_CFG_WIN_W, (gint)content.width);
    task_config_set_int(TASK_CFG_WIN_H, (gint)content.height);
    if (!self.sidebar_item.collapsed) {
        task_config_set_int(TASK_CFG_SIDEBAR_WIDTH, (gint)NSWidth(self.sidebar.frame));
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
    self.status_message_id++;       /* a pending status clear finds nothing to do */

    [self.task_list cancel_drag];
    [self.kanban cancel_drag];
    tasks_editor_close_all();

    [self.sidebar shut_down];
    [self.task_list shut_down];
    [self.kanban shut_down];

    task_search_free((TaskSearch *)_search);
    _search = NULL;
}

@end
