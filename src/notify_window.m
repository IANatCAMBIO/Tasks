/*
 * notify_window.m — Tasks reminders window (notify_window.h).
 *
 * One persistent NSPanel accumulates all pending reminders.  The window title
 * reads "1 Reminder" or "N Reminders".  A full-width blue hero area at the top
 * shows the selected item; a scrollable NSTableView lists every pending item.
 * Snooze▼ and Dismiss▼ popup buttons sit at the bottom.
 *
 * Selecting a row updates the hero.  Dismissing removes the row and closes the
 * window when the list empties.  Snoozing removes the row and re-queues it
 * after the chosen interval via dispatch_after.
 */

#import "notify_window.h"

/* ── Split button ────────────────────────────────────────────────────── */

/*
 * TasksSplitButton — a regular push button that also has a drop menu.
 *
 * A quick click fires the button's own action (the default).
 * Holding the mouse down for 350 ms shows drop_menu below the button so
 * the user can choose an alternate action.  A small chevron icon to the
 * right signals that the dropdown exists.
 */
@interface TasksSplitButton : NSButton
@property (nonatomic, strong) NSMenu *drop_menu;
@end

@implementation TasksSplitButton

- (void)
mouseDown:(NSEvent *)event
{
    (void)event;
    NSDate *timeout = [NSDate dateWithTimeIntervalSinceNow:0.35];
    NSEvent *up = [self.window nextEventMatchingMask:NSEventMaskLeftMouseUp
                                          untilDate:timeout
                                             inMode:NSEventTrackingRunLoopMode
                                            dequeue:YES];
    if (up) {
        /* Quick release — fire default action if still inside the button. */
        NSPoint loc = [self convertPoint:up.locationInWindow fromView:nil];
        if (NSPointInRect(loc, self.bounds))
            [NSApp sendAction:self.action to:self.target from:self];
    } else {
        /* Held long enough — show the alternate-action menu. */
        NSRect bounds = self.bounds;
        [_drop_menu popUpMenuPositioningItem:nil
                                 atLocation:NSMakePoint(0, NSMinY(bounds))
                                     inView:self];
    }
}

@end

/* ── Layout constants ────────────────────────────────────────────────── */

static const CGFloat win_w         = 480.0;
static const CGFloat win_h         = 230.0;
static const CGFloat win_min_h     = 190.0;
static const CGFloat hero_h        = 78.0;
static const CGFloat btn_bar_h     = 44.0;
static const CGFloat btn_h         = 28.0;
static const CGFloat btn_w         = 110.0;
static const CGFloat btn_gap       = 8.0;
static const CGFloat btn_margin    = 12.0;
static const CGFloat row_h         = 30.0;
static const CGFloat icon_size     = 16.0;
static const CGFloat hero_icon_sz  = 24.0;
static const CGFloat hero_h_pad    = 16.0;  /* horizontal inset in hero */
static const NSInteger snooze_default_mins = 10;
static const CGFloat hero_v_pad    = 10.0;

/* ── Data model ──────────────────────────────────────────────────────── */

@interface TasksReminderItem : NSObject
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy) NSString *subtitle;
- (instancetype)initWithTitle:(NSString *)title subtitle:(NSString *)subtitle;
@end

@implementation TasksReminderItem

- (instancetype)
initWithTitle:(NSString *)title subtitle:(NSString *)subtitle
{
    self = [super init];
    if (self) {
        _title    = [title copy];
        _subtitle = [subtitle copy];
    }
    return self;
}

@end

/* ── Hero view ───────────────────────────────────────────────────────── */

/*
 * TasksHeroView — solid accent-blue background drawn behind the hero labels.
 */
@interface TasksHeroView : NSView
@end

@implementation TasksHeroView

- (BOOL)isOpaque { return YES; }

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;
    [[NSColor colorWithSRGBRed:0.0 green:0.447 blue:0.776 alpha:1.0] setFill];
    NSRectFill(self.bounds);
}

@end

/* ── Window controller ───────────────────────────────────────────────── */

