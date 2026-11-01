/*
 * notify_window.m — Tasks reminders window (notify_window.h).
 *
 * One persistent NSPanel accumulates all pending reminders.  The window title
 * reads "Tasks - 1 Reminder" or "Tasks - N Reminders".  A full-width blue hero
 * area at the top shows the selected item; a scrollable NSTableView lists every
 * pending item.
 * Snooze and Dismiss split combo buttons sit at the bottom: the title fires
 * the default action and the chevron opens a menu of alternatives.  Mark
 * Complete, beside the expand chevron, finishes the selected reminders' tasks.
 *
 * Selecting a row updates the hero (several rows may be selected).  Dismissing removes the row and closes the
 * window when the list empties.  Snoozing removes the row and re-queues it
 * after the chosen interval via dispatch_after.  Double-clicking a row opens
 * the task's editor and leaves the reminder in place.
 *
 * ONE ROW PER TASK.  task_notify.m mints a fresh notification key for an
 * undismissed task every reminder day, and again whenever its due date
 * changes, so an overdue task is reminded each morning.  A reminder for a task
 * already listed is therefore collapsed into the row shown: the row takes the
 * newer title and subtitle and carries a red circled count of the reminders
 * behind it, and dismissing it records every one of their keys.  Without that
 * collapsing, a panel left standing — or closed with its red button, which
 * only hides it — grew a second identical "Overdue" row every morning.
 *
 * A chevron button at the bar's lower left toggles between the default height
 * and one that fits every reminder; while expanded, the height follows the
 * list as reminders come and go.
 */

#import "notify_window.h"
#import "editor_window.h"
#import "look.h"
#import "ui_util.h"

#include "db.h"

/* ── Constants ───────────────────────────────────────────────────────── */

static const NSInteger snooze_default_mins = 10;

/* The gap between a reminder's title and its repeat badge.  The badge carries
 * it on its own left (its image is right-aligned), so the gap collapses with
 * the badge and an unrepeated reminder's title gets the width back. */
static const CGFloat badge_gap = 6.0;

/* The highest count SF Symbols draws as "<n>.circle.fill"; a reminder repeated
 * more often than this shows this number and the exact one in its tooltip. */
static const NSUInteger badge_symbol_max = 50;

/* ── Data model ──────────────────────────────────────────────────────── */

@interface TasksReminderItem : NSObject
@property (nonatomic, assign) int64_t   task_id;
/* Every notification key this one row stands for, oldest first: one per time
 * the reminder fired for the task without being dismissed.  All of them are
 * recorded when the row is dismissed, so none comes back. */
@property (nonatomic, strong)   NSMutableArray<NSString *> *keys;
@property (nonatomic, copy)     NSString *title;
@property (nonatomic, copy)     NSString *subtitle;
@property (nonatomic, readonly) NSUInteger repeats;
- (instancetype)initWithTaskId:(int64_t)task_id key:(NSString *)key title:(NSString *)title
                      subtitle:(NSString *)subtitle;
- (void)merge:(TasksReminderItem *)other;
@end

@implementation TasksReminderItem

/*
 * initWithTaskId:key:title:subtitle: — one reminder, firing for the first
 * time.
 *
 * Inputs:
 *   task_id  — the task it is for
 *   key      — its notification key (task_notify.m)
 *   title    — the task's title
 *   subtitle — the context line, e.g. "Overdue"
 *
 * Output:
 *   the item, holding that one key, so its repeats count is 1.
 */
- (instancetype)
initWithTaskId:(int64_t)task_id key:(NSString *)key title:(NSString *)title
      subtitle:(NSString *)subtitle
{
    self = [super init];
    if (self) {
        _task_id  = task_id;
        _keys     = [NSMutableArray arrayWithObject:[key copy]];
        _title    = [title copy];
        _subtitle = [subtitle copy];
    }
    return self;
}

/*
 * repeats — how many times this reminder has fired: the number of keys it
 * stands for, which is what the badge shows.
 *
 * Output:
 *   the count, never 0; 1 for a reminder that has fired once.
 */
- (NSUInteger)
repeats
{
    return _keys.count;
}

