#import "lists_bar.h"
#import "editor_window.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

#include "task_order.h"
#include "task_sidebar.h"
#include "task_view.h"

/* ── Constants ───────────────────────────────────────────────────────── */

static const CGFloat btn_h        = 24.0;  /* button height inside the 28 pt bar */
static const CGFloat btn_gap      = 4.0;   /* gap between buttons */
static const CGFloat bar_pad      = 6.0;   /* leading padding of button strip */
static const CGFloat btn_min_w    = 60.0;  /* minimum button width */
static const CGFloat btn_text_pad = 16.0;  /* horizontal text padding in button */

/* The list sheet's emoji slot. */
static NSString * const emoji_placeholder = @"…";
static NSString * const emoji_tooltip     = @"Optional emoji — click to pick";

/* The sheets' fields. */
static const CGFloat emoji_field_w = 40.0;
static const CGFloat name_field_w  = 240.0;

/* Dragging a list's button along the bar.  The pasteboard type is the
 * bar's own: a list drag never leaves it.  The item's content is the list
 * id as decimal text. */
static NSPasteboardType const bar_drag_type = @TASKS_BUNDLE_ID ".lists-bar-list";
static const CGFloat drag_threshold  = 4.0;   /* points moved before a press is a drag */
static const CGFloat drop_rule_w     = 2.0;   /* the insertion mark                    */
static const CGFloat dragged_alpha   = 0.4;   /* the button left behind during a drag  */

/* ── The classes, declared ───────────────────────────────────────────── */

/*
 * TasksListsBtn — NSButton with a sel_data property that carries the
 * selection (@[@(kind), @(id)]) without touching the cell.  The "on" state
 * is painted explicitly via a layer background because most NSBezelStyle
 * variants only show pressed state while the mouse is held, not persistently.
 * A draggable button (an ungrouped list) starts a drag of its list when
 * pressed and moved; a plain press is still a click.
 */
@interface TasksListsBtn : NSButton
@property (nonatomic, strong) NSArray *sel_data;
@property (nonatomic, assign) BOOL     draggable;
@end

/*
 * TasksListsStrip — the scrolling document view holding the buttons.  It
 * draws the drop marks of a list drag: an insertion rule between buttons,
 * or an outline round the group the list would join.
 */
@interface TasksListsStrip : NSView
@property (nonatomic, assign) CGFloat drop_rule_x;   /* < 0: none           */
@property (nonatomic, assign) NSRect  drop_group;    /* NSZeroRect: none    */
@end

/* The lists' one order as the bar shows it (task_sidebar.h): each group's
 * members group by group, then the ungrouped lists. */
typedef struct {
    int64_t *ids;                   /* every list, in that order           */
    int      n;
    int      n_grouped;             /* how many belong to a group          */
    int64_t *group_ids;             /* the groups, in their order          */
    int     *group_ends;            /* index in ids past each group's last */
    int      n_groups;
} bar_order_t;

@interface TasksListsBar () <NSDraggingSource, NSDraggingDestination>
@property (nonatomic, weak)   TasksLibraryWindow  *library;
@property (nonatomic, strong) NSScrollView        *scroll;
@property (nonatomic, strong) TasksListsStrip     *strip;
@property (nonatomic, weak)   TasksListsBtn       *dragged;    /* during a drag  */
@property (nonatomic, assign) BOOL                 is_shut_down;
@property (nonatomic, assign) CGFloat              content_w;  /* buttons + padding, no scroll floor */
- (void)begin_drag_of:(TasksListsBtn *)button event:(NSEvent *)event;
@end

/*
 * bar_of — the lists bar a view sits in.
 *
 * Inputs:
 *   view — a view inside the bar
 *
 * Output:
 *   the bar, or nil when the view is not in one.
 */
static TasksListsBar *
bar_of(NSView *view)
{
    while (view != nil && ![view isKindOfClass:TasksListsBar.class]) view = view.superview;
    return (TasksListsBar *)view;
}

/* ── Button subclass ─────────────────────────────────────────────────── */

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

/*
 * mouseDown: — a draggable button watches the press itself: moved past
 * the threshold it becomes a drag of the list; released in place it is a
 * click.  Any other button clicks as NSButton does.
 *
 * Inputs:
 *   event — the mouse-down
 */
- (void)
mouseDown:(NSEvent *)event
{
    if (!self.draggable) {
        [super mouseDown:event];
        return;
    }

    NSPoint start = event.locationInWindow;
    self.highlighted = YES;
    for (;;) {
        NSEvent *next = [self.window nextEventMatchingMask:NSEventMaskLeftMouseUp |
                                                            NSEventMaskLeftMouseDragged];
        if (next.type == NSEventTypeLeftMouseUp) {
            self.highlighted = NO;
            [self performClick:nil];
            return;
        }
        if (hypot(next.locationInWindow.x - start.x, next.locationInWindow.y - start.y)
            >= drag_threshold) {
            self.highlighted = NO;
            [bar_of(self) begin_drag_of:self event:next];
            return;
        }
    }
}

/*
 * menuForEvent: — a right-click on a button is the bar's context menu for
 * that button: the bar knows which list or group it stands for.
 *
 * Inputs:
 *   event — the right mouse-down
 *
 * Output:
 *   the bar's menu, or nil when the button is not in a bar.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    return [bar_of(self) menuForEvent:event];
}

@end

/* ── The strip ───────────────────────────────────────────────────────── */

@implementation TasksListsStrip

/*
 * initWithFrame: — a strip with no drop mark.
 *
 * Inputs:
 *   frame — the initial frame
 *
 * Output:
 *   the view.
 */
- (instancetype)
initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) _drop_rule_x = -1.0;
    return self;
}

/*
 * drawRect: — the drop mark of a list drag, if one is in progress: the
 * insertion rule, or the outline round the group being dropped on.
 *
 * Inputs:
 *   dirty_rect — the dirty rectangle (unused)
 */