@interface TasksReminderWindowController
    : NSWindowController <NSTableViewDataSource, NSTableViewDelegate>

@property (nonatomic, strong) NSMutableArray<TasksReminderItem *> *items;
@property (nonatomic, strong) NSTableView   *table;
@property (nonatomic, strong) NSTextField   *hero_title;
@property (nonatomic, strong) NSTextField   *hero_subtitle;
@property (nonatomic, strong) NSImageView   *hero_icon;

+ (instancetype)shared;
- (void)addItem:(TasksReminderItem *)item;

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
    NSRect frame = NSMakeRect(0, 0, win_w, win_h);
    NSWindowStyleMask mask = (NSWindowStyleMaskTitled      |
                              NSWindowStyleMaskClosable    |
                              NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable);
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:frame
                                                styleMask:mask
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    self = [super initWithWindow:panel];
    if (!self) return nil;

    _items = [[NSMutableArray alloc] init];

    panel.title    = @"Reminders";
    panel.minSize  = NSMakeSize(300.0, win_min_h);
    panel.becomesKeyOnlyIfNeeded = NO;

    /* Position bottom-right of main screen; autosave restores on next launch. */
    [panel setFrameAutosaveName:@"TasksReminderWindow"];
    if (![panel setFrameUsingName:@"TasksReminderWindow"]) {
        NSScreen *scr = NSScreen.mainScreen;
        if (scr) {
            NSRect vis = scr.visibleFrame;
            NSPoint origin = NSMakePoint(NSMaxX(vis) - win_w - 16.0,
                                         NSMinY(vis) + 16.0);
            [panel setFrameOrigin:origin];
        }
    }

    NSView *cv = panel.contentView;

    /* ── Hero area ───────────────────────────────────────────────────── */
    TasksHeroView *hero = [[TasksHeroView alloc] init];
    hero.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:hero];

    /* Hero icon */
    _hero_icon = [[NSImageView alloc] init];
    NSImage *sym = [NSImage imageWithSystemSymbolName:@"checklist.checked"
                                accessibilityDescription:nil];
    if (!sym)
        sym = [NSImage imageWithSystemSymbolName:@"checkmark.circle.fill"
                            accessibilityDescription:nil];
    if (sym) {
        NSImageSymbolConfiguration *cfg =
            [NSImageSymbolConfiguration
                configurationWithPointSize:hero_icon_sz
                                    weight:NSFontWeightMedium];
        _hero_icon.image = [sym imageWithSymbolConfiguration:cfg];
    }
    _hero_icon.contentTintColor = NSColor.whiteColor;
    _hero_icon.translatesAutoresizingMaskIntoConstraints = NO;
    [hero addSubview:_hero_icon];

    /* Hero title */
    _hero_title = [NSTextField labelWithString:@""];
    _hero_title.font                  = [NSFont boldSystemFontOfSize:15.0];
    _hero_title.textColor             = NSColor.whiteColor;
    _hero_title.lineBreakMode         = NSLineBreakByTruncatingTail;
    _hero_title.maximumNumberOfLines  = 1;
    _hero_title.translatesAutoresizingMaskIntoConstraints = NO;
    [hero addSubview:_hero_title];

    /* Hero subtitle */
    _hero_subtitle = [NSTextField labelWithString:@""];
    _hero_subtitle.font                  = [NSFont systemFontOfSize:12.0];
    _hero_subtitle.textColor             = [NSColor colorWithWhite:0.88 alpha:1.0];
    _hero_subtitle.lineBreakMode         = NSLineBreakByTruncatingTail;
    _hero_subtitle.maximumNumberOfLines  = 1;
    _hero_subtitle.translatesAutoresizingMaskIntoConstraints = NO;
    [hero addSubview:_hero_subtitle];

    /* Hero layout */
    [NSLayoutConstraint activateConstraints:@[
        [hero.topAnchor    constraintEqualToAnchor:cv.topAnchor],
        [hero.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor],
        [hero.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor],
        [hero.heightAnchor   constraintEqualToConstant:hero_h],

        [_hero_icon.leadingAnchor  constraintEqualToAnchor:hero.leadingAnchor
                                                  constant:hero_h_pad],
        [_hero_icon.centerYAnchor  constraintEqualToAnchor:hero.centerYAnchor],
        [_hero_icon.widthAnchor    constraintEqualToConstant:hero_icon_sz],
        [_hero_icon.heightAnchor   constraintEqualToConstant:hero_icon_sz],

        [_hero_title.leadingAnchor  constraintEqualToAnchor:_hero_icon.trailingAnchor
                                                   constant:10.0],
        [_hero_title.trailingAnchor constraintEqualToAnchor:hero.trailingAnchor
                                                   constant:-hero_h_pad],
        [_hero_title.bottomAnchor   constraintEqualToAnchor:hero.centerYAnchor
                                                   constant:-hero_v_pad / 2.0],

        [_hero_subtitle.leadingAnchor  constraintEqualToAnchor:_hero_title.leadingAnchor],
        [_hero_subtitle.trailingAnchor constraintEqualToAnchor:_hero_title.trailingAnchor],
        [_hero_subtitle.topAnchor      constraintEqualToAnchor:hero.centerYAnchor
                                                     constant:hero_v_pad / 2.0],
    ]];

    /* ── Button bar ──────────────────────────────────────────────────── */
    NSView *btn_bar = [[NSView alloc] init];
    btn_bar.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:btn_bar];

    TasksSplitButton *snooze_btn = [self make_snooze_button];
    TasksSplitButton *dismiss_btn = [self make_dismiss_button];
    snooze_btn.translatesAutoresizingMaskIntoConstraints  = NO;
    dismiss_btn.translatesAutoresizingMaskIntoConstraints = NO;
    [btn_bar addSubview:snooze_btn];
    [btn_bar addSubview:dismiss_btn];

    [NSLayoutConstraint activateConstraints:@[
        [btn_bar.leadingAnchor   constraintEqualToAnchor:cv.leadingAnchor],
        [btn_bar.trailingAnchor  constraintEqualToAnchor:cv.trailingAnchor],
        [btn_bar.bottomAnchor    constraintEqualToAnchor:cv.bottomAnchor],
        [btn_bar.heightAnchor    constraintEqualToConstant:btn_bar_h],

        /* Snooze left of Dismiss, both right-aligned */
        [dismiss_btn.trailingAnchor  constraintEqualToAnchor:btn_bar.trailingAnchor
                                                    constant:-btn_margin],
        [dismiss_btn.centerYAnchor   constraintEqualToAnchor:btn_bar.centerYAnchor],
        [dismiss_btn.widthAnchor     constraintEqualToConstant:btn_w],
        [dismiss_btn.heightAnchor    constraintEqualToConstant:btn_h],

        [snooze_btn.trailingAnchor   constraintEqualToAnchor:dismiss_btn.leadingAnchor
                                                   constant:-btn_gap],
        [snooze_btn.centerYAnchor    constraintEqualToAnchor:btn_bar.centerYAnchor],
        [snooze_btn.widthAnchor      constraintEqualToConstant:btn_w],
        [snooze_btn.heightAnchor     constraintEqualToConstant:btn_h],
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
    _table = [[NSTableView alloc] init];
    _table.dataSource       = self;
    _table.delegate         = self;
    _table.headerView       = nil;
    _table.rowHeight        = row_h;
    _table.intercellSpacing = NSMakeSize(0, 0);
    _table.gridStyleMask    = NSTableViewSolidHorizontalGridLineMask;
    _table.gridColor        = [NSColor separatorColor];
    _table.allowsEmptySelection = NO;

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
        [scroll.topAnchor    constraintEqualToAnchor:hero.bottomAnchor],
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
 * make_split_button — build a TasksSplitButton with a chevron hint image.
 *
 * Inputs:
 *   title  — button label
 *   action — default action selector fired on a quick click
 */
- (TasksSplitButton *)
make_split_button:(NSString *)title action:(SEL)action
{
    TasksSplitButton *btn = [TasksSplitButton buttonWithTitle:title
                                                       target:self
                                                       action:action];
    btn.bezelStyle = NSBezelStyleRounded;

    NSImage *chevron = [NSImage imageWithSystemSymbolName:@"chevron.down"
                                    accessibilityDescription:nil];
    if (chevron) {
        NSImageSymbolConfiguration *cfg =
            [NSImageSymbolConfiguration configurationWithPointSize:8.0
                                                            weight:NSFontWeightRegular];
        btn.image         = [chevron imageWithSymbolConfiguration:cfg];
        btn.imagePosition = NSImageRight;
    }
    return btn;
}

/*
 * make_snooze_button — split button; default = snooze_default_mins, menu =
 * alternate durations.
 */
- (TasksSplitButton *)
make_snooze_button
{
    TasksSplitButton *btn = [self make_split_button:@"Snooze"
                                             action:@selector(snooze_default)];

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
                                          action:@selector(snooze_item:)
                                   keyEquivalent:@""];
        item.target = self;
        item.tag    = opts[i].mins;
    }
    btn.drop_menu = menu;
    return btn;
}