/*
 * merge: — take another reminder for the same task into this row: the keys it
 * stands for that this row does not hold yet, and its title and subtitle.
 * Those are normally the newer ones, so an "Overdue" supersedes yesterday's
 * "Due today"; a snoozed reminder returning to a row that fired again while
 * it was away brings back its own line instead, which is the one the user
 * snoozed.  Merging a row into itself, or a reminder for a different task,
 * does nothing.
 *
 * Inputs:
 *   other — the other reminder for this task, borrowed
 */
- (void)
merge:(TasksReminderItem *)other
{
    if (other == nil || other == self || other.task_id != _task_id) return;

    for (NSString *key in other.keys) {
        if (![_keys containsObject:key]) [_keys addObject:key];
    }
    self.title    = other.title;
    self.subtitle = other.subtitle;
}

@end

/* ── Cell parts ──────────────────────────────────────────────────────── */

/*
 * tasks_reminder_badge — the red circled count on a reminder that has fired
 * more than once.  It sizes itself from its image, so clearing the image
 * collapses it to nothing and the title gets the room back; its width carries
 * badge_gap, which the right-aligned image leaves empty on its left.
 */
@interface tasks_reminder_badge : NSImageView
@end

@implementation tasks_reminder_badge

/*
 * intrinsicContentSize — the circle plus its leading gap, or nothing at all
 * when the badge has no image.
 *
 * Output:
 *   the size the badge asks the row for.
 */
- (NSSize)
intrinsicContentSize
{
    if (self.image == nil) return NSZeroSize;
    return NSMakeSize(tasks_look_reminders_icon + badge_gap, tasks_look_reminders_icon);
}

/*
 * setImage: — set the image and re-measure, so the row lays out again as the
 * badge appears or goes.
 *
 * Inputs:
 *   image — the circled-count symbol, or nil for no badge
 */
- (void)
setImage:(NSImage *)image
{
    [super setImage:image];
    [self invalidateIntrinsicContentSize];
}

@end

/*
 * cell_subview — a reminder cell's subview carrying `ident`.  Cells are reused
 * (makeViewWithIdentifier:), so the views built on the first pass have to be
 * found again every time a row is filled in.
 *
 * Inputs:
 *   cell  — the cell view
 *   ident — the identifier the subview was given when it was built
 *
 * Output:
 *   the view, or nil when the cell carries none with that identifier.
 */
static NSView *
cell_subview(NSTableCellView *cell, NSString *ident)
{
    for (NSView *v in cell.subviews) {
        if ([v.identifier isEqualToString:ident]) return v;
    }
    return nil;
}

/*
 * badge_image — the circled count for a reminder that fired n times, as SF
 * Symbols' "<n>.circle.fill".  The symbols stop at badge_symbol_max, so a
 * higher count draws that number and the caller's tooltip carries the exact
 * one.  The red is the image view's tint, not the image's.
 *
 * Inputs:
 *   n — how many times the reminder fired, 2 or more
 *
 * Output:
 *   the image, or nil when the system has no such symbol.
 */
static NSImage *
badge_image(NSUInteger n)
{
    NSString *name = [NSString stringWithFormat:@"%lu.circle.fill",
                      (unsigned long)MIN(n, badge_symbol_max)];
    NSImage  *sym  = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    if (sym == nil) return nil;

    NSImageSymbolConfiguration *cfg =
        [NSImageSymbolConfiguration configurationWithPointSize:tasks_look_reminders_icon
                                                        weight:NSFontWeightRegular];
    return [sym imageWithSymbolConfiguration:cfg];
}

/* ── Window controller ───────────────────────────────────────────────── */

@interface TasksReminderWindowController
    : NSWindowController <NSTableViewDataSource, NSTableViewDelegate>

@property (nonatomic, assign) TaskApp *app;   /* borrowed; outlives the window */
@property (nonatomic, strong) NSMutableArray<TasksReminderItem *> *items;
@property (nonatomic, strong) NSTableView   *table;
@property (nonatomic, strong) SharedHero     *hero;   /* the selected reminder */
@property (nonatomic, assign) BOOL           refresh_pending;  /* a show_items is queued */
@property (nonatomic, strong) NSButton      *expand_btn;       /* the chevron */
@property (nonatomic, assign) BOOL           expanded;         /* window fits every reminder */

+ (instancetype)shared;
- (void)addItem:(TasksReminderItem *)item;
- (TasksReminderItem *)item_for_task:(int64_t)task_id;
- (void)update_hero;

@end

static TasksReminderWindowController *shared_controller = nil;

@implementation TasksReminderWindowController