- (void)
drawRect:(NSRect)dirty_rect
{
    [NSColor.controlAccentColor set];
    if (self.drop_rule_x >= 0.0) {
        CGFloat cy = NSMidY(self.bounds);
        NSRectFill(NSMakeRect(self.drop_rule_x - drop_rule_w / 2.0, cy - btn_h / 2.0,
                              drop_rule_w, btn_h));
    }
    if (!NSIsEmptyRect(self.drop_group)) {
        NSBezierPath *outline = [NSBezierPath
            bezierPathWithRoundedRect:NSInsetRect(self.drop_group, -1.0, -1.0)
                              xRadius:5.0 yRadius:5.0];
        outline.lineWidth = drop_rule_w;
        [outline stroke];
    }
}

@end

/* ── The emoji field ────────────────────────────────────────────────── */

/*
 * TasksEmojiField — the list sheet's emoji slot: a small text field that
 * holds at most one character and opens the system's emoji picker when
 * clicked.
 *
 * The picker is the system's Character Viewer, which types into whatever
 * text is being edited, so the slot has to be a real text field; this
 * class adds only what makes it behave like a slot rather than a line of
 * text.  In its compact form (the one Ctrl+Cmd+Space shows) the picker
 * closes itself once an emoji is chosen; the slot then ends its edit, so
 * the expanded Character Viewer window, which has no API to close it,
 * types nowhere until the slot is clicked again.
 */
@interface TasksEmojiField : NSTextField
@end

@implementation TasksEmojiField

/*
 * mouseDown: — a click starts editing as usual, selects what is there so
 * that the chosen emoji replaces it, and opens the system's emoji picker
 * over the slot.
 *
 * Inputs:
 *   event — the mouse-down
 */
- (void)
mouseDown:(NSEvent *)event
{
    [super mouseDown:event];
    [self.currentEditor selectAll:nil];
    [NSApp orderFrontCharacterPalette:nil];
}

/*
 * textDidChange: — keep only the last character entered, then give up the
 * keyboard.
 *
 * "Character" is a composed sequence: an emoji with a skin tone or a flag
 * is several code points and must stay whole.  Editing ends once there is
 * a character because a field that is still being edited does not see the
 * next click (its editor does), and so could not open the picker again.
 * That happens on the next run-loop pass: ending an edit from inside the
 * editor's own change notification re-enters it.
 *
 * Inputs:
 *   note — NSTextDidChangeNotification; its object is the field editor
 */
- (void)
textDidChange:(NSNotification *)note
{
    NSText   *editor = note.object;
    NSString *text   = editor.string;

    if (text.length > 0) {
        NSRange last = [text rangeOfComposedCharacterSequenceAtIndex:text.length - 1];
        if (last.location > 0) editor.string = [text substringWithRange:last];

        __weak TasksEmojiField *ws = self;
        dispatch_async(dispatch_get_main_queue(), ^{
            TasksEmojiField *s = ws;
            if (s.currentEditor) [s.window makeFirstResponder:nil];
        });
    }
    [super textDidChange:note];
}

@end

/* ── Sheets ─────────────────────────────────────────────────────────── */

/*
 * sheet_content — the content of one of the bar's sheets: a headline
 * saying what the sheet is for (a sheet has no title bar to say it), above
 * a row of fields.
 *
 * Inputs:
 *   headline — "New List", "Rename Group", ...
 *   fields   — the fields, left to right; each defines its own width
 *
 * Output:
 *   new view for tasks_ui_sheet, sized by its content.
 */
static NSView *
sheet_content(NSString *headline, NSArray<NSView *> *fields)
{
    NSTextField *title = [NSTextField labelWithString:headline];
    title.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];

    NSStackView *row = [NSStackView stackViewWithViews:fields];
    row.orientation  = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing      = tasks_look_form_spacing;

    NSStackView *content = [NSStackView stackViewWithViews:@[title, row]];
    content.orientation  = NSUserInterfaceLayoutOrientationVertical;
    content.alignment    = NSLayoutAttributeLeading;
    content.spacing      = tasks_look_form_spacing;
    return content;
}

/*
 * name_field — a sheet's name field: a stock bezeled text field of the
 * sheets' common width.
 *
 * Inputs:
 *   text        — the name to start with; @"" for none
 *   placeholder — what the empty field says
 *
 * Output:
 *   new field.
 */
static NSTextField *
name_field(NSString *text, NSString *placeholder)
{
    NSTextField *field = [NSTextField textFieldWithString:text];

    field.placeholderString = placeholder;
    [field.widthAnchor constraintEqualToConstant:name_field_w].active = YES;
    return field;
}

/*
 * trimmed — a name as typed, without the white space around it.
 *
 * Inputs:
 *   field — a sheet's name field
 *
 * Output:
 *   the name; empty when nothing but white space was typed.
 */
