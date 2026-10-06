#import "lists_bar.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

#include "task_sidebar.h"
#include "task_view.h"

/* ── Button subclass ─────────────────────────────────────────────────── */

/*
 * TasksListsBtn — NSButton with a sel_data property that carries the
 * selection (@[@(kind), @(id)]) without touching the cell.  The "on" state
 * is painted explicitly via a layer background because most NSBezelStyle
 * variants only show pressed state while the mouse is held, not persistently.
 */
@interface TasksListsBtn : NSButton
@property (nonatomic, strong) NSArray *sel_data;
@end

@implementation TasksListsBtn

/*
 * setState: — mark dirty so drawRect: repaints the selection background.
 *
 * Inputs:
 *   state — the new control state
 */
- (void)
setState:(NSControlStateValue)state
{
    [super setState:state];
    [self setNeedsDisplay:YES];
}

/*
 * drawRect: — paint a rounded selection background before the button's own
 * content.  The rect is inset 2 pt top/bottom so it sits inside the bezel's
 * visual boundary rather than filling the whole frame.
 *
 * Inputs:
 *   dirty_rect — the dirty rectangle (unused; we always repaint the background)
 */
- (void)
drawRect:(NSRect)dirty_rect
{
    if (self.state == NSControlStateValueOn) {
        [[NSColor.labelColor colorWithAlphaComponent:0.12] setFill];
        NSRect dr = [self.cell drawingRectForBounds:self.bounds];
        NSRect r  = NSMakeRect(NSMinX(self.bounds), NSMinY(dr),
                               NSWidth(self.bounds), NSHeight(dr));
        [[NSBezierPath bezierPathWithRoundedRect:r xRadius:4.0 yRadius:4.0] fill];
    }
    [super drawRect:dirty_rect];
}

@end

/* ── Constants ───────────────────────────────────────────────────────── */

static const CGFloat btn_h        = 24.0;  /* button height inside the 28 pt bar */
static const CGFloat btn_gap      = 4.0;   /* gap between buttons */
static const CGFloat bar_pad      = 6.0;   /* leading padding of button strip */
static const CGFloat btn_min_w    = 60.0;  /* minimum button width */
static const CGFloat btn_text_pad = 16.0;  /* horizontal text padding in button */

/* ── Horizontal-scroll view ──────────────────────────────────────────── */

/*
 * TasksListsScroll — an NSScrollView that redirects a vertical wheel
 * gesture to horizontal scrolling, so a normal two-finger swipe pans the
 * button strip without a visible scrollbar.
 */
@interface TasksListsScroll : NSScrollView
@end

@implementation TasksListsScroll

/*
 * scrollWheel: — when the Y delta dominates, pan the strip horizontally;
 * otherwise let AppKit handle the event normally.
 *
 * Inputs:
 *   event — the wheel event
 */
- (void)
scrollWheel:(NSEvent *)event
{
    CGFloat dx = event.scrollingDeltaX;
    CGFloat dy = event.scrollingDeltaY;

    if (fabs(dy) > fabs(dx)) {
        NSPoint origin  = self.contentView.bounds.origin;
        origin.x       -= dy * (event.hasPreciseScrollingDeltas ? 1.0 : 10.0);
        CGFloat max_x   = self.documentView.frame.size.width
                        - self.contentView.bounds.size.width;
        origin.x        = MAX(0.0, MIN(origin.x, MAX(0.0, max_x)));
        [self.contentView scrollToPoint:origin];
        [self reflectScrolledClipView:self.contentView];
    } else {
        [super scrollWheel:event];
    }
}

@end

/* ── The bar ─────────────────────────────────────────────────────────── */

@interface TasksListsBar ()
@property (nonatomic, weak)   TasksLibraryWindow  *library;
@property (nonatomic, strong) TasksListsScroll *scroll;
@property (nonatomic, strong) NSView               *strip;
@property (nonatomic, assign) BOOL                  is_shut_down;
@end

@implementation TasksListsBar

/* ── Construction ────────────────────────────────────────────────────── */

/*
 * init_with_library: — build the bar.
 * Inputs/output are documented in lists_bar.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library = library;

    self.scroll                    = [[TasksListsScroll alloc] initWithFrame:NSZeroRect];
    self.scroll.hasHorizontalScroller = NO;
    self.scroll.hasVerticalScroller   = NO;
    self.scroll.drawsBackground       = NO;
    self.scroll.translatesAutoresizingMaskIntoConstraints = NO;

    self.strip               = [[NSView alloc] initWithFrame:NSZeroRect];
    self.scroll.documentView = self.strip;

    [self addSubview:self.scroll];
    [NSLayoutConstraint activateConstraints:@[
        [self.scroll.topAnchor constraintEqualToAnchor:self.topAnchor],
        [self.scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

/* ── Button helpers ──────────────────────────────────────────────────── */