/*
 * shared — return the module-level singleton, creating it on first call.
 */
+ (instancetype)
shared
{
    if (!shared_controller)
        shared_controller = [[self alloc] init_window];
    return shared_controller;
}

/*
 * init_window — build the panel, hero area, table, and button bar.
 */
- (instancetype)
init_window
{
    NSRect frame = NSMakeRect(0, 0, tasks_look_reminders_w, tasks_look_reminders_h);
    /* Utility mask: the small tool-window titlebar (no minimize button). */
    NSWindowStyleMask mask = (NSWindowStyleMaskTitled        |
                              NSWindowStyleMaskClosable      |
                              NSWindowStyleMaskResizable     |
                              NSWindowStyleMaskUtilityWindow);
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:frame
                                                styleMask:mask
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    self = [super initWithWindow:panel];
    if (!self) return nil;

    _items = [[NSMutableArray alloc] init];
    /* The title counts the reminders listed, from the empty list on, so the
     * window's name has one spelling and one place it is set. */
    [self update_title];

    panel.minSize  = NSMakeSize(tasks_look_reminders_min_w, tasks_look_reminders_min_h);
    panel.becomesKeyOnlyIfNeeded = NO;
    /* A reminder stays in view while another app is in front: above
     * ordinary windows, not hidden when Tasks deactivates, and on every
     * Space, full-screen ones included. */
    panel.floatingPanel      = YES;
    panel.hidesOnDeactivate  = NO;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                               NSWindowCollectionBehaviorFullScreenAuxiliary;

    NSView *cv = panel.contentView;

    /* ── Hero area ───────────────────────────────────────────────────── */
    _hero = [[SharedHero alloc] init_with_symbol:@"checklist.checked"];
    [cv addSubview:_hero];
    [NSLayoutConstraint activateConstraints:@[
        [_hero.topAnchor      constraintEqualToAnchor:cv.topAnchor],
        [_hero.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor],
        [_hero.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor],
    ]];

    /* ── Button bar ──────────────────────────────────────────────────── */
    NSView *btn_bar = [[NSView alloc] init];
    btn_bar.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:btn_bar];

    NSComboButton *snooze_btn  = [self make_snooze_button];
    NSComboButton *dismiss_btn = [self make_dismiss_button];
    snooze_btn.translatesAutoresizingMaskIntoConstraints  = NO;
    dismiss_btn.translatesAutoresizingMaskIntoConstraints = NO;
    [btn_bar addSubview:snooze_btn];
    [btn_bar addSubview:dismiss_btn];

    NSButton *complete_btn = [NSButton buttonWithTitle:@"Mark Complete" target:self
                                                action:@selector(mark_complete)];
    complete_btn.bezelStyle = NSBezelStyleRounded;
    complete_btn.font       = [NSFont systemFontOfSize:shared_look_ui_font];
    complete_btn.translatesAutoresizingMaskIntoConstraints = NO;
    [btn_bar addSubview:complete_btn];

    _expand_btn = [NSButton buttonWithTitle:@"" target:self action:@selector(toggle_expanded:)];
    _expand_btn.bezelStyle = NSBezelStyleRounded;
    _expand_btn.translatesAutoresizingMaskIntoConstraints = NO;
    [self update_expand_button];
    [btn_bar addSubview:_expand_btn];

    [NSLayoutConstraint activateConstraints:@[
        [btn_bar.leadingAnchor   constraintEqualToAnchor:cv.leadingAnchor],
        [btn_bar.trailingAnchor  constraintEqualToAnchor:cv.trailingAnchor],
        [btn_bar.bottomAnchor    constraintEqualToAnchor:cv.bottomAnchor],
        [btn_bar.heightAnchor    constraintEqualToConstant:shared_look_tool_bar_h],

        /* Snooze left of Dismiss, both right-aligned, in the tool panels'
         * bar: the same sizes (shared_look_tool_btn_w is tighter than a
         * combo button's own). */
        [dismiss_btn.trailingAnchor  constraintEqualToAnchor:btn_bar.trailingAnchor
                                                    constant:-shared_look_tool_btn_margin],
        [dismiss_btn.centerYAnchor   constraintEqualToAnchor:btn_bar.centerYAnchor],
        [dismiss_btn.widthAnchor     constraintEqualToConstant:shared_look_tool_btn_w],
        [dismiss_btn.heightAnchor    constraintEqualToConstant:shared_look_tool_btn_h],

        [snooze_btn.trailingAnchor   constraintEqualToAnchor:dismiss_btn.leadingAnchor
                                                   constant:-shared_look_tool_btn_gap],
        [snooze_btn.centerYAnchor    constraintEqualToAnchor:btn_bar.centerYAnchor],
        [snooze_btn.widthAnchor      constraintEqualToConstant:shared_look_tool_btn_w],
        [snooze_btn.heightAnchor     constraintEqualToConstant:shared_look_tool_btn_h],

        [_expand_btn.leadingAnchor   constraintEqualToAnchor:btn_bar.leadingAnchor
                                                    constant:shared_look_tool_btn_margin],
        [_expand_btn.centerYAnchor   constraintEqualToAnchor:btn_bar.centerYAnchor],
        [_expand_btn.widthAnchor     constraintEqualToConstant:shared_look_tool_btn_h * 1.5],
        [_expand_btn.heightAnchor    constraintEqualToConstant:shared_look_tool_btn_h],

        [complete_btn.leadingAnchor  constraintEqualToAnchor:_expand_btn.trailingAnchor
                                                    constant:shared_look_tool_btn_gap],
        [complete_btn.centerYAnchor  constraintEqualToAnchor:btn_bar.centerYAnchor],
        [complete_btn.heightAnchor   constraintEqualToConstant:shared_look_tool_btn_h],
    ]];

    /* Separator above buttons */
    NSBox *sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;
    sep.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:sep];
    [NSLayoutConstraint activateConstraints:@[
        [sep.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor],
        [sep.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor],
        [sep.bottomAnchor   constraintEqualToAnchor:btn_bar.topAnchor],
        [sep.heightAnchor   constraintEqualToConstant:1.0],
    ]];

    /* ── Table ───────────────────────────────────────────────────────── */
    _table = [[SharedTable alloc] init];
    _table.dataSource       = self;
    _table.delegate         = self;
    _table.headerView       = nil;
    _table.rowHeight        = tasks_look_reminders_row_h;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.gridStyleMask    = NSTableViewSolidHorizontalGridLineMask;
    _table.gridColor        = [NSColor separatorColor];
    _table.allowsEmptySelection = NO;
    _table.allowsMultipleSelection = YES;
    _table.target           = self;
    _table.doubleAction     = @selector(open_clicked:);

    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"item"];
    col.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:col];

    NSScrollView *scroll = [[NSScrollView alloc] init];
    scroll.documentView             = _table;
    scroll.hasVerticalScroller      = YES;
    scroll.hasHorizontalScroller    = NO;
    scroll.autohidesScrollers       = YES;
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:scroll];

    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor    constraintEqualToAnchor:_hero.bottomAnchor],
        [scroll.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor],
        [scroll.bottomAnchor   constraintEqualToAnchor:sep.topAnchor],
    ]];

    /* The table column must track the scroll view width. */
    [_table sizeLastColumnToFit];

    return self;
}