static NSString *
trimmed(NSTextField *field)
{
    return [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

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

/* ── The lists' order ────────────────────────────────────────────────── */

/*
 * bar_order_load — the lists' one order as the bar shows it, from the
 * database.
 *
 * Inputs:
 *   db    — the connection
 *   order — receives the order; free with bar_order_free
 */
static void
bar_order_load(TaskDatabase *db, bar_order_t *order)
{
    GPtrArray *groups = task_db_groups(db);
    GPtrArray *lists  = task_db_lists(db, FALSE);

    order->n          = 0;
    order->n_groups   = (int)groups->len;
    order->ids        = g_new(int64_t, lists->len + 1);
    order->group_ids  = g_new(int64_t, groups->len + 1);
    order->group_ends = g_new(int, groups->len + 1);

    for (guint g = 0; g < groups->len; g++) {
        const TaskGroup *group = g_ptr_array_index(groups, g);
        order->group_ids[g] = group->id;
        for (guint i = 0; i < lists->len; i++) {
            const TaskList *list = g_ptr_array_index(lists, i);
            if (list->group_id == group->id) order->ids[order->n++] = list->id;
        }
        order->group_ends[g] = order->n;
    }
    order->n_grouped = order->n;

    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list   = g_ptr_array_index(lists, i);
        bool            placed = false;
        for (guint g = 0; g < groups->len && !placed; g++) {
            placed = list->group_id == ((const TaskGroup *)g_ptr_array_index(groups, g))->id;
        }
        if (!placed) order->ids[order->n++] = list->id;
    }
    task_ptr_array_free_lists(lists);
    task_ptr_array_free_groups(groups);
}

/*
 * bar_order_free — release what bar_order_load allocated.
 *
 * Inputs:
 *   order — the order
 */
static void
bar_order_free(bar_order_t *order)
{
    g_free(order->ids);
    g_free(order->group_ids);
    g_free(order->group_ends);
}

/*
 * bar_order_index_of — where a list stands in the order.
 *
 * Inputs:
 *   order   — the order
 *   list_id — the list
 *
 * Output:
 *   its index, or -1 when it is not there.
 */
static int
bar_order_index_of(const bar_order_t *order, int64_t list_id)
{
    for (int i = 0; i < order->n; i++) {
        if (order->ids[i] == list_id) return i;
    }
    return -1;
}

/*
 * bar_order_group_at — the group of a position in the order.
 *
 * Inputs:
 *   order — the order
 *   index — a position in ids
 *
 * Output:
 *   the group id, or 0 for an ungrouped position.
 */
static int64_t
bar_order_group_at(const bar_order_t *order, int index)
{
    for (int g = 0; g < order->n_groups; g++) {
        if (index < order->group_ends[g]) return order->group_ids[g];
    }
    return 0;
}

/* ── The bar ─────────────────────────────────────────────────────────── */

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

    self.strip               = [[TasksListsStrip alloc] initWithFrame:NSZeroRect];
    self.scroll.documentView = self.strip;

    [self addSubview:self.scroll];
    [NSLayoutConstraint activateConstraints:@[
        [self.scroll.topAnchor constraintEqualToAnchor:self.topAnchor],
        [self.scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    [self registerForDraggedTypes:@[bar_drag_type]];
    return self;
}

/* ── Layout ──────────────────────────────────────────────────────────── */

/*
 * layout — keep the strip frame and scroll position consistent with the
 * current scroll view width.  The strip is always at least as wide as the
 * scroll view (so the bar looks full), but never so wide that the scroll
 * position can reveal blank space past the last button.
 */
- (void)
layout
{
    [super layout];

    CGFloat sw      = NSWidth(self.scroll.bounds);
    CGFloat new_w   = MAX(self.content_w, sw);
    NSRect  sf      = self.strip.frame;

    if (fabs(sf.size.width - new_w) > 0.5) {
        sf.size.width    = new_w;
        self.strip.frame = sf;

        /* Pull the scroll position back if it is now past the new end. */
        NSPoint  origin = self.scroll.contentView.bounds.origin;
        CGFloat  max_x  = MAX(0.0, new_w - sw);
        if (origin.x > max_x) {
            origin.x = max_x;
            [self.scroll.contentView scrollToPoint:origin];
            [self.scroll reflectScrolledClipView:self.scroll.contentView];
        }
    }
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

/*
 * sel_of — the selection a button stands for.
 *
 * Inputs:
 *   btn — a strip button
 *
 * Output:
 *   its selection; a header-kind selection with id 0 when it has none.
 */
static task_sel_t
sel_of(TasksListsBtn *btn)
{
    NSArray *repr = btn.sel_data;
    if (repr == nil || repr.count < 2) return (task_sel_t){ task_sel_header, 0 };
    return (task_sel_t){ (task_sel_kind_t)[repr[0] integerValue], [repr[1] longLongValue] };
}

/*
 * group_among — whether a group id names one of the groups shown.
 *
 * Inputs:
 *   groups   — the groups, from task_db_groups
 *   group_id — a list's group id; 0 (ungrouped) is never among them
 *
 * Output:
 *   true when the group is there.
 */
static bool
group_among(const GPtrArray *groups, int64_t group_id)
{
    for (guint i = 0; i < groups->len; i++) {
        if (((const TaskGroup *)g_ptr_array_index(groups, i))->id == group_id) return true;
    }
    return false;
}

/*
 * buttons — the strip's buttons, left to right.
 *
 * Output:
 *   the buttons.
 */
- (NSArray<TasksListsBtn *> *)
buttons
{
    NSMutableArray<TasksListsBtn *> *found = [NSMutableArray array];
    for (NSView *view in self.strip.subviews) {
        if ([view isKindOfClass:TasksListsBtn.class]) [found addObject:(TasksListsBtn *)view];
    }
    [found sortUsingComparator:^NSComparisonResult(TasksListsBtn *a, TasksListsBtn *b) {
        return [@(NSMinX(a.frame)) compare:@(NSMinX(b.frame))];
    }];
    return found;
}

/*
 * button_at: — the strip button under a point.
 *
 * Inputs:
 *   point — a point in the strip's coordinates
 *
 * Output:
 *   the button, or nil for the empty space of the bar.
 */
- (TasksListsBtn *)
button_at:(NSPoint)point
{
    for (TasksListsBtn *btn in [self buttons]) {
        if (NSPointInRect(point, btn.frame)) return btn;
    }
    return nil;
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

    for (TasksListsBtn *btn in [self buttons]) {
        task_sel_t own = sel_of(btn);
        BOOL       active;

        if (own.kind == task_sel_header) {
            active = NO;
        } else if (own.kind == task_sel_group) {
            active = (sel.kind == task_sel_group && own.id == sel.id) ||
                     (sel.kind == task_sel_list  && sel_group_id == own.id);
        } else {
            active = (own.kind == sel.kind && own.id == sel.id);
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
 * add_button:sel:at:action: — append one button to the strip.
 *
 * Inputs:
 *   title  — its label
 *   sel    — the selection it stands for
 *   x      — the x the button starts at; advanced past it and the gap
 *   action — the selector a click sends to the bar
 *
 * Output:
 *   the button.
 */
- (TasksListsBtn *)
add_button:(NSString *)title sel:(task_sel_t)sel at:(CGFloat *)x action:(SEL)action
{
    CGFloat        w   = btn_width_for(title);
    CGFloat        cy  = tasks_look_lists_bar_h / 2.0;
    TasksListsBtn *btn = make_btn(title, self, action);

    btn.frame    = NSMakeRect(*x, cy - btn_h / 2.0, w, btn_h);
    btn.sel_data = sel_array(sel.kind, sel.id);
    [self.strip addSubview:btn];
    *x += w + btn_gap;
    return btn;
}

/*
 * reload — discard and rebuild every button from the database, then update
 * states to reflect the window's selection.  A selection whose row has
 * gone (a deleted list or group, the Favorites view emptied) is moved by
 * the sidebar's rules (task_sidebar_pick) and reported with [library
 * select:].
 * Documented in lists_bar.h.
 */
- (void)
reload
{
    if (!self.library || self.is_shut_down) return;

    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];

    TaskApp   *app    = self.library.app;
    CGFloat    x      = bar_pad;
    GPtrArray *groups = task_db_groups(app->db);
    GPtrArray *lists  = task_db_lists(app->db, FALSE);

    /* Every selection the bar can reach, in the order it is shown: the
     * views, each group followed by its lists (reached through the group's
     * menu), then the ungrouped lists. */
    guint       n_rows = 0;
    task_sel_t *rows   = g_new(task_sel_t, task_view_count() + groups->len + lists->len);

    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *view = task_view_nth(i);
        if (!task_view_shown(view, app)) continue;

        rows[n_rows++] = (task_sel_t){ task_sel_view, (int64_t)i };
        [self add_button:tasks_string(view->label) sel:rows[n_rows - 1] at:&x
                  action:@selector(btn_clicked:)];
    }

    for (guint i = 0; i < groups->len; i++) {
        const TaskGroup *group = g_ptr_array_index(groups, i);

        char *label = task_sidebar_group_label(group);
        rows[n_rows++] = (task_sel_t){ task_sel_group, group->id };
        [self add_button:tasks_string(label) sel:rows[n_rows - 1] at:&x
                  action:@selector(group_btn_clicked:)];
        free(label);
        for (guint j = 0; j < lists->len; j++) {
            const TaskList *list = g_ptr_array_index(lists, j);
            if (list->group_id == group->id) {
                rows[n_rows++] = (task_sel_t){ task_sel_list, list->id };
            }
        }
    }

    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        if (group_among(groups, list->group_id)) continue;   /* in its group's menu */

        char     *label = task_sidebar_list_label(list);
        NSString *title = tasks_string(label);
        free(label);

        rows[n_rows++] = (task_sel_t){ task_sel_list, list->id };
        [self add_button:title sel:rows[n_rows - 1] at:&x action:@selector(btn_clicked:)]
            .draggable = YES;
    }
    task_ptr_array_free_lists(lists);
    task_ptr_array_free_groups(groups);

    self.content_w   = x + bar_pad;
    CGFloat strip_w  = MAX(self.content_w, NSWidth(self.scroll.bounds));
    self.strip.frame = NSMakeRect(0, 0, strip_w, tasks_look_lists_bar_h);

    task_sel_t wanted = self.library.selection;
    int        pick   = task_sidebar_pick(rows, (int)n_rows, wanted);
    if (pick >= 0 && (rows[pick].kind != wanted.kind || rows[pick].id != wanted.id)) {
        [self.library select:rows[pick]];   /* updates the display itself */
    } else {
        [self update_selection_display];
    }
    g_free(rows);
}

/* ── Button actions ──────────────────────────────────────────────────── */

/*
 * btn_clicked: — a view or standalone list button was clicked: tell the
 * window to select it.
 *
 * Inputs:
 *   sender — the button; sel_data is @[@(kind), @(id)]
 */
- (void)
btn_clicked:(TasksListsBtn *)sender
{
    if (self.is_shut_down) return;
    [self.library select:sel_of(sender)];
    [self update_selection_display];
}

/*
 * menu_for_group: — the menu a group button's click pops up.
 * Inputs/output are documented in lists_bar.h.
 */
- (NSMenu *)
menu_for_group:(int64_t)group_id
{
    TasksLibraryWindow *library = self.library;
    NSMenu             *menu    = [[NSMenu alloc] initWithTitle:@""];
    if (!library || self.is_shut_down) return menu;

    task_sel_t current = library.selection;

    /* The group's own view: every member list's tasks together. */
    NSMenuItem *all = [[NSMenuItem alloc]
        initWithTitle:@"All Tasks" action:@selector(sel_item_chosen:) keyEquivalent:@""];
    all.target            = self;
    all.representedObject = sel_array(task_sel_group, group_id);
    all.state             = current.kind == task_sel_group && current.id == group_id
                            ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:all];

    bool       separated = false;
    GPtrArray *lists     = task_db_lists(library.app->db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        if (list->group_id != group_id) continue;
        if (!separated) { [menu addItem:NSMenuItem.separatorItem]; separated = true; }

        char     *label   = task_sidebar_list_label(list);
        NSString *title   = tasks_string(label);
        free(label);

        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:title action:@selector(sel_item_chosen:) keyEquivalent:@""];
        item.target            = self;
        item.representedObject = sel_array(task_sel_list, list->id);
        item.state             = current.kind == task_sel_list && current.id == list->id
                                 ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:item];
    }
    task_ptr_array_free_lists(lists);

    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:tasks_command_menu_item(TasksCommandRenameGroup, @(group_id), library)];
    [menu addItem:tasks_command_menu_item(TasksCommandDeleteGroup, @(group_id), library)];
    return menu;
}