/*
 * btn_width_for: — the width a recessed button needs to show a title.
 *
 * Inputs:
 *   title — the button's label
 *
 * Output:
 *   the width in points, at least btn_min_w.
 */
static CGFloat
btn_width_for(NSString *title)
{
    NSDictionary *attrs = @{ NSFontAttributeName:
        [NSFont systemFontOfSize:11.0] };
    return MAX(btn_min_w, ceil([title sizeWithAttributes:attrs].width) + btn_text_pad);
}

/*
 * make_btn — a recessed toggle button for the strip.
 *
 * Inputs:
 *   title  — the label
 *   target — the action receiver
 *   action — the selector to call when clicked
 *
 * Output:
 *   new button; caller sets frame and sel_data.
 */
static TasksListsBtn *
make_btn(NSString *title, id target, SEL action)
{
    TasksListsBtn *btn = [[TasksListsBtn alloc] initWithFrame:NSZeroRect];
    btn.title         = title;
    btn.bezelStyle    = NSBezelStyleAccessoryBar;
    btn.buttonType    = NSButtonTypeToggle;
    btn.imagePosition = NSNoImage;
    btn.font          = [NSFont systemFontOfSize:11.0];
    btn.target        = target;
    btn.action        = action;
    return btn;
}

/*
 * sel_array — box a task_sel_t as an NSArray for representedObject storage.
 *
 * Inputs:
 *   kind  — the selection kind
 *   ident — the selection id
 *
 * Output:
 *   @[@(kind), @(ident)]
 */
static NSArray *
sel_array(task_sel_kind_t kind, int64_t ident)
{
    return @[@((NSInteger)kind), @(ident)];
}

/* ── Selection display ───────────────────────────────────────────────── */

/*
 * update_selection_display — set each button's state to match the window's
 * current selection.  A group button is active when the selection is that
 * group or a list that belongs to it.
 */
- (void)
update_selection_display
{
    task_sel_t sel = self.library.selection;

    int64_t sel_group_id = 0;
    if (sel.kind == task_sel_list) {
        TaskList *list = task_db_list_get(self.library.app->db, sel.id);
        if (list) { sel_group_id = list->group_id; task_list_free(list); }
    }

    for (NSView *v in self.strip.subviews) {
        if (![v isKindOfClass:TasksListsBtn.class]) continue;
        TasksListsBtn *btn  = (TasksListsBtn *)v;
        NSArray           *repr = btn.sel_data;
        if (!repr || repr.count < 2) { btn.state = NSControlStateValueOff; continue; }

        task_sel_kind_t kind  = (task_sel_kind_t)[repr[0] integerValue];
        int64_t         ident = [repr[1] longLongValue];
        BOOL            active;

        if (kind == task_sel_group) {
            active = (sel.kind == task_sel_group && ident == sel.id) ||
                     (sel.kind == task_sel_list  && sel_group_id == ident);
        } else {
            active = (kind == sel.kind && ident == sel.id);
        }
        btn.state = active ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

/* ── Reload ──────────────────────────────────────────────────────────── */

/*
 * update_selection — reflect the window's current selection in the button
 * states without rebuilding the strip.
 * Documented in lists_bar.h.
 */
- (void)
update_selection
{
    if (!self.library || self.is_shut_down) return;
    [self update_selection_display];
}

/*
 * reload — discard and rebuild every button from the database, then update
 * states to reflect the window's selection.
 * Documented in lists_bar.h.
 */
- (void)
reload
{
    if (!self.library || self.is_shut_down) return;

    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];

    TaskApp *app = self.library.app;
    CGFloat  x   = bar_pad;
    CGFloat  cy  = tasks_look_lists_bar_h / 2.0;

    NSMutableSet<NSNumber *> *known_groups = [NSMutableSet set];

    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *view = task_view_nth(i);
        if (view->visible != NULL && !view->visible(app, view->user_data)) continue;

        NSString *title = tasks_string(view->label);
        CGFloat    w    = btn_width_for(title);
        TasksListsBtn *btn = make_btn(title, self, @selector(btn_clicked:));
        btn.frame    = NSMakeRect(x, cy - btn_h / 2.0, w, btn_h);
        btn.sel_data = sel_array(task_sel_view, (int64_t)i);
        [self.strip addSubview:btn];
        x += w + btn_gap;
    }

    GPtrArray *groups = task_db_groups(app->db);
    for (guint i = 0; i < groups->len; i++) {
        const TaskGroup *group = g_ptr_array_index(groups, i);
        [known_groups addObject:@(group->id)];

        NSString *title = tasks_string(group->name);
        CGFloat    w    = btn_width_for(title);
        TasksListsBtn *btn = make_btn(title, self, @selector(group_btn_clicked:));
        btn.frame    = NSMakeRect(x, cy - btn_h / 2.0, w, btn_h);
        btn.sel_data = sel_array(task_sel_group, group->id);
        [self.strip addSubview:btn];
        x += w + btn_gap;
    }
    task_ptr_array_free_groups(groups);

    GPtrArray *lists = task_db_lists(app->db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        if (list->group_id != 0 && [known_groups containsObject:@(list->group_id)]) continue;

        char     *label = task_sidebar_list_label(list);
        NSString *title = tasks_string(label);
        free(label);

        CGFloat            w   = btn_width_for(title);
        TasksListsBtn *btn = make_btn(title, self, @selector(btn_clicked:));
        btn.frame    = NSMakeRect(x, cy - btn_h / 2.0, w, btn_h);
        btn.sel_data = sel_array(task_sel_list, list->id);
        [self.strip addSubview:btn];
        x += w + btn_gap;
    }
    task_ptr_array_free_lists(lists);

    CGFloat strip_w  = MAX(x + bar_pad, NSWidth(self.scroll.bounds));
    self.strip.frame = NSMakeRect(0, 0, strip_w, tasks_look_lists_bar_h);
    [self update_selection_display];
}