/* ── Button factories ────────────────────────────────────────────────── */

/*
 * make_split_button — a split-style NSComboButton: the title fires action,
 * the chevron at its right opens menu.
 *
 * Inputs:
 *   title  — button label
 *   action — selector fired by a click on the title, with the button as sender
 *   menu   — the alternatives shown by the chevron
 *
 * Output:
 *   the button, not yet constrained.
 */
- (NSComboButton *)
make_split_button:(NSString *)title action:(SEL)action menu:(NSMenu *)menu
{
    NSComboButton *btn = [NSComboButton comboButtonWithTitle:title
                                                        menu:menu
                                                      target:self
                                                      action:action];
    btn.style = NSComboButtonStyleSplit;
    btn.font  = [NSFont systemFontOfSize:shared_look_ui_font];
    return btn;
}

/*
 * make_snooze_button — Snooze: the title snoozes for snooze_default_mins
 * (carried in the button's tag), the menu offers the other durations (each
 * carried in its item's tag).  Both routes land in snooze:.
 */
- (NSComboButton *)
make_snooze_button
{
    NSMenu *menu = [[NSMenu alloc] init];
    static const struct { NSInteger mins; const char *label; } opts[] = {
        {  5, "5 minutes"  },
        { 15, "15 minutes" },
        { 30, "30 minutes" },
        { 60, "1 hour"     },
        {120, "2 hours"    },
    };
    for (NSUInteger i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        NSString *label = [NSString stringWithFormat:@"Snooze %s", opts[i].label];
        NSMenuItem *item = [menu addItemWithTitle:label
                                          action:@selector(snooze:)
                                   keyEquivalent:@""];
        item.target = self;
        item.tag    = opts[i].mins;
    }

    NSComboButton *btn = [self make_split_button:@"Snooze"
                                          action:@selector(snooze:)
                                            menu:menu];
    btn.tag = snooze_default_mins;
    return btn;
}