/*
 * group_btn_clicked: — a group button was clicked: pop up its menu
 * (menu_for_group:) just below the button.
 *
 * Inputs:
 *   sender — the button; sel_data is @[@(task_sel_group), @(group_id)]
 */
- (void)
group_btn_clicked:(TasksListsBtn *)sender
{
    if (self.is_shut_down) return;

    [[self menu_for_group:sel_of(sender).id] popUpMenuPositioningItem:nil
                                                           atLocation:NSMakePoint(0, NSHeight(sender.bounds))
                                                               inView:sender];
    /* The click turned the toggle on whether or not an item was chosen. */
    [self update_selection_display];
}

/*
 * sel_item_chosen: — an item of a group's menu was chosen: select the
 * group (All Tasks) or one of its lists.
 *
 * Inputs:
 *   sender — the menu item; representedObject is @[@(kind), @(id)]
 */
- (void)
sel_item_chosen:(NSMenuItem *)sender
{
    if (self.is_shut_down) return;
    NSArray   *repr = sender.representedObject;
    task_sel_t sel  = { (task_sel_kind_t)[repr[0] integerValue], [repr[1] longLongValue] };
    [self.library select:sel];
    [self update_selection_display];
}

/* ── Context menu ────────────────────────────────────────────────────── */

/*
 * add_list_commands:to: — the commands of the selected list, for a context
 * menu: Edit List, Delete List, Move to Group (the other groups) and, for
 * a grouped list, Remove from Group.  They act on the window's selection,
 * which the caller has made this list.
 *
 * Inputs:
 *   list_id — the list
 *   menu    — the menu to add to
 */