/*
 * make_dismiss_button — split button; default = dismiss selected, menu adds
 * Dismiss All.
 */
- (TasksSplitButton *)
make_dismiss_button
{
    TasksSplitButton *btn = [self make_split_button:@"Dismiss"
                                             action:@selector(dismiss_selected)];

    NSMenu *menu = [[NSMenu alloc] init];
    NSMenuItem *all = [menu addItemWithTitle:@"Dismiss All"
                                      action:@selector(dismiss_all)
                               keyEquivalent:@""];
    all.target = self;
    btn.drop_menu = menu;
    return btn;
}

/* ── Item management ─────────────────────────────────────────────────── */

/*
 * addItem: — append a reminder to the list and bring the window forward.
 */
- (void)
addItem:(TasksReminderItem *)item
{
    [_items addObject:item];
    [_table reloadData];

    /* Select the new item if nothing is selected, else keep selection. */
    if (_table.selectedRow < 0 || _table.selectedRow >= (NSInteger)_items.count)
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
            byExtendingSelection:NO];

    [self update_hero];
    [self update_title];
    [self.window orderFrontRegardless];
}

/*
 * update_hero — refresh the hero area to match the currently selected row.
 */
- (void)
update_hero
{
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.count) {
        _hero_title.stringValue    = @"";
        _hero_subtitle.stringValue = @"";
        return;
    }
    TasksReminderItem *item = _items[(NSUInteger)row];
    _hero_title.stringValue    = item.title;
    _hero_subtitle.stringValue = item.subtitle;
}