/*
 * make_dismiss_button — Dismiss: the title dismisses the selected reminder,
 * the menu adds Dismiss All.
 */
- (NSComboButton *)
make_dismiss_button
{
    NSMenu *menu = [[NSMenu alloc] init];
    NSMenuItem *all = [menu addItemWithTitle:@"Dismiss All"
                                      action:@selector(dismiss_all)
                               keyEquivalent:@""];
    all.target = self;

    return [self make_split_button:@"Dismiss"
                            action:@selector(dismiss_selected)
                              menu:menu];
}

/* ── Height ──────────────────────────────────────────────────────────── */

/*
 * update_expand_button — show the chevron for what a click will do: up to
 * expand (the window grows upward), down to collapse.
 */
- (void)
update_expand_button
{
    NSString *name = self.expanded ? @"chevron.down" : @"chevron.up";
    _expand_btn.image   = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
    _expand_btn.toolTip = self.expanded ? @"Collapse" : @"Show all reminders";
}

/*
 * set_content_height: — resize the window to a content height, keeping its
 * bottom edge, within the screen's usable height and the window's minimum.
 *
 * Inputs:
 *   height  — wanted content height
 *   animate — whether the resize animates
 */
- (void)
set_content_height:(CGFloat)height animate:(BOOL)animate
{
    NSWindow *win = self.window;
    NSRect content = [win contentRectForFrameRect:win.frame];
    content.size.height = height;
    NSRect frame = [win frameRectForContentRect:content];
    frame.origin.y = NSMinY(win.frame);

    NSScreen *screen = win.screen ?: NSScreen.screens.firstObject;
    NSRect usable = screen.visibleFrame;
    frame.size.height = MIN(NSHeight(frame), NSHeight(usable));
    frame.size.height = MAX(frame.size.height, win.minSize.height);
    if (NSMaxY(frame) > NSMaxY(usable)) frame.origin.y = NSMaxY(usable) - NSHeight(frame);

    [win setFrame:frame display:YES animate:animate];
}

/*
 * apply_height — resize the window to the default height, or, when expanded,
 * to the room around the list plus every reminder's row plus a small buffer
 * (at most the screen's usable height).
 */
- (void)
apply_height
{
    NSWindow *win = self.window;
    CGFloat height = tasks_look_reminders_h;
    if (self.expanded) {
        [win.contentView layoutSubtreeIfNeeded];
        NSScrollView *scroll = _table.enclosingScrollView;
        CGFloat around = NSHeight(win.contentView.frame) - NSHeight(scroll.contentView.bounds);
        height = around + (CGFloat)_items.count * tasks_look_reminders_row_h +
                 tasks_look_reminders_fit_buffer;
    }
    [self set_content_height:height animate:win.visible];
}

/*
 * toggle_expanded: — switch between the default height and the height that
 * shows every reminder.
 *
 * Inputs:
 *   sender — the chevron button (unused)
 */
- (void)
toggle_expanded:(id)sender
{
    (void)sender;
    self.expanded = !self.expanded;
    [self update_expand_button];
    [self apply_height];
}

/* ── Item management ─────────────────────────────────────────────────── */

/*
 * place_lower_right — put the window in the lower-right corner of the
 * primary display (shared_ui_place_lower_right).  The window keeps its size.
 */
- (void)
place_lower_right
{
    shared_ui_place_lower_right(self.window, NSScreen.screens.firstObject);
}

/*
 * item_for_task: — the listed reminder for a task.
 *
 * Inputs:
 *   task_id — the task
 *
 * Output:
 *   the row standing for that task, or nil when none is listed.
 */
- (TasksReminderItem *)
item_for_task:(int64_t)task_id
{
    for (TasksReminderItem *item in _items) {
        if (item.task_id == task_id) return item;
    }
    return nil;
}