- (void)
add_list_commands:(int64_t)list_id to:(NSMenu *)menu
{
    TasksLibraryWindow *library  = self.library;
    TaskList           *list     = task_db_list_get(library.app->db, list_id);
    int64_t             group_id = list != NULL ? list->group_id : 0;
    task_list_free(list);

    [menu addItem:tasks_command_menu_item(TasksCommandEditList, nil, library)];
    [menu addItem:tasks_command_menu_item(TasksCommandDeleteList, nil, library)];

    NSMenu    *groups = [[NSMenu alloc] initWithTitle:@""];
    GPtrArray *all    = task_db_groups(library.app->db);
    for (guint i = 0; i < all->len; i++) {
        const TaskGroup *group = g_ptr_array_index(all, i);
        if (group->id == group_id) continue;
        NSMenuItem *item = tasks_command_menu_item(TasksCommandMoveToGroup, @(group->id), library);
        item.title = tasks_string(group->name);
        [groups addItem:item];
    }
    task_ptr_array_free_groups(all);

    if (groups.numberOfItems > 0 || group_id != 0) {
        [menu addItem:NSMenuItem.separatorItem];
    }
    if (groups.numberOfItems > 0) {
        NSMenuItem *holder = [[NSMenuItem alloc]
            initWithTitle:tasks_command_spec(TasksCommandMoveToGroup)->title
                   action:NULL
            keyEquivalent:@""];
        holder.submenu = groups;
        [menu addItem:holder];
    }
    if (group_id != 0) {
        [menu addItem:tasks_command_menu_item(TasksCommandRemoveFromGroup, nil, library)];
    }
}

/*
 * menuForEvent: — the context menu for a right-click: New List and New
 * Group anywhere; a list button adds its list's commands; a group button
 * adds the group's own and, when the selection is a list of that group,
 * that list's commands too (a grouped list has no button of its own: its
 * group's button is lit for it).  A list clicked becomes the selection
 * first, since the list commands act on the selection; a group clicked
 * does not — its commands carry its id, and showing its tasks is the
 * left-click menu's All Tasks.
 *
 * Inputs:
 *   event — the right mouse-down
 *
 * Output:
 *   the menu.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    TasksLibraryWindow *library = self.library;
    NSMenu             *menu    = [[NSMenu alloc] initWithTitle:@""];
    if (!library || self.is_shut_down) return menu;

    TasksListsBtn *btn = [self button_at:[self.strip convertPoint:event.locationInWindow
                                                         fromView:nil]];
    task_sel_t     sel = btn != nil ? sel_of(btn) : (task_sel_t){ task_sel_header, 0 };

    /* The list of the clicked group that is selected, if that is the
     * selection. */
    int64_t    in_group = 0;
    task_sel_t current  = library.selection;
    if (sel.kind == task_sel_group && current.kind == task_sel_list) {
        TaskList *list = task_db_list_get(library.app->db, current.id);
        if (list != NULL && list->group_id == sel.id) in_group = current.id;
        task_list_free(list);
    }

    if (sel.kind == task_sel_list) {
        [library select:sel];
        [self update_selection_display];
    }

    [menu addItem:tasks_command_menu_item(TasksCommandNewList, nil, library)];
    [menu addItem:tasks_command_menu_item(TasksCommandNewGroup, nil, library)];

    if (sel.kind == task_sel_list) {
        [menu addItem:NSMenuItem.separatorItem];
        [self add_list_commands:sel.id to:menu];
    } else if (sel.kind == task_sel_group) {
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:tasks_command_menu_item(TasksCommandRenameGroup, @(sel.id), library)];
        [menu addItem:tasks_command_menu_item(TasksCommandDeleteGroup, @(sel.id), library)];
        if (in_group != 0) {
            TaskList *list  = task_db_list_get(library.app->db, in_group);
            char     *label = task_sidebar_list_label(list);
            /* A heading naming the list: not a command. */
            NSMenuItem *heading = [[NSMenuItem alloc] initWithTitle:tasks_string(label)
                                                             action:NULL
                                                      keyEquivalent:@""];
            heading.enabled = NO;
            [menu addItem:NSMenuItem.separatorItem];
            [menu addItem:heading];
            free(label);
            task_list_free(list);
            [self add_list_commands:in_group to:menu];
        }
    }
    return menu;
}

/* ── Dragging a list ─────────────────────────────────────────────────── */

/*
 * begin_drag_of:event: — a list's button was pressed and moved: start the
 * drag, with the button's own picture as what moves.  The button stays,
 * dimmed, until the drag ends.
 *
 * Inputs:
 *   button — the button, an ungrouped list's
 *   event  — the mouse-dragged event that crossed the threshold
 */
- (void)
begin_drag_of:(TasksListsBtn *)button event:(NSEvent *)event
{
    if (self.is_shut_down) return;

    NSPasteboardItem *content = [[NSPasteboardItem alloc] init];
    [content setString:@(sel_of(button).id).stringValue forType:bar_drag_type];

    NSImage *ghost = [[NSImage alloc] initWithData:[button dataWithPDFInsideRect:button.bounds]];
    NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:content];
    [item setDraggingFrame:button.bounds contents:ghost];

    self.dragged     = button;
    button.alphaValue = dragged_alpha;
    [button beginDraggingSessionWithItems:@[item] event:event source:self];
}

