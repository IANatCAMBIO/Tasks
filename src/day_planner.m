#import "day_planner.h"
#import "editor_window.h"
#import "look.h"
#import "ui_util.h"

#include "db.h"
#include "task_app.h"
#include "task_config.h"

#include <math.h>
#include <time.h>

/* The pasteboard types the two task panes put on the board. */
static NSString * const kanban_drag_type = @"com.cambio.tasks-appkit.kanban-card";
static NSString * const list_drag_type   = @"tasks.task-row";

/* Snap granularity for time placement, resize and move. */
static const NSInteger snap_minutes  = 15;

/* Height of the date-navigation header above the scroll area. */
static const CGFloat   header_h      = 36.0;

/* ── TasksDayItem ──────────────────────────────────────────────────────── */

@interface TasksDayItem : NSObject
@property (nonatomic) int64_t task_id;
@property (nonatomic, copy) NSString *title;
@property (nonatomic) int64_t start_unix;
@property (nonatomic) int duration;          /* minutes */
@property (nonatomic) TaskStatus status;
@end

@implementation TasksDayItem
@end

/* ── Forward declaration ─────────────────────────────────────────────── */

@class TasksDayCanvas;

/* ── TasksDayPlanner private interface (visible to the canvas below) ─── */

@interface TasksDayPlanner ()
@property (nonatomic, weak)   TasksLibraryWindow            *library;
@property (nonatomic, strong) NSScrollView                  *scroll;
@property (nonatomic, strong) TasksDayCanvas                *canvas;
@property (nonatomic, strong) NSTimer                       *clock_timer;
@property (nonatomic, strong) NSTimer                       *progress_timer;
@property (nonatomic, strong) NSMutableArray<TasksDayItem *> *items;
@property (nonatomic)         BOOL                           is_shut_down;
@property (nonatomic)         NSInteger                      days_offset; /* 0 = today */
@property (nonatomic, weak)   NSDatePicker                  *date_picker;
@property (nonatomic, weak)   NSButton                      *today_button;
@end

/* ── TasksDayCanvas ────────────────────────────────────────────────────── */

@interface TasksDayCanvas : NSView <NSDraggingDestination>
@property (nonatomic, weak) TasksDayPlanner *planner;
/* Resize tracking */
@property (nonatomic) BOOL    resizing;
@property (nonatomic) int64_t resize_id;
@property (nonatomic) CGFloat resize_start_y;
@property (nonatomic) int     resize_orig_duration;
/* Move tracking */
@property (nonatomic) BOOL    moving;
@property (nonatomic) int64_t move_id;
@property (nonatomic) int64_t move_orig_start_unix;
@property (nonatomic) CGFloat move_start_mouse_y;
/* Drop highlight Y, NAN when no drag is over */
@property (nonatomic) CGFloat drop_y;
@end

@implementation TasksDayCanvas

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

/* ── Coordinate helpers ───────────────────────────────────────────────── */

/*
 * midnight_for_offset — unix seconds of local midnight for today + offset days.
 *
 * Inputs:
 *   offset — days relative to today; 0 = today, 1 = tomorrow, -1 = yesterday
 *
 * Output:
 *   unix seconds of that day's local midnight.
 */
static int64_t
midnight_for_offset(NSInteger offset)
{
    time_t    now   = time(NULL);
    struct tm local = *localtime(&now);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_mday += (int)offset;
    return (int64_t)mktime(&local);
}

/*
 * y_for_unix: — canvas Y for a unix timestamp on the displayed day.
 *
 * Inputs:
 *   t — unix seconds
 *
 * Output:
 *   Y in flipped canvas coordinates (Y=0 at top).
 */
- (CGFloat)
y_for_unix:(int64_t)t
{
    NSInteger offset   = self.planner ? self.planner.days_offset : 0;
    double    hours    = (double)(t - midnight_for_offset(offset)) / 3600.0;
    return tasks_look_day_planner_pad_v +
           (hours - (double)tasks_look_day_planner_start_h) * tasks_look_day_planner_hour_h;
}