/* ── Button actions ──────────────────────────────────────────────────── */

/*
 * btn_clicked: — a view or standalone list button was clicked: tell the
 * window to select it.
 *
 * Inputs:
 *   sender — the button; representedObject is @[@(kind), @(id)]
 */
- (void)
btn_clicked:(TasksListsBtn *)sender
{
    if (self.is_shut_down) return;
    NSArray   *repr = sender.sel_data;
    task_sel_t sel  = { (task_sel_kind_t)[repr[0] integerValue],
                        [repr[1] longLongValue] };
    [self.library select:sel];
    [self update_selection_display];
}

/*
 * group_btn_clicked: — a group button was clicked: build a menu of the
 * group's lists and pop it up just below the button.
 *
 * Inputs:
 *   sender — the button; sel_data is @[@(task_sel_group), @(group_id)]
 */
- (void)
group_btn_clicked:(TasksListsBtn *)sender
{
    if (self.is_shut_down) return;

    NSArray  *repr     = sender.sel_data;
    int64_t   group_id = [repr[1] longLongValue];
    TaskApp  *app      = self.library.app;
    NSMenu   *menu     = [[NSMenu alloc] initWithTitle:@""];

    GPtrArray *lists = task_db_lists(app->db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        if (list->group_id != group_id) continue;

        char     *label   = task_sidebar_list_label(list);
        NSString *title   = tasks_string(label);
        free(label);

        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:title action:@selector(list_item_chosen:) keyEquivalent:@""];
        item.target            = self;
        item.representedObject = @(list->id);
        [menu addItem:item];
    }
    task_ptr_array_free_lists(lists);

    if (menu.numberOfItems == 0) return;
    [menu popUpMenuPositioningItem:nil
                        atLocation:NSMakePoint(0, NSHeight(sender.bounds))
                            inView:sender];
}

/*
 * list_item_chosen: — a list menu item was chosen: select that list.
 *
 * Inputs:
 *   sender — the menu item; representedObject is the list id as NSNumber
 */
- (void)
list_item_chosen:(NSMenuItem *)sender
{
    if (self.is_shut_down) return;
    int64_t    list_id = [sender.representedObject longLongValue];
    task_sel_t sel     = { task_sel_list, list_id };
    [self.library select:sel];
    [self update_selection_display];
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * perform_command:param: — layout experiment stub; sidebar commands are not
 * implemented here.
 * Documented in lists_bar.h.
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
    (void)command;
    (void)param;
}

/*
 * update_state:for_command: — mirror the sidebar's enabled/disabled logic
 * so main-menu commands grey out correctly.
 * Documented in lists_bar.h.
 */
- (void)
update_state:(TasksCommandState *)state for_command:(TasksCommand)command
{
    task_sel_t sel = self.library.selection;

    switch (command) {
    case TasksCommandEditList:
    case TasksCommandMoveToGroup:
        state.enabled = sel.kind == task_sel_list;
        break;
    case TasksCommandDeleteList:
        state.enabled = sel.kind == task_sel_list || sel.kind == task_sel_group;
        if (sel.kind == task_sel_group) {
            state.title = tasks_command_spec(TasksCommandDeleteGroup)->title;
        }
        break;
    case TasksCommandRemoveFromGroup:
        if (sel.kind == task_sel_list) {
            TaskList *list = task_db_list_get(self.library.app->db, sel.id);
            state.enabled  = list != NULL && list->group_id != 0;
            task_list_free(list);
        } else {
            state.enabled = NO;
        }
        break;
    case TasksCommandRenameGroup:
    case TasksCommandDeleteGroup:
        state.enabled = sel.kind == task_sel_group;
        break;
    default:
        break;
    }
}

/* ── Lifetime ────────────────────────────────────────────────────────── */

/*
 * shut_down — the window is closing.
 * Documented in lists_bar.h.
 */
- (void)
shut_down
{
    self.is_shut_down = YES;
}

@end