/*
 * draggingSession:sourceOperationMaskForDraggingContext: — a list may only
 * be moved, and only within the app.
 *
 * Inputs:
 *   session — the drag (unused)
 *   context — within the app, or outside it
 *
 * Output:
 *   NSDragOperationMove inside the app, NSDragOperationNone outside.
 */
- (NSDragOperation)
draggingSession:(NSDraggingSession *)session
sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    return context == NSDraggingContextWithinApplication ? NSDragOperationMove
                                                         : NSDragOperationNone;
}

/*
 * draggingSession:endedAtPoint:operation: — the drag is over, dropped or
 * not: the button comes back and the drop mark goes.
 *
 * Inputs:
 *   session   — the drag (unused)
 *   point     — where it ended (unused)
 *   operation — what the drop did (unused)
 */
- (void)
draggingSession:(NSDraggingSession *)session
   endedAtPoint:(NSPoint)point
      operation:(NSDragOperation)operation
{
    self.dragged.alphaValue = 1.0;
    self.dragged = nil;
    [self clear_drop_mark];
}

/*
 * clear_drop_mark — show no drop mark.
 */
- (void)
clear_drop_mark
{
    self.strip.drop_rule_x = -1.0;
    self.strip.drop_group  = NSZeroRect;
    [self.strip setNeedsDisplay:YES];
}

/*
 * resolve_drop_of:at:order:group:before:rule:frame: — where a list dropped
 * at a point would land.
 *
 *   - on a group's button: in that group, at its end;
 *   - anywhere else: among the ungrouped lists, in front of the first
 *     ungrouped list whose button's middle lies to the right of the point
 *     — so the left half of a list means "before it", the right half
 *     "after it", a point left of them all means the head, and the empty
 *     space past the last button means the end.
 *
 * A drop that would leave the list exactly where it is is refused, so no
 * mark is shown for it.
 *
 * Inputs:
 *   list_id — the list being dropped
 *   point   — the drop point, in the strip's coordinates
 *   order   — the lists' order (bar_order_load)
 *   group   — receives the group the list lands in, 0 for none
 *   before  — receives the index in order->ids of the list it lands in
 *             front of; order->n for "after the last" (task_sidebar.h)
 *   rule    — receives the x of the insertion mark, or -1 for none
 *   frame   — receives the frame of the group dropped on, or NSZeroRect
 *
 * Output:
 *   YES when the drop is allowed and the outputs are set.
 */
- (BOOL)
resolve_drop_of:(int64_t)list_id
             at:(NSPoint)point
          order:(const bar_order_t *)order
          group:(int64_t *)group
         before:(int *)before
           rule:(CGFloat *)rule
          frame:(NSRect *)frame
{
    int own = bar_order_index_of(order, list_id);
    if (own < 0) return NO;
    int64_t own_group = bar_order_group_at(order, own);

    *rule  = -1.0;
    *frame = NSZeroRect;

    NSArray<TasksListsBtn *> *buttons = [self buttons];
    TasksListsBtn            *hit     = [self button_at:point];

    if (hit != nil && sel_of(hit).kind == task_sel_group) {
        int64_t group_id = sel_of(hit).id;
        for (int g = 0; g < order->n_groups; g++) {
            if (order->group_ids[g] != group_id) continue;
            *group  = group_id;
            *before = order->group_ends[g];
            *frame  = hit.frame;
        }
        if (NSIsEmptyRect(*frame)) return NO;      /* a group not in the order */
    } else {
        *group  = 0;
        *before = order->n;
        *rule   = buttons.count > 0 ? NSMaxX(buttons.lastObject.frame) + btn_gap / 2.0
                                    : bar_pad;
        for (TasksListsBtn *btn in buttons) {
            task_sel_t sel = sel_of(btn);
            if (sel.kind != task_sel_list || NSMidX(btn.frame) <= point.x) continue;
            *before = bar_order_index_of(order, sel.id);
            *rule   = NSMinX(btn.frame) - btn_gap / 2.0;
            break;
        }
    }

    if (*group == own_group && (*before == own || *before == own + 1)) return NO;
    return YES;
}

/*
 * drop_list:at: — what a drop does.
 * Inputs/output are documented in lists_bar.h.
 */
- (BOOL)
drop_list:(int64_t)list_id at:(NSPoint)point
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down) return NO;

    TaskDatabase *db = library.app->db;
    bar_order_t   order;
    int64_t       group;
    int           before;
    CGFloat       rule;
    NSRect        frame;

    bar_order_load(db, &order);
    BOOL allowed = [self resolve_drop_of:list_id at:[self.strip convertPoint:point fromView:self]
                                   order:&order group:&group before:&before
                                    rule:&rule frame:&frame];
    if (allowed) {
        int64_t  own_group = bar_order_group_at(&order, bar_order_index_of(&order, list_id));
        gboolean saved     = group == own_group || task_db_list_set_group(db, list_id, group);

        /* The order is saved even when the move left it as it was (the
         * last list of a group dropped at the head of the ungrouped ones):
         * until an order has been saved the database lists alphabetically,
         * which would put a list that changed group somewhere other than
         * where it was dropped.  The cast: GLib spells the same 64-bit
         * integer as a different C type. */
        task_sidebar_move_list(order.ids, order.n, list_id, before);
        saved = saved && task_db_lists_reorder(db, (const gint64 *)order.ids, (gsize)order.n);

        /* The rebuild waits for the next run-loop pass: it replaces the
         * buttons while one of them may still be handling this drop.  A
         * failed write is reported there too: its refresh is the rebuild. */
        __weak TasksLibraryWindow *weak_library = library;
        dispatch_async(dispatch_get_main_queue(), ^{
            TasksLibraryWindow *window = weak_library;
            if (window == nil) return;
            if (saved) [window full_refresh];
            else task_app_write_failed(window.app, "The list\xe2\x80\x99s new place");
        });
    }
    bar_order_free(&order);
    return allowed;
}

/*
 * dragged_list_in: — the list a drag carries.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   the list id, or 0 when the drag is not a list of this bar's.
 */