/*
 * unix_for_y: — unix timestamp for a canvas Y coordinate, snapped to
 * snap_minutes and clamped to the visible hour range of the displayed day.
 *
 * Inputs:
 *   y — Y in flipped canvas coordinates
 *
 * Output:
 *   unix seconds within the displayed day.
 */
- (int64_t)
unix_for_y:(CGFloat)y
{
    NSInteger offset   = self.planner ? self.planner.days_offset : 0;
    double    hours    = (y - tasks_look_day_planner_pad_v) / tasks_look_day_planner_hour_h
                         + (double)tasks_look_day_planner_start_h;
    int total_min = (int)round(hours * 60.0);
    total_min = (total_min / (int)snap_minutes) * (int)snap_minutes;
    int lo_min = (int)tasks_look_day_planner_start_h * 60;
    int hi_min = (int)tasks_look_day_planner_end_h   * 60 - (int)snap_minutes;
    if (total_min < lo_min) total_min = lo_min;
    if (total_min > hi_min) total_min = hi_min;
    return midnight_for_offset(offset) + (int64_t)total_min * 60;
}

/*
 * item_rect: — canvas rectangle for a scheduled item.
 *
 * Inputs:
 *   item — the item
 *
 * Output:
 *   block frame in canvas coordinates.
 */
- (NSRect)
item_rect:(TasksDayItem *)item
{
    CGFloat x = tasks_look_day_planner_label_w + tasks_look_day_planner_block_inset;
    CGFloat w = NSWidth(self.bounds) - x - tasks_look_day_planner_block_inset;
    CGFloat y = [self y_for_unix:item.start_unix];
    CGFloat h = (item.duration / 60.0) * tasks_look_day_planner_hour_h;
    CGFloat min_h = tasks_look_day_planner_resize_h * 2.0 + 4.0;
    if (h < min_h) h = min_h;
    return NSMakeRect(x, y, w, h);
}

/* ── Drawing ──────────────────────────────────────────────────────────── */

/*
 * drawRect: — draw hour grid, task blocks, drop indicator, current time line.
 *
 * Inputs:
 *   dirty — the rectangle to redraw
 */