/*
 * addItem: — add a reminder to the list and bring the window forward.  A
 * reminder for a task that is already listed is collapsed into the row shown
 * instead of adding a second one: the row keeps its place and its selection,
 * takes the added reminder's title and subtitle (merge:), and counts it in
 * its badge.
 *
 * The showing waits for the next run-loop pass, so that a pass delivering
 * twenty overdue tasks at launch reloads the table and orders the window
 * front once, not twenty times.
 *
 * Inputs:
 *   item — the reminder; the list takes it over, or merges it into the row it
 *          belongs with and lets it go
 */
- (void)
addItem:(TasksReminderItem *)item
{
    TasksReminderItem *shown = [self item_for_task:item.task_id];

    if (shown == nil) [_items addObject:item];
    else              [shown merge:item];

    if (self.refresh_pending) return;

    self.refresh_pending = YES;
    __weak TasksReminderWindowController *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        TasksReminderWindowController *s = ws;
        s.refresh_pending = NO;
        [s show_items];
    });
}

/*
 * show_items — make the window show the list as it now is and bring it
 * forward.  A window coming on screen appears in the lower-right corner;
 * one already showing stays where it is.
 */
- (void)
show_items
{
    [_table reloadData];

    /* Select the newest item if nothing is selected, else keep selection. */
    if (_table.selectedRow < 0 || _table.selectedRow >= (NSInteger)_items.count)
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
            byExtendingSelection:NO];

    [self update_hero];
    [self update_title];
    if (self.expanded) [self apply_height];
    if (!self.window.visible) [self place_lower_right];
    [self.window orderFrontRegardless];
}

/*
 * open_clicked: — open the editor for the double-clicked reminder's task.
 * The reminder stays listed.
 *
 * Inputs:
 *   sender — the table (unused)
 */
- (void)
open_clicked:(id)sender
{
    (void)sender;
    NSInteger row = _table.clickedRow;
    if (row < 0 || row >= (NSInteger)_items.count || self.app == NULL) return;
    tasks_editor_open(self.app, _items[(NSUInteger)row].task_id);
}

/*
 * update_hero — refresh the hero area to match the currently selected row.
 */
- (void)
update_hero
{
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.count) {
        [_hero set_title:@"" subtitle:nil];
        return;
    }
    TasksReminderItem *item = _items[(NSUInteger)row];
    [_hero set_title:item.title subtitle:item.subtitle];
}

/*
 * update_title — set the window title to "Tasks - N Reminders".  The count is
 * the rows listed: a reminder that fired more than once is one row, and its
 * badge carries how many times.
 */
- (void)
update_title
{
    NSUInteger n    = _items.count;
    NSString  *word = n == 1 ? @"Reminder" : @"Reminders";

    self.window.title = [NSString stringWithFormat:@"Tasks - %lu %@",
                         (unsigned long)n, word];
}

/* ── Actions ─────────────────────────────────────────────────────────── */

/*
 * record_dismissed: — remember every notification key a dismissed reminder
 * stands for, so none of them is shown again until its task's due date or
 * scheduled start changes: the key that first put the row on screen, and any
 * that fired into it while it stood there.
 *
 * Inputs:
 *   item — the reminder
 */
- (void)
record_dismissed:(TasksReminderItem *)item
{
    if (self.app == NULL || self.app->db == NULL) return;

    for (NSString *key in item.keys) {
        task_db_reminder_dismiss(self.app->db, key.UTF8String);
    }
}

/*
 * dismiss_selected — dismiss the currently selected reminder.
 */
- (void)
dismiss_selected
{
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.count) return;

    [self record_dismissed:_items[(NSUInteger)row]];
    [_items removeObjectAtIndex:(NSUInteger)row];
    [self show_after_removal:row];
}

/*
 * show_after_removal: — redraw the list after reminders were removed: close
 * the window when none are left, else select the reminder now at row (or the
 * last one) and refresh the hero, the title and, when expanded, the height.
 *
 * Inputs:
 *   row — the row of the first reminder removed
 */
- (void)
show_after_removal:(NSInteger)row
{
    [_table reloadData];

    if (_items.count == 0) {
        [self.window orderOut:nil];
        return;
    }

    NSInteger next = MIN(row, (NSInteger)_items.count - 1);
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)next]
        byExtendingSelection:NO];
    [self update_hero];
    [self update_title];
    if (self.expanded) [self apply_height];
}