static int64_t
dragged_list_in(id<NSDraggingInfo> info)
{
    NSString *text = [info.draggingPasteboard stringForType:bar_drag_type];
    return text != nil ? text.longLongValue : 0;
}

/*
 * draggingEntered: — a drag came over the bar: as draggingUpdated:.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   as draggingUpdated:.
 */
- (NSDragOperation)
draggingEntered:(id<NSDraggingInfo>)info
{
    return [self draggingUpdated:info];
}

/*
 * draggingUpdated: — a drag moved over the bar: show where the list would
 * land, or no mark where it may not.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   NSDragOperationMove where a drop is allowed, else NSDragOperationNone.
 */
- (NSDragOperation)
draggingUpdated:(id<NSDraggingInfo>)info
{
    int64_t list_id = dragged_list_in(info);
    if (list_id == 0 || !self.library || self.is_shut_down) return NSDragOperationNone;

    bar_order_t order;
    int64_t     group;
    int         before;
    CGFloat     rule;
    NSRect      frame;

    bar_order_load(self.library.app->db, &order);
    BOOL allowed = [self resolve_drop_of:list_id
                                      at:[self.strip convertPoint:info.draggingLocation fromView:nil]
                                   order:&order group:&group before:&before
                                    rule:&rule frame:&frame];
    bar_order_free(&order);

    self.strip.drop_rule_x = allowed ? rule  : -1.0;
    self.strip.drop_group  = allowed ? frame : NSZeroRect;
    [self.strip setNeedsDisplay:YES];
    return allowed ? NSDragOperationMove : NSDragOperationNone;
}

/*
 * draggingExited: — the drag left the bar: no mark.
 *
 * Inputs:
 *   info — the drag (unused)
 */
- (void)
draggingExited:(id<NSDraggingInfo>)info
{
    [self clear_drop_mark];
}

/*
 * performDragOperation: — the list was dropped.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   YES when the list was moved.
 */
- (BOOL)
performDragOperation:(id<NSDraggingInfo>)info
{
    [self clear_drop_mark];
    int64_t list_id = dragged_list_in(info);
    if (list_id == 0) return NO;
    return [self drop_list:list_id at:[self convertPoint:info.draggingLocation fromView:nil]];
}

/* ── List and group sheets ───────────────────────────────────────────── */

/*
 * save_list:name:emoji: — the list sheet was accepted: create the list, or
 * update the one being edited.  A new list becomes the selection.  An
 * empty name does nothing.
 *
 * Inputs:
 *   list_id — the list being edited, or 0 to create one
 *   name    — the name as typed, already trimmed
 *   emoji   — the emoji, @"" for none
 */
- (void)
save_list:(int64_t)list_id name:(NSString *)name emoji:(NSString *)emoji
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down || name.length == 0) return;

    TaskApp *app = library.app;
    if (list_id != 0) {
        if (!task_db_list_update(app->db, list_id, name.UTF8String, emoji.UTF8String)) {
            task_app_write_failed(app, "The list\xe2\x80\x99s new name");
            return;
        }
        [library full_refresh];
        task_app_status(app, "Updated list \xe2\x80\x9c%s\xe2\x80\x9d", name.UTF8String);
        return;
    }

    int64_t created = task_db_list_create(app->db, name.UTF8String, emoji.UTF8String);
    if (created == 0) {
        task_app_write_failed(app, "The new list");
        return;
    }
    [library select:(task_sel_t){ task_sel_list, created }];
    [library full_refresh];
    task_app_status(app, "Created list \xe2\x80\x9c%s\xe2\x80\x9d", name.UTF8String);
}

/*
 * show_list_sheet: — the New List / Edit List sheet: an emoji slot and the
 * name.  The name has the keyboard when the sheet opens.
 *
 * Inputs:
 *   list_id — the list to edit, or 0 for a new one
 */
- (void)
show_list_sheet:(int64_t)list_id
{
    TasksLibraryWindow *library = self.library;
    TaskList *list = list_id != 0 ? task_db_list_get(library.app->db, list_id) : NULL;
    if (list_id != 0 && list == NULL) return;

    TasksEmojiField *emoji = [[TasksEmojiField alloc] initWithFrame:NSZeroRect];
    emoji.stringValue       = tasks_string(list != NULL ? list->emoji : "");
    emoji.placeholderString = emoji_placeholder;
    emoji.toolTip           = emoji_tooltip;
    emoji.alignment         = NSTextAlignmentCenter;
    [emoji.widthAnchor constraintEqualToConstant:emoji_field_w].active = YES;

    NSTextField *name = name_field(tasks_string(list != NULL ? list->name : ""), @"List name");
    task_list_free(list);

    tasks_ui_sheet(library.window,
                   sheet_content(list_id != 0 ? @"Edit List" : @"New List", @[emoji, name]),
                   list_id != 0 ? @"Save" : @"Create", ^(BOOL accepted) {
        if (accepted) [self save_list:list_id name:trimmed(name) emoji:emoji.stringValue];
    });

    /* The emoji slot is the sheet's first field and would take the
     * keyboard.  The name is what is typed, so it gets it instead, one
     * run-loop pass later: the sheet sets its own first responder as it
     * appears, and would undo a choice made now. */
    __weak NSTextField *weak_name = name;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSTextField *field = weak_name;
        [field.window makeFirstResponder:field];
    });
}

/*
 * save_group:name: — the group sheet was accepted: create the group, or
 * rename the one being edited.  An empty name does nothing.
 *
 * Inputs:
 *   group_id — the group being renamed, or 0 to create one
 *   name     — the name as typed, already trimmed
 */
- (void)
save_group:(int64_t)group_id name:(NSString *)name
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down || name.length == 0) return;

    TaskApp *app = library.app;
    if (group_id != 0 ? !task_db_group_rename(app->db, group_id, name.UTF8String)
                      : task_db_group_create(app->db, name.UTF8String) == 0) {
        task_app_write_failed(app, group_id != 0 ? "The group\xe2\x80\x99s new name"
                                                 : "The new group");
        return;
    }
    [library full_refresh];
}