- (void)
drawRect:(NSRect)dirty
{
    NSInteger start_h = tasks_look_day_planner_start_h;
    NSInteger end_h   = tasks_look_day_planner_end_h;
    CGFloat   hour_h  = tasks_look_day_planner_hour_h;
    CGFloat   pad     = tasks_look_day_planner_pad_v;
    CGFloat   label_w = tasks_look_day_planner_label_w;

    [NSColor.textBackgroundColor setFill];
    NSRectFill(dirty);

    /* Thin separator between label column and block area */
    NSRect col_rule = NSMakeRect(label_w - 0.5, 0, 1.0, NSHeight(self.bounds));
    if (NSIntersectsRect(col_rule, dirty)) {
        [NSColor.separatorColor setFill];
        NSRectFill(col_rule);
    }

    NSFont *label_font = [NSFont monospacedDigitSystemFontOfSize:10.0
                                                          weight:NSFontWeightRegular];
    NSDictionary *label_attrs = @{
        NSFontAttributeName:            label_font,
        NSForegroundColorAttributeName: NSColor.secondaryLabelColor,
    };

    for (NSInteger h = start_h; h <= end_h; h++) {
        CGFloat y = pad + (h - start_h) * hour_h;

        NSRect line = NSMakeRect(0, y - 0.5, NSWidth(self.bounds), 1.0);
        if (NSIntersectsRect(line, dirty)) {
            [NSColor.separatorColor setFill];
            NSRectFill(line);
        }

        if (h < end_h) {
            NSString *text;
            if (h == 0)       text = @"12 AM";
            else if (h < 12)  text = [NSString stringWithFormat:@"%ld AM", (long)h];
            else if (h == 12) text = @"12 PM";
            else               text = [NSString stringWithFormat:@"%ld PM", (long)(h - 12)];
            NSSize  sz   = [text sizeWithAttributes:label_attrs];
            NSRect  lrec = NSMakeRect(2.0, y + 2.0, label_w - 4.0, sz.height);
            if (NSIntersectsRect(lrec, dirty))
                [text drawInRect:lrec withAttributes:label_attrs];
        }
    }

    /* Task blocks */
    NSColor *acc_fill   = [NSColor.controlAccentColor colorWithAlphaComponent:0.18];
    NSColor *acc_border = [NSColor.controlAccentColor colorWithAlphaComponent:0.55];
    NSColor *done_fill  = [NSColor.secondaryLabelColor colorWithAlphaComponent:0.10];
    NSColor *done_bord  = [NSColor.secondaryLabelColor colorWithAlphaComponent:0.28];
    NSDictionary *text_attrs = @{
        NSFontAttributeName:            [NSFont systemFontOfSize:tasks_look_row_small_font],
        NSForegroundColorAttributeName: NSColor.labelColor,
    };

    for (TasksDayItem *item in self.planner.items) {
        NSRect r = [self item_rect:item];
        if (!NSIntersectsRect(r, dirty)) continue;

        NSBezierPath *path =
            [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 0.5, 0.5)
                                            xRadius:4.0 yRadius:4.0];
        if (item.status == TASK_STATUS_DONE) {
            [done_fill setFill];  [path fill];
            [done_bord setStroke]; [path stroke];
        } else {
            [acc_fill setFill];   [path fill];
            [acc_border setStroke]; [path stroke];
        }

        NSRect text_rect = NSInsetRect(r, 4.0, 3.0);
        text_rect.size.height -= tasks_look_day_planner_resize_h;
        [item.title drawInRect:text_rect withAttributes:text_attrs];

        /* Resize grip stripe */
        NSRect grip = NSMakeRect(r.origin.x,
                                 NSMaxY(r) - tasks_look_day_planner_resize_h,
                                 r.size.width, tasks_look_day_planner_resize_h);
        [[NSColor.separatorColor colorWithAlphaComponent:0.28] setFill];
        NSRectFillUsingOperation(grip, NSCompositingOperationSourceOver);
    }

    /* Drop indicator */
    if (!isnan(self.drop_y)) {
        NSRect mark = NSMakeRect(label_w, self.drop_y - 1.0,
                                 NSWidth(self.bounds) - label_w, 2.0);
        [NSColor.controlAccentColor setFill];
        NSRectFill(mark);
    }

    /* Current-time line: only shown when viewing today */
    if (!self.planner || self.planner.days_offset == 0) {
        CGFloat now_y = [self y_for_unix:(int64_t)time(NULL)];
        if (now_y >= pad && now_y <= NSMaxY(self.bounds) - pad) {
            NSRect tl = NSMakeRect(label_w - 5.0,
                                   now_y - tasks_look_day_planner_time_w / 2.0,
                                   NSWidth(self.bounds) - label_w + 5.0,
                                   tasks_look_day_planner_time_w);
            [[NSColor systemRedColor] setFill];
            NSRectFill(tl);
            /* Small circle at the left edge */
            NSRect dot = NSMakeRect(label_w - 7.0, now_y - 4.0, 8.0, 8.0);
            [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
        }
    }
}

/* ── Drag destination ─────────────────────────────────────────────────── */

/*
 * draggingEntered: — a drag entered the canvas: accept if it carries task ids.
 *
 * Inputs:
 *   info — the dragging session
 *
 * Output:
 *   NSDragOperationLink on acceptance, NSDragOperationNone otherwise.
 */
- (NSDragOperation)
draggingEntered:(id<NSDraggingInfo>)info
{
    NSPasteboard *pb = info.draggingPasteboard;
    BOOL ok = ([pb availableTypeFromArray:@[list_drag_type]]   != nil ||
               [pb availableTypeFromArray:@[kanban_drag_type]] != nil);
    if (!ok) return NSDragOperationNone;

    NSPoint pt = [self convertPoint:info.draggingLocation fromView:nil];
    self.drop_y = pt.y;
    [self setNeedsDisplay:YES];
    return NSDragOperationLink;
}