/*
 * mark_complete — mark the task of every selected reminder done, and remove
 * those reminders.  A task whose write fails is reported in the status bar
 * and keeps its reminder.
 */
- (void)
mark_complete
{
    NSIndexSet *rows = _table.selectedRowIndexes;
    if (rows.count == 0 || self.app == NULL || self.app->db == NULL) return;

    NSMutableIndexSet *done = [NSMutableIndexSet indexSet];
    [rows enumerateIndexesUsingBlock:^(NSUInteger i, BOOL *stop) {
        (void)stop;
        TasksReminderItem *item = self->_items[i];
        Task *task = task_db_task_get(self.app->db, item.task_id);
        BOOL  ok   = task == NULL || task->deleted
                  || task_db_task_set_status(self.app->db, item.task_id,
                                             task_status_apply_done(task->status, TRUE));
        task_free(task);
        if (ok) {
            [done addIndex:i];
        } else {
            task_app_write_failed(self.app, "The status of a task");
        }
    }];
    if (done.count == 0) return;

    task_app_status(self.app, "%lu task%s \xe2\x80\x94 Completed", (unsigned long)done.count,
                    done.count == 1 ? "" : "s");
    NSInteger first = (NSInteger)done.firstIndex;
    [_items removeObjectsAtIndexes:done];
    [self show_after_removal:first];
}

/*
 * dismiss_all — dismiss every reminder and close the window.
 */
- (void)
dismiss_all
{
    for (TasksReminderItem *item in _items) [self record_dismissed:item];
    [_items removeAllObjects];
    [_table reloadData];
    [self.window orderOut:nil];
}

/*
 * snooze: — remove the selected reminder and re-queue it after the sender's
 * tag in minutes: the Snooze button carries the default, each menu item its
 * own duration.
 *
 * Inputs:
 *   sender — the NSComboButton or NSMenuItem that fired
 */
- (void)
snooze:(id)sender
{
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.count) return;

    TasksReminderItem *item = _items[(NSUInteger)row];
    NSInteger minutes = [sender tag];

    [_items removeObjectAtIndex:(NSUInteger)row];
    [self show_after_removal:row];

    /* The reminder comes back only for a task that is still there and still
     * open: the user may have finished or deleted it meanwhile. */
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(minutes * 60 * NSEC_PER_SEC)),
        dispatch_get_main_queue(),
        ^{
            TasksReminderWindowController *controller = [TasksReminderWindowController shared];
            if (controller.app == NULL || controller.app->db == NULL) return;
            Task *task = task_db_task_get(controller.app->db, item.task_id);
            BOOL  open = task != NULL && !task->deleted && task->status != TASK_STATUS_DONE;
            task_free(task);
            if (open) [controller addItem:item];
        });
}

/* ── NSTableViewDataSource ───────────────────────────────────────────── */

- (NSInteger)
numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return (NSInteger)_items.count;
}

/* ── NSTableViewDelegate ─────────────────────────────────────────────── */