/*
 * show_group_sheet: — the New Group / Rename Group sheet: one name field.
 *
 * Inputs:
 *   group_id — the group to rename, or 0 for a new one
 */
- (void)
show_group_sheet:(int64_t)group_id
{
    TasksLibraryWindow *library = self.library;
    TaskGroup *group = group_id != 0 ? task_db_group_get(library.app->db, group_id) : NULL;
    if (group_id != 0 && group == NULL) return;

    NSTextField *name = name_field(tasks_string(group != NULL ? group->name : ""), @"Group name");
    task_group_free(group);

    tasks_ui_sheet(library.window,
                   sheet_content(group_id != 0 ? @"Rename Group" : @"New Group", @[name]),
                   group_id != 0 ? @"Rename" : @"Create", ^(BOOL accepted) {
        if (accepted) [self save_group:group_id name:trimmed(name)];
    });
}

/* ── Deleting ────────────────────────────────────────────────────────── */

/*
 * close_editors_of_list: — close the editor of every task in a list that
 * is about to be deleted.  A closing editor flushes its pending save, and
 * that must land before the tasks go, not on rows already deleted.
 *
 * Inputs:
 *   app     — the application context
 *   list_id — the list
 */
static void
close_editors_of_list(TaskApp *app, int64_t list_id)
{
    GPtrArray *tasks = task_db_tasks_in_list_all(app->db, list_id);

    for (guint i = 0; i < tasks->len; i++) {
        tasks_editor_close(((const Task *)g_ptr_array_index(tasks, i))->id);
    }
    task_ptr_array_free_tasks(tasks);
}

/*
 * delete_list: — ask, then delete a list and every task in it, and forget
 * the orders saved for it.  The selection falls to the first list (reload).
 *
 * Inputs:
 *   list_id — the list
 */
- (void)
delete_list:(int64_t)list_id
{
    TasksLibraryWindow *library = self.library;
    TaskList *list = task_db_list_get(library.app->db, list_id);
    if (list == NULL) return;

    NSString *name  = tasks_string(list->name);
    NSString *title = [NSString stringWithFormat:@"Delete the list “%@” and all of its tasks?",
                       name];
    task_list_free(list);

    tasks_ui_confirm(library.window, title, @"You cannot undo this action.", @"Delete", YES,
                     ^(BOOL confirmed) {
        TasksLibraryWindow *window = self.library;
        if (!confirmed || !window || self.is_shut_down) return;

        TaskApp *app = window.app;
        close_editors_of_list(app, list_id);
        if (!task_db_list_delete(app->db, list_id)) {
            task_app_write_failed(app, "Deleting the list");
            return;
        }
        task_order_drop((task_sel_t){ task_sel_list, list_id });
        [window full_refresh];
        task_app_status(app, "Deleted list \xe2\x80\x9c%s\xe2\x80\x9d", name.UTF8String);
    });
}

/*
 * delete_group: — ask, then remove a group and forget the orders saved for
 * its aggregate view.  Its lists stay, ungrouped; if the group was the
 * selection, that falls to the first list (reload).
 *
 * Inputs:
 *   group_id — the group
 */
- (void)
delete_group:(int64_t)group_id
{
    TasksLibraryWindow *library = self.library;
    TaskGroup *group = task_db_group_get(library.app->db, group_id);
    if (group == NULL) return;

    NSString *title = [NSString stringWithFormat:@"Remove the group “%@”?",
                       tasks_string(group->name)];
    task_group_free(group);

    tasks_ui_confirm(library.window, title, @"Its lists will become ungrouped.", @"Remove", NO,
                     ^(BOOL confirmed) {
        TasksLibraryWindow *window = self.library;
        if (!confirmed || !window || self.is_shut_down) return;

        if (!task_db_group_delete(window.app->db, group_id)) {
            task_app_write_failed(window.app, "Removing the group");
            return;
        }
        task_order_drop((task_sel_t){ task_sel_group, group_id });
        [window full_refresh];
    });
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * set_group_of_selected_list: — file the selected list under a group, or
 * under none.
 *
 * Inputs:
 *   group_id — the group, or 0 to take the list out of its group
 */
- (void)
set_group_of_selected_list:(int64_t)group_id
{
    TasksLibraryWindow *library = self.library;
    int64_t list_id = task_sel_list_id(library.selection);
    if (list_id == 0) return;

    if (!task_db_list_set_group(library.app->db, list_id, group_id)) {
        task_app_write_failed(library.app, "The list\xe2\x80\x99s group");
        return;
    }
    [library full_refresh];
}

/*
 * perform_command:param: — carry out one of the bar's commands.
 * Inputs are documented in lists_bar.h.
 *
 * Edit List and Delete List act on the window's selection.  Delete List
 * with a GROUP selected removes that group: its title says so
 * (update_state:for_command:).
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
    TasksLibraryWindow *library = self.library;
    if (!library || self.is_shut_down) return;

    task_sel_t selection = library.selection;
    switch (command) {
    case TasksCommandNewList:
        [self show_list_sheet:0];
        break;
    case TasksCommandNewGroup:
        [self show_group_sheet:0];
        break;
    case TasksCommandEditList:
        if (selection.kind == task_sel_list) [self show_list_sheet:selection.id];
        break;
    case TasksCommandDeleteList:
        if (selection.kind == task_sel_list) [self delete_list:selection.id];
        if (selection.kind == task_sel_group) [self delete_group:selection.id];
        break;
    case TasksCommandMoveToGroup:
        [self set_group_of_selected_list:param.longLongValue];
        break;
    case TasksCommandRemoveFromGroup:
        [self set_group_of_selected_list:0];
        break;
    case TasksCommandRenameGroup:
        [self show_group_sheet:param.longLongValue];
        break;
    case TasksCommandDeleteGroup:
        [self delete_group:param.longLongValue];
        break;
    default:
        break;                      /* not one of the bar's */
    }
}

/*
 * update_state:for_command: — the current look of one of the bar's
 * commands.  A command that has nothing to act on is disabled rather than
 * left to refuse when chosen.
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
        break;                      /* New List, New Group: always available */
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
