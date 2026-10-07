#import "day_planner.h"
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

/* Snap granularity for time placement and resize. */
static const NSInteger snap_minutes = 15;

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

/* ── TasksDayCanvas ────────────────────────────────────────────────────── */

@interface TasksDayCanvas : NSView <NSDraggingDestination>
@property (nonatomic, weak) TasksDayPlanner *planner;
/* Resize tracking */
@property (nonatomic) BOOL resizing;
@property (nonatomic) int64_t resize_id;
@property (nonatomic) CGFloat resize_start_y;
@property (nonatomic) int resize_orig_duration;
/* Drop highlight Y, NAN when no drag is over */
@property (nonatomic) CGFloat drop_y;
@end

@implementation TasksDayCanvas

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

/* ── Coordinate helpers ───────────────────────────────────────────────── */

/*
 * today_midnight — unix seconds of today's local midnight.
 *
 * Output:
 *   unix seconds.
 */
static int64_t
today_midnight(void)
{
    time_t    now   = time(NULL);
    struct tm local = *localtime(&now);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    return (int64_t)mktime(&local);
}

/*
 * y_for_unix: — canvas Y for a unix timestamp.
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
    double hours = (double)(t - today_midnight()) / 3600.0;
    return tasks_look_day_planner_pad_v +
           (hours - (double)tasks_look_day_planner_start_h) * tasks_look_day_planner_hour_h;
}

/*
 * unix_for_y: — unix timestamp for a canvas Y coordinate, snapped to
 * snap_minutes and clamped to the visible hour range.
 *
 * Inputs:
 *   y — Y in flipped canvas coordinates
 *
 * Output:
 *   unix seconds within today.
 */
- (int64_t)
unix_for_y:(CGFloat)y
{
    double hours = (y - tasks_look_day_planner_pad_v) / tasks_look_day_planner_hour_h
                   + (double)tasks_look_day_planner_start_h;
    int total_min = (int)round(hours * 60.0);
    total_min = (total_min / (int)snap_minutes) * (int)snap_minutes;
    int lo_min = (int)tasks_look_day_planner_start_h * 60;
    int hi_min = (int)tasks_look_day_planner_end_h   * 60 - (int)snap_minutes;
    if (total_min < lo_min) total_min = lo_min;
    if (total_min > hi_min) total_min = hi_min;
    return today_midnight() + (int64_t)total_min * 60;
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

    /* Current-time line */
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

/* ── Mouse events (block resize) ──────────────────────────────────────── */

/*
 * mouseDown: — begin a resize if the click landed on a block's grip stripe.
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

    for (TasksDayItem *item in self.planner.items) {
        NSRect r = [self item_rect:item];
        if (!NSPointInRect(pt, r)) continue;
        NSRect grip = NSMakeRect(r.origin.x,
                                 NSMaxY(r) - tasks_look_day_planner_resize_h,
                                 r.size.width, tasks_look_day_planner_resize_h);
        if (NSPointInRect(pt, grip)) {
            self.resizing            = YES;
            self.resize_id           = item.task_id;
            self.resize_start_y      = pt.y;
            self.resize_orig_duration = item.duration;
            return;
        }
    }
}

/*
 * mouseDragged: — update a block's duration while the resize drag is live.
 *
 * Inputs:
 *   event — the mouse-dragged event
 */
- (void)
mouseDragged:(NSEvent *)event
{
    if (!self.resizing) return;
    NSPoint pt      = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat dy      = pt.y - self.resize_start_y;
    int     delta   = (int)round((dy / tasks_look_day_planner_hour_h) * 60.0);
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
}

/*
 * mouseUp: — commit the new duration to the database.
 *
 * Inputs:
 *   event — the mouse-up event (unused)
 */
- (void)
mouseUp:(NSEvent *)event
{
    if (!self.resizing) return;
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
}

/*
 * resetCursorRects — show the resize cursor over each block's grip stripe.
 */
- (void)
resetCursorRects
{
    [self addCursorRect:self.bounds cursor:NSCursor.arrowCursor];
    for (TasksDayItem *item in self.planner.items) {
        NSRect r    = [self item_rect:item];
        NSRect grip = NSMakeRect(r.origin.x,
                                 NSMaxY(r) - tasks_look_day_planner_resize_h,
                                 r.size.width, tasks_look_day_planner_resize_h);
        [self addCursorRect:grip cursor:NSCursor.resizeDownCursor];
    }
}

@end

/* ── TasksDayPlanner ──────────────────────────────────────────────────── */

@interface TasksDayPlanner ()
@property (nonatomic, weak)   TasksLibraryWindow          *library;
@property (nonatomic, strong) NSScrollView                *scroll;
@property (nonatomic, strong) TasksDayCanvas              *canvas;
@property (nonatomic, strong) NSTimer                     *clock_timer;
@property (nonatomic, strong) NSTimer                     *progress_timer;
@property (nonatomic, strong) NSMutableArray<TasksDayItem *> *items;
@property (nonatomic)         BOOL                         is_shut_down;
@end

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

    _library = library;
    _items   = [NSMutableArray array];

    NSInteger n_hours  = tasks_look_day_planner_end_h - tasks_look_day_planner_start_h;
    CGFloat   canvas_h = tasks_look_day_planner_pad_v * 2.0 +
                         n_hours * tasks_look_day_planner_hour_h;

    _canvas         = [[TasksDayCanvas alloc]
                           initWithFrame:NSMakeRect(0, 0, tasks_look_day_planner_w, canvas_h)];
    _canvas.planner = self;
    _canvas.drop_y  = NAN;
    [_canvas registerForDraggedTypes:@[list_drag_type, kanban_drag_type]];

    _scroll                      = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.documentView         = _canvas;
    _scroll.hasVerticalScroller  = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers   = YES;
    _scroll.drawsBackground      = NO;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;

    [self addSubview:_scroll];
    [NSLayoutConstraint activateConstraints:@[
        [_scroll.topAnchor    constraintEqualToAnchor:self.topAnchor],
        [_scroll.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [_scroll.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
    ]];

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
 * reload — re-query today's scheduled tasks and redraw.
 * Documented in day_planner.h.
 */
- (void)
reload
{
    if (self.is_shut_down) return;

    GPtrArray *tasks = task_db_tasks_scheduled_today(self.library.app->db);

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
 * scroll_to_now — scroll so the current time is visible with one hour of lead.
 * Documented in day_planner.h.
 */
- (void)
scroll_to_now
{
    CGFloat now_y = [self.canvas y_for_unix:(int64_t)time(NULL)];
    CGFloat lead  = tasks_look_day_planner_hour_h;
    NSPoint pt    = NSMakePoint(0.0, MAX(0.0, now_y - lead));
    [self.canvas scrollPoint:pt];
}

/*
 * check_and_progress — promote any New task whose scheduled start has passed.
 */
- (void)
check_and_progress
{
    if (self.is_shut_down) return;

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