/*
 * draggingUpdated: — the drag moved; update the indicator line.
 *
 * Inputs:
 *   info — the dragging session
 *
 * Output:
 *   NSDragOperationLink.
 */
- (NSDragOperation)
draggingUpdated:(id<NSDraggingInfo>)info
{
    NSPoint pt = [self convertPoint:info.draggingLocation fromView:nil];
    self.drop_y = pt.y;
    [self setNeedsDisplay:YES];
    return NSDragOperationLink;
}

/*
 * draggingExited: — the drag left without dropping; clear the indicator.
 *
 * Inputs:
 *   info — the dragging session (unused)
 */
- (void)
draggingExited:(id<NSDraggingInfo>)info
{
    self.drop_y = NAN;
    [self setNeedsDisplay:YES];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info { return YES; }

/*
 * performDragOperation: — schedule each dropped task at the target time.
 *
 * Inputs:
 *   info — the dragging session; its pasteboard holds the task ids
 *
 * Output:
 *   YES on success.
 */
- (BOOL)
performDragOperation:(id<NSDraggingInfo>)info
{
    self.drop_y = NAN;
    NSPoint pt    = [self convertPoint:info.draggingLocation fromView:nil];
    int64_t start = [self unix_for_y:pt.y];

    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    NSPasteboard *pb = info.draggingPasteboard;

    /* Kanban puts a comma-separated list in one item */
    NSString *kanban_str = [pb stringForType:kanban_drag_type];
    if (kanban_str.length > 0) {
        for (NSString *part in [kanban_str componentsSeparatedByString:@","]) {
            int64_t tid = part.longLongValue;
            if (tid > 0) [ids addObject:@(tid)];
        }
    }

    /* Task list puts one item per row */
    for (NSPasteboardItem *item in pb.pasteboardItems) {
        NSString *s = [item stringForType:list_drag_type];
        if (s.length > 0) {
            int64_t tid = s.longLongValue;
            if (tid > 0 && ![ids containsObject:@(tid)])
                [ids addObject:@(tid)];
        }
    }

    if (ids.count == 0) return NO;

    TaskApp   *app        = self.planner.library.app;
    NSInteger  offset_min = 0;
    for (NSNumber *tid in ids) {
        task_db_task_set_scheduled(app->db, tid.longLongValue,
                                   start + offset_min * 60, 60);
        offset_min += (NSInteger)snap_minutes;
    }
    [self.planner reload];
    return YES;
}

/*
 * concludeDragOperation: — clear the indicator after the drop.
 *
 * Inputs:
 *   info — the dragging session (unused)
 */
- (void)
concludeDragOperation:(id<NSDraggingInfo>)info
{
    self.drop_y = NAN;
    [self setNeedsDisplay:YES];
}

/* ── Mouse events (block move and resize, double-click to edit) ─────── */

/*
 * mouseDown: — begin a resize on the grip stripe, a move on the block body,
 * or open the editor on a double-click.
 *
 * Inputs:
 *   event — the mouse-down event
 */
- (void)
mouseDown:(NSEvent *)event
{
    NSPoint pt     = [self convertPoint:event.locationInWindow fromView:nil];
    self.resize_id = 0;
    self.resizing  = NO;
    self.move_id   = 0;
    self.moving    = NO;

    for (TasksDayItem *item in self.planner.items) {
        NSRect r = [self item_rect:item];
        if (!NSPointInRect(pt, r)) continue;

        /* Double-click: open the editor */
        if (event.clickCount == 2) {
            tasks_editor_open(self.planner.library.app, item.task_id);
            return;
        }

        NSRect grip = NSMakeRect(r.origin.x,
                                 NSMaxY(r) - tasks_look_day_planner_resize_h,
                                 r.size.width, tasks_look_day_planner_resize_h);
        if (NSPointInRect(pt, grip)) {
            self.resizing             = YES;
            self.resize_id            = item.task_id;
            self.resize_start_y       = pt.y;
            self.resize_orig_duration = item.duration;
        } else {
            /* Block body: drag to move */
            self.moving               = YES;
            self.move_id              = item.task_id;
            self.move_orig_start_unix = item.start_unix;
            self.move_start_mouse_y   = pt.y;
        }
        return;
    }
}

/*
 * mouseDragged: — update a block's duration (resize) or start time (move)
 * while the drag is live.
 *
 * Inputs:
 *   event — the mouse-dragged event
 */
- (void)
mouseDragged:(NSEvent *)event
{
    NSPoint pt = [self convertPoint:event.locationInWindow fromView:nil];

    if (self.resizing) {
        CGFloat dy    = pt.y - self.resize_start_y;
        int     delta = (int)round((dy / tasks_look_day_planner_hour_h) * 60.0);
        delta = (delta / (int)snap_minutes) * (int)snap_minutes;
        int new_dur = self.resize_orig_duration + delta;
        if (new_dur < (int)snap_minutes) new_dur = (int)snap_minutes;
        if (new_dur > 8 * 60)           new_dur = 8 * 60;
        for (TasksDayItem *item in self.planner.items) {
            if (item.task_id == self.resize_id) {
                item.duration = new_dur;
                break;
            }
        }
        [self setNeedsDisplay:YES];
        return;
    }

    if (self.moving) {
        CGFloat dy        = pt.y - self.move_start_mouse_y;
        int     delta_min = (int)round((dy / tasks_look_day_planner_hour_h) * 60.0);
        delta_min = (delta_min / (int)snap_minutes) * (int)snap_minutes;
        for (TasksDayItem *item in self.planner.items) {
            if (item.task_id != self.move_id) continue;
            int64_t   new_start = self.move_orig_start_unix + (int64_t)delta_min * 60;
            NSInteger offset    = self.planner ? self.planner.days_offset : 0;
            int64_t   mid       = midnight_for_offset(offset);
            int64_t   lo        = mid + (int64_t)tasks_look_day_planner_start_h * 3600;
            int64_t   hi        = mid + (int64_t)tasks_look_day_planner_end_h   * 3600
                                  - (int64_t)item.duration * 60;
            if (new_start < lo) new_start = lo;
            if (new_start > hi) new_start = hi;
            item.start_unix = new_start;
            break;
        }
        [self setNeedsDisplay:YES];
        return;
    }
}

/*
 * mouseUp: — commit the new duration (resize) or start time (move) to the DB.
 *
 * Inputs:
 *   event — the mouse-up event (unused)
 */
- (void)
mouseUp:(NSEvent *)event
{
    if (self.resizing) {
        self.resizing = NO;
        for (TasksDayItem *item in self.planner.items) {
            if (item.task_id == self.resize_id) {
                task_db_task_set_scheduled(self.planner.library.app->db,
                                           item.task_id,
                                           item.start_unix,
                                           item.duration);
                break;
            }
        }
        self.resize_id = 0;
        return;
    }

    if (self.moving) {
        self.moving = NO;
        for (TasksDayItem *item in self.planner.items) {
            if (item.task_id == self.move_id) {
                task_db_task_set_scheduled(self.planner.library.app->db,
                                           item.task_id,
                                           item.start_unix,
                                           item.duration);
                break;
            }
        }
        self.move_id = 0;
        return;
    }
}

/*
 * resetCursorRects — open-hand cursor on block bodies; resize cursor on grips.
 */
- (void)
resetCursorRects
{
    [self addCursorRect:self.bounds cursor:NSCursor.arrowCursor];
    for (TasksDayItem *item in self.planner.items) {
        NSRect r    = [self item_rect:item];
        CGFloat body_h = r.size.height - tasks_look_day_planner_resize_h;
        NSRect body = NSMakeRect(r.origin.x, r.origin.y, r.size.width, body_h);
        NSRect grip = NSMakeRect(r.origin.x,
                                 NSMaxY(r) - tasks_look_day_planner_resize_h,
                                 r.size.width, tasks_look_day_planner_resize_h);
        [self addCursorRect:body cursor:NSCursor.openHandCursor];
        [self addCursorRect:grip cursor:NSCursor.resizeUpDownCursor];
    }
}

@end

/* ── TasksDayPlanner ──────────────────────────────────────────────────── */

@implementation TasksDayPlanner

/*
 * init_with_library: — build the panel.
 * Inputs/output are documented in day_planner.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library     = library;
    _items       = [NSMutableArray array];
    _days_offset = 0;

    NSInteger n_hours  = tasks_look_day_planner_end_h - tasks_look_day_planner_start_h;
    CGFloat   canvas_h = tasks_look_day_planner_pad_v * 2.0 +
                         n_hours * tasks_look_day_planner_hour_h;

    _canvas         = [[TasksDayCanvas alloc]
                           initWithFrame:NSMakeRect(0, 0, tasks_look_day_planner_w, canvas_h)];
    _canvas.planner = self;
    _canvas.drop_y  = NAN;
    [_canvas registerForDraggedTypes:@[list_drag_type, kanban_drag_type]];

    _scroll                       = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.documentView          = _canvas;
    _scroll.hasVerticalScroller   = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers    = YES;
    _scroll.drawsBackground       = NO;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;

    [self build_header];

    __weak TasksDayPlanner *ws = self;

    _clock_timer = [NSTimer scheduledTimerWithTimeInterval:30.0
                                                   repeats:YES
                                                     block:^(NSTimer *t) {
        [ws.canvas setNeedsDisplay:YES];
    }];

    _progress_timer = [NSTimer scheduledTimerWithTimeInterval:60.0
                                                      repeats:YES
                                                        block:^(NSTimer *t) {
        [ws check_and_progress];
    }];

    return self;
}

/*
 * build_header — add the date navigation bar and the scroll view, wired with
 * Auto Layout.  Layout: [‹]  [Today]  [date-picker]  [›]
 * Called once from init.
 */
- (void)
build_header
{
    NSView *header = [[NSView alloc] initWithFrame:NSZeroRect];
    header.translatesAutoresizingMaskIntoConstraints = NO;

    NSButton *prev     = [NSButton buttonWithTitle:@""
                                            target:self
                                            action:@selector(go_prev_day:)];
    prev.image         = [NSImage imageWithSystemSymbolName:@"chevron.left"
                                       accessibilityDescription:nil];
    prev.imagePosition = NSImageOnly;
    prev.bordered      = NO;
    prev.translatesAutoresizingMaskIntoConstraints = NO;

    NSButton *next     = [NSButton buttonWithTitle:@""
                                            target:self
                                            action:@selector(go_next_day:)];
    next.image         = [NSImage imageWithSystemSymbolName:@"chevron.right"
                                       accessibilityDescription:nil];
    next.imagePosition = NSImageOnly;
    next.bordered      = NO;
    next.translatesAutoresizingMaskIntoConstraints = NO;

    NSButton *today         = [NSButton buttonWithTitle:@"Today"
                                                 target:self
                                                 action:@selector(go_today:)];
    today.controlSize       = NSControlSizeSmall;
    today.bezelStyle        = NSBezelStyleRounded;
    today.translatesAutoresizingMaskIntoConstraints = NO;
    _today_button = today;

    NSDatePicker *picker       = [[NSDatePicker alloc] initWithFrame:NSZeroRect];
    picker.datePickerStyle     = NSDatePickerStyleTextField;
    picker.datePickerElements  = NSDatePickerElementFlagYearMonthDay;
    picker.controlSize         = NSControlSizeSmall;
    picker.dateValue           = [NSDate date];
    picker.target              = self;
    picker.action              = @selector(date_picker_changed:);
    picker.translatesAutoresizingMaskIntoConstraints = NO;
    _date_picker = picker;

    [header addSubview:prev];
    [header addSubview:today];
    [header addSubview:picker];
    [header addSubview:next];

    [NSLayoutConstraint activateConstraints:@[
        [prev.leadingAnchor   constraintEqualToAnchor:header.leadingAnchor constant:4.0],
        [prev.centerYAnchor   constraintEqualToAnchor:header.centerYAnchor],

        [today.leadingAnchor  constraintEqualToAnchor:prev.trailingAnchor  constant:2.0],
        [today.centerYAnchor  constraintEqualToAnchor:header.centerYAnchor],

        [picker.leadingAnchor constraintEqualToAnchor:today.trailingAnchor constant:4.0],
        [picker.centerYAnchor constraintEqualToAnchor:header.centerYAnchor],

        [next.leadingAnchor   constraintEqualToAnchor:picker.trailingAnchor constant:2.0],
        [next.centerYAnchor   constraintEqualToAnchor:header.centerYAnchor],
        [next.trailingAnchor  constraintLessThanOrEqualToAnchor:header.trailingAnchor constant:-4.0],
    ]];

    /* 1 pt separator below the header */
    NSView *sep = [[NSView alloc] initWithFrame:NSZeroRect];
    sep.wantsLayer = YES;
    sep.layer.backgroundColor = NSColor.separatorColor.CGColor;
    sep.translatesAutoresizingMaskIntoConstraints = NO;

    [self addSubview:header];
    [self addSubview:sep];
    [self addSubview:_scroll];

    [NSLayoutConstraint activateConstraints:@[
        [header.topAnchor      constraintEqualToAnchor:self.topAnchor],
        [header.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [header.heightAnchor   constraintEqualToConstant:header_h],

        [sep.topAnchor         constraintEqualToAnchor:header.bottomAnchor],
        [sep.leadingAnchor     constraintEqualToAnchor:self.leadingAnchor],
        [sep.trailingAnchor    constraintEqualToAnchor:self.trailingAnchor],
        [sep.heightAnchor      constraintEqualToConstant:1.0],

        [_scroll.topAnchor     constraintEqualToAnchor:sep.bottomAnchor],
        [_scroll.bottomAnchor  constraintEqualToAnchor:self.bottomAnchor],
        [_scroll.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    ]];

    [self update_header_controls];
}

/*
 * update_header_controls — sync the date picker value and Today button state
 * to the current days_offset.
 */
- (void)
update_header_controls
{
    time_t  t   = (time_t)midnight_for_offset(_days_offset);
    _date_picker.dateValue  = [NSDate dateWithTimeIntervalSince1970:t];
    _today_button.enabled   = (_days_offset != 0);
}

/*
 * date_picker_changed: — the user typed or scrolled to a new date in the
 * picker; compute the offset from today and reload.
 *
 * Inputs:
 *   picker — the NSDatePicker that fired
 */
- (void)
date_picker_changed:(NSDatePicker *)picker
{
    NSCalendar       *cal   = NSCalendar.currentCalendar;
    NSDate           *today = [cal startOfDayForDate:[NSDate date]];
    NSDate           *chosen = [cal startOfDayForDate:picker.dateValue];
    NSDateComponents *diff   = [cal components:NSCalendarUnitDay
                                      fromDate:today toDate:chosen options:0];
    _days_offset = (NSInteger)diff.day;
    [self update_header_controls];
    [self reload];
    [self scroll_to_now];
}

/*
 * go_prev_day: — navigate one day back and reload.
 *
 * Inputs:
 *   sender — the previous-day button (unused)
 */
- (void)
go_prev_day:(id)sender
{
    _days_offset--;
    [self update_header_controls];
    [self reload];
    [self scroll_to_now];
}

/*
 * go_next_day: — navigate one day forward and reload.
 *
 * Inputs:
 *   sender — the next-day button (unused)
 */
- (void)
go_next_day:(id)sender
{
    _days_offset++;
    [self update_header_controls];
    [self reload];
    [self scroll_to_now];
}

/*
 * go_today: — jump back to today and reload.
 *
 * Inputs:
 *   sender — the Today button (unused)
 */
- (void)
go_today:(id)sender
{
    _days_offset = 0;
    [self update_header_controls];
    [self reload];
    [self scroll_to_now];
}

/*
 * reload — re-query the displayed day's scheduled tasks and redraw.
 * Documented in day_planner.h.
 */
- (void)
reload
{
    if (self.is_shut_down) return;

    int64_t   mid   = midnight_for_offset(_days_offset);
    GPtrArray *tasks = task_db_tasks_scheduled_for_day(self.library.app->db, mid);

    [self.items removeAllObjects];
    for (guint i = 0; i < tasks->len; i++) {
        Task          *t    = g_ptr_array_index(tasks, i);
        TasksDayItem  *item = [[TasksDayItem alloc] init];
        item.task_id    = t->id;
        item.title      = (*t->title != '\0') ? tasks_string(t->title) : @"Untitled";
        item.start_unix = t->scheduled_start;
        item.duration   = t->scheduled_duration > 0 ? t->scheduled_duration : 60;
        item.status     = t->status;
        [self.items addObject:item];
    }
    task_ptr_array_free_tasks(tasks);

    /* Keep the canvas width in sync with the scroll view's content width */
    CGFloat cw = NSWidth(self.scroll.contentView.bounds);
    if (cw < tasks_look_day_planner_min_w) cw = tasks_look_day_planner_min_w;
    NSRect cf = self.canvas.frame;
    if (cf.size.width != cw) {
        cf.size.width   = cw;
        self.canvas.frame = cf;
    }

    [self.canvas setNeedsDisplay:YES];
    [self.canvas.window invalidateCursorRectsForView:self.canvas];
}

/*
 * scroll_to_now — scroll so the current time (today) or 8 AM (other days) is
 * visible with one hour of lead.
 * Documented in day_planner.h.
 */
- (void)
scroll_to_now
{
    CGFloat scroll_y;
    if (_days_offset == 0) {
        CGFloat now_y = [self.canvas y_for_unix:(int64_t)time(NULL)];
        scroll_y = MAX(0.0, now_y - tasks_look_day_planner_hour_h);
    } else {
        int64_t eight_am = midnight_for_offset(_days_offset) + 8 * 3600;
        scroll_y = MAX(0.0, [self.canvas y_for_unix:eight_am] - tasks_look_day_planner_hour_h);
    }
    [self.canvas scrollPoint:NSMakePoint(0.0, scroll_y)];
}

/*
 * check_and_progress — promote any New task whose scheduled start has passed.
 * Only runs when the displayed day is today.
 */
- (void)
check_and_progress
{
    if (self.is_shut_down) return;
    if (_days_offset != 0) return;

    int64_t  now     = (int64_t)time(NULL);
    TaskApp *app     = self.library.app;
    BOOL     changed = NO;

    for (TasksDayItem *item in self.items) {
        if (item.start_unix > 0 && item.start_unix <= now &&
            item.status == TASK_STATUS_NEW) {
            task_db_task_set_status(app->db, item.task_id, TASK_STATUS_IN_PROGRESS);
            item.status = TASK_STATUS_IN_PROGRESS;
            changed = YES;
        }
    }
    if (changed) {
        [self.library refresh_tasks];
        [self.canvas setNeedsDisplay:YES];
    }
}

/*
 * shut_down — stop timers.  Idempotent.
 * Documented in day_planner.h.
 */
- (void)
shut_down
{
    if (self.is_shut_down) return;
    self.is_shut_down = YES;
    [self.clock_timer invalidate];
    self.clock_timer = nil;
    [self.progress_timer invalidate];
    self.progress_timer = nil;
}

@end