/*
 * update_title — set the window title to "N Reminder(s)".
 */
- (void)
update_title
{
    NSUInteger n = _items.count;
    if (n == 1)
        self.window.title = @"1 Reminder";
    else
        self.window.title = [NSString stringWithFormat:@"%lu Reminders",
                             (unsigned long)n];
}

/* ── Actions ─────────────────────────────────────────────────────────── */

/*
 * dismiss_selected — remove the currently selected reminder.
 */
- (void)
dismiss_selected
{
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.count) return;

    [_items removeObjectAtIndex:(NSUInteger)row];
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
}

/*
 * dismiss_all — remove every reminder and close the window.
 */
- (void)
dismiss_all
{
    [_items removeAllObjects];
    [_table reloadData];
    [self.window orderOut:nil];
}

/*
 * snooze_default — snooze the selected item for the default interval.
 */
- (void)
snooze_default
{
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.count) return;

    TasksReminderItem *item = _items[(NSUInteger)row];
    [_items removeObjectAtIndex:(NSUInteger)row];
    [_table reloadData];

    if (_items.count == 0) {
        [self.window orderOut:nil];
    } else {
        NSInteger next = MIN(row, (NSInteger)_items.count - 1);
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)next]
            byExtendingSelection:NO];
        [self update_hero];
        [self update_title];
    }

    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW,
                      (int64_t)(snooze_default_mins * 60 * NSEC_PER_SEC)),
        dispatch_get_main_queue(),
        ^{
            [[TasksReminderWindowController shared] addItem:item];
        });
}