- (NSView *)
tableView:(NSTableView *)tv
    viewForTableColumn:(NSTableColumn *)col
                   row:(NSInteger)row
{
    (void)col;
    static NSString *ident = @"ReminderCell";
    NSTableCellView *cell = [tv makeViewWithIdentifier:ident owner:self];

    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = ident;

        /* Icon */
        NSImageView *iv = [[NSImageView alloc] init];
        NSImage *sym = [NSImage imageWithSystemSymbolName:@"checklist.checked"
                                    accessibilityDescription:nil];
        if (!sym)
            sym = [NSImage imageWithSystemSymbolName:@"checkmark.circle.fill"
                                accessibilityDescription:nil];
        if (sym) {
            NSImageSymbolConfiguration *cfg =
                [NSImageSymbolConfiguration
                    configurationWithPointSize:tasks_look_reminders_icon
                                        weight:NSFontWeightRegular];
            iv.image = [sym imageWithSymbolConfiguration:cfg];
        }
        iv.contentTintColor = NSColor.secondaryLabelColor;
        iv.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:iv];
        cell.imageView = iv;

        /* Title */
        NSTextField *tf = [NSTextField labelWithString:@""];
        tf.font          = [NSFont systemFontOfSize:shared_look_ui_font];
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
        tf.maximumNumberOfLines = 1;
        tf.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:tf];
        cell.textField = tf;

        /* Status label (right-aligned) */
        NSTextField *st = [NSTextField labelWithString:@""];
        st.font          = [NSFont systemFontOfSize:shared_look_ui_font];
        st.textColor     = NSColor.secondaryLabelColor;
        st.lineBreakMode = NSLineBreakByTruncatingTail;
        st.maximumNumberOfLines = 1;
        st.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:st];
        st.identifier = @"status";

        /* Repeat badge, between the title and the status label.  With no
         * image it measures zero and the title spans to the status label. */
        tasks_reminder_badge *bd = [[tasks_reminder_badge alloc] init];
        bd.identifier       = @"badge";
        bd.imageAlignment   = NSImageAlignRight;
        bd.imageScaling     = NSImageScaleProportionallyDown;
        bd.contentTintColor = shared_ui_color(shared_look_due_overdue);
        bd.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:bd];

        [NSLayoutConstraint activateConstraints:@[
            [iv.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:10.0],
            [iv.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
            [iv.widthAnchor    constraintEqualToConstant:tasks_look_reminders_icon],
            [iv.heightAnchor   constraintEqualToConstant:tasks_look_reminders_icon],

            [st.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-10.0],
            [st.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
            [st.widthAnchor    constraintGreaterThanOrEqualToConstant:60.0],

            [bd.trailingAnchor constraintEqualToAnchor:st.leadingAnchor
                                             constant:-badge_gap],
            [bd.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],

            [tf.leadingAnchor  constraintEqualToAnchor:iv.trailingAnchor constant:8.0],
            [tf.trailingAnchor constraintLessThanOrEqualToAnchor:bd.leadingAnchor],
            [tf.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }

    TasksReminderItem *item = _items[(NSUInteger)row];
    cell.textField.stringValue = item.title;

    /* Status label */
    NSTextField *st = (NSTextField *)cell_subview(cell, @"status");
    if (st != nil) {
        st.stringValue = item.subtitle;
        /* "Overdue" in the same red as an overdue Due cell (tasks_due_color),
         * anything else in the secondary label colour. */
        st.textColor = [item.subtitle isEqualToString:@"Overdue"]
            ? shared_ui_color(shared_look_due_overdue)
            : NSColor.secondaryLabelColor;
    }

    /* Repeat badge: shown only once the reminder has fired more than once,
     * with the exact count in its tooltip, which the symbol may cap. */
    tasks_reminder_badge *bd = (tasks_reminder_badge *)cell_subview(cell, @"badge");
    if (bd != nil) {
        BOOL repeated = item.repeats > 1;
        bd.image   = repeated ? badge_image(item.repeats) : nil;
        bd.toolTip = repeated ? [NSString stringWithFormat:@"Reminded %lu times",
                                 (unsigned long)item.repeats]
                              : nil;
    }

    return cell;
}

- (CGFloat)
tableView:(NSTableView *)tv heightOfRow:(NSInteger)row
{
    (void)tv; (void)row;
    return tasks_look_reminders_row_h;
}

- (void)
tableViewSelectionDidChange:(NSNotification *)note
{
    (void)note;
    [self update_hero];
}

@end

/* ── Public entry point ──────────────────────────────────────────────── */

/*
 * tasks_notify_add — enqueue a reminder and show the window.
 * Inputs are documented in notify_window.h.
 */
void
tasks_notify_add(TaskApp *app, int64_t task_id, NSString *key, NSString *title,
                 NSString *subtitle)
{
    TasksReminderItem *item = [[TasksReminderItem alloc] initWithTaskId:task_id
                                                                    key:key
                                                                  title:title
                                                               subtitle:subtitle];
    TasksReminderWindowController *controller = [TasksReminderWindowController shared];
    controller.app = app;
    [controller addItem:item];
}

/*
 * tasks_notify_retitle — re-read every listed reminder's title.
 * Documented in notify_window.h.
 */
void
tasks_notify_retitle(void)
{
    TasksReminderWindowController *controller = shared_controller;
    if (controller == nil || controller.app == NULL || controller.app->db == NULL ||
        controller.items.count == 0) return;

    for (TasksReminderItem *item in controller.items) {
        Task *task = task_db_task_get(controller.app->db, item.task_id);
        if (task != NULL) item.title = tasks_title_string(task, @"");
        task_free(task);
    }
    [controller.table reloadData];
    [controller update_hero];
}