/*
 * snooze_item: — remove the selected item and re-queue it after the
 * interval given in the menu item's tag (minutes).
 */
- (void)
snooze_item:(NSMenuItem *)sender
{
    NSInteger row = _table.selectedRow;
    if (row < 0 || row >= (NSInteger)_items.count) return;

    TasksReminderItem *item = _items[(NSUInteger)row];
    NSInteger minutes = sender.tag;

    [_items removeObjectAtIndex:(NSUInteger)row];
    [_table reloadData];

    if (_items.count == 0) {
        [self.window orderOut:nil];
    } else {
        NSInteger next = MIN(row, (NSInteger)_items.count - 1);
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)next]
            byExtendingSelection:NO];
        [self update_hero];
        [self update_title];
    }

    /* Re-add after the snooze interval. */
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(minutes * 60 * NSEC_PER_SEC)),
        dispatch_get_main_queue(),
        ^{
            [[TasksReminderWindowController shared] addItem:item];
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
                    configurationWithPointSize:icon_size
                                        weight:NSFontWeightRegular];
            iv.image = [sym imageWithSymbolConfiguration:cfg];
        }
        iv.contentTintColor = NSColor.secondaryLabelColor;
        iv.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:iv];
        cell.imageView = iv;

        /* Title */
        NSTextField *tf = [NSTextField labelWithString:@""];
        tf.font          = [NSFont systemFontOfSize:13.0];
        tf.lineBreakMode = NSLineBreakByTruncatingTail;
        tf.maximumNumberOfLines = 1;
        tf.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:tf];
        cell.textField = tf;

        /* Status label (right-aligned) */
        NSTextField *st = [NSTextField labelWithString:@""];
        st.font          = [NSFont systemFontOfSize:11.0];
        st.textColor     = NSColor.secondaryLabelColor;
        st.lineBreakMode = NSLineBreakByTruncatingTail;
        st.maximumNumberOfLines = 1;
        st.translatesAutoresizingMaskIntoConstraints = NO;
        [cell addSubview:st];
        st.identifier = @"status";

        [NSLayoutConstraint activateConstraints:@[
            [iv.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor constant:10.0],
            [iv.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
            [iv.widthAnchor    constraintEqualToConstant:icon_size],
            [iv.heightAnchor   constraintEqualToConstant:icon_size],

            [st.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-10.0],
            [st.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
            [st.widthAnchor    constraintGreaterThanOrEqualToConstant:60.0],

            [tf.leadingAnchor  constraintEqualToAnchor:iv.trailingAnchor constant:8.0],
            [tf.trailingAnchor constraintLessThanOrEqualToAnchor:st.leadingAnchor
                                                        constant:-6.0],
            [tf.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }

    TasksReminderItem *item = _items[(NSUInteger)row];
    cell.textField.stringValue = item.title;

    /* Status label */
    NSTextField *st = [cell viewWithTag:0];
    for (NSView *v in cell.subviews) {
        if ([v isKindOfClass:[NSTextField class]] &&
            [((NSTextField *)v).identifier isEqualToString:@"status"]) {
            st = (NSTextField *)v;
            break;
        }
    }
    if (st) {
        st.stringValue = item.subtitle;
        /* Colour "Overdue" red, others in secondary label colour. */
        if ([item.subtitle isEqualToString:@"Overdue"])
            st.textColor = [NSColor colorWithSRGBRed:0.85 green:0.15 blue:0.15 alpha:1.0];
        else
            st.textColor = NSColor.secondaryLabelColor;
    }

    return cell;
}

- (CGFloat)
tableView:(NSTableView *)tv heightOfRow:(NSInteger)row
{
    (void)tv; (void)row;
    return row_h;
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
tasks_notify_add(NSString *title, NSString *subtitle)
{
    TasksReminderItem *item =
        [[TasksReminderItem alloc] initWithTitle:title subtitle:subtitle];
    [[TasksReminderWindowController shared] addItem:item];
}
