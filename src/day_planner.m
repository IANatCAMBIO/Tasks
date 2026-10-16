#import "day_planner.h"
#import "editor_window.h"
#import "kanban.h"
#import "look.h"
#import "task_list.h"
#import "tool_panel.h"
#import "ui_util.h"

#include "db.h"
#include "gcal_store.h"
#include "gcal_sync.h"
#include "task_app.h"
#include "task_config.h"
#include "task_dates.h"

#include <math.h>
#include <time.h>

/* Snap granularity for time placement, resize and move. */
static const NSInteger snap_minutes  = 15;

/* Height of the date-navigation header above the scroll area. */
static const CGFloat   header_h      = 36.0;

/* Height of the all-day strip under the header, when the day has any
 * all-day events (zero otherwise). */
static const CGFloat   all_day_h     = 20.0;

/* Width of the calendar-coloured bar down a calendar event's left edge. */
static const CGFloat   event_bar_w   = 3.0;

/* ── TasksDayItem ──────────────────────────────────────────────────────── */

/* One block: a scheduled task, or (is_event) an event from Google
 * Calendar (gcal_sync.h), which is drawn to plan around and cannot be
 * selected, moved or stretched — its task_id is 0. */
@interface TasksDayItem : NSObject
@property (nonatomic) int64_t task_id;
@property (nonatomic, copy) NSString *title;
@property (nonatomic) int64_t start_unix;
@property (nonatomic) int duration;          /* minutes */
@property (nonatomic) TaskStatus status;
@property (nonatomic) BOOL is_event;
@property (nonatomic, strong) NSColor *event_fill;  /* is_event: its calendar's colour, dimmed */
@property (nonatomic, strong) NSColor *event_bar;   /* is_event: the colour itself            */
/* Side-by-side placement among the blocks it overlaps: column `column`
 * of `columns` (layout_columns). */
@property (nonatomic) NSUInteger column;
@property (nonatomic) NSUInteger columns;
/* The "H:MM–H:MM (Xhr Ymin)" line, built when first drawn and again after
 * the start or the duration changes; drawRect runs per frame of a drag. */
@property (nonatomic, readonly) NSString *time_text;
@end

static NSString *format_block_time(int64_t start_unix, int duration_min);

@implementation TasksDayItem {
    NSString *_time_text;
}

- (void)setStart_unix:(int64_t)start_unix { _start_unix = start_unix; _time_text = nil; }
- (void)setDuration:(int)duration         { _duration   = duration;   _time_text = nil; }

- (NSString *)
time_text
{
    if (_time_text == nil) _time_text = format_block_time(_start_unix, _duration);
    return _time_text;
}

@end

/* ── Forward declaration ─────────────────────────────────────────────── */

@class TasksDayCanvas;

/* ── TasksDayPlanner private interface (visible to the canvas below) ─── */

@interface TasksDayPlanner ()
@property (nonatomic, weak)   TasksLibraryWindow            *library;
@property (nonatomic, strong) NSScrollView                  *scroll;
@property (nonatomic, strong) TasksDayCanvas                *canvas;
@property (nonatomic, strong) NSTimer                       *clock_timer;
@property (nonatomic, strong) NSMutableArray<TasksDayItem *> *items;
@property (nonatomic)         BOOL                           is_shut_down;
@property (nonatomic)         NSInteger                      days_offset; /* 0 = today */
@property (nonatomic, weak)   NSTextField                   *date_label;
@property (nonatomic, weak)   NSButton                      *today_button;
@property (nonatomic, weak)   NSTextField                   *all_day_label;
@property (nonatomic, strong) NSLayoutConstraint            *all_day_height;
- (void)committed_block;
@end

/* ── TasksDayCanvas ────────────────────────────────────────────────────── */

@interface TasksDayCanvas : NSView <NSDraggingDestination>
@property (nonatomic, weak) TasksDayPlanner *planner;
/* Resize tracking */
@property (nonatomic) BOOL    resizing;
@property (nonatomic) BOOL    resize_top;           /* YES = dragging start, NO = dragging end */
@property (nonatomic) int64_t resize_id;
@property (nonatomic) CGFloat resize_start_y;
@property (nonatomic) int     resize_orig_duration;
@property (nonatomic) int64_t resize_orig_start_unix; /* top-edge: original start  */
@property (nonatomic) int64_t resize_orig_end_unix;   /* top-edge: fixed end time  */
/* Move tracking */
@property (nonatomic) BOOL    move_pending;   /* mouseDown seen, drag not yet started */
@property (nonatomic) BOOL    moving;
@property (nonatomic) int64_t move_id;
@property (nonatomic) int64_t move_orig_start_unix;
@property (nonatomic) CGFloat move_start_mouse_y;
/* Drop highlight Y, NAN when no drag is over */
@property (nonatomic) CGFloat drop_y;
/* Where the current-time line was last drawn, for redrawing just it */
@property (nonatomic) CGFloat now_y;
/* Selected task ids */
@property (nonatomic, strong) NSMutableSet<NSNumber *> *selected_ids;
/* The hours shown, [first_h, end_h): business hours widened to fit the
 * day's blocks (fit_hours) */
@property (nonatomic) NSInteger first_h;
@property (nonatomic) NSInteger end_h;
@end

@implementation TasksDayCanvas

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { return YES; }

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self setNeedsDisplay:YES];
}

/* ── Coordinate helpers ───────────────────────────────────────────────── */

/*
 * midnight_for_offset — unix seconds of local midnight for today + offset
 * days, by the calendar (task_day_bounds): a day across a clock change is
 * not today's midnight plus a multiple of 24 hours.
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
    gint64 lo, hi;
    task_day_bounds((gint)offset, &lo, &hi);
    return lo;
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
    return [self y_for_unix:t midnight:[self midnight]];
}

/*
 * midnight — the displayed day's local midnight.  A loop over the items
 * takes it once rather than once per item (task_day_bounds is calendar
 * arithmetic for any day but today).
 *
 * Output:
 *   unix seconds.
 */
- (int64_t)
midnight
{
    return midnight_for_offset(self.planner ? self.planner.days_offset : 0);
}

/*
 * y_for_unix:midnight: — canvas Y for a unix timestamp, the day's midnight
 * given.
 *
 * Inputs:
 *   t   — unix seconds
 *   mid — the displayed day's local midnight
 *
 * Output:
 *   Y in flipped canvas coordinates (Y=0 at top).
 */
- (CGFloat)
y_for_unix:(int64_t)t midnight:(int64_t)mid
{
    double hours = (double)(t - mid) / 3600.0;
    return tasks_look_day_planner_pad_v +
           (hours - (double)self.first_h) * tasks_look_day_planner_hour_h;
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
                         + (double)self.first_h;
    int total_min = (int)round(hours * 60.0);
    total_min = (total_min / (int)snap_minutes) * (int)snap_minutes;
    int lo_min = (int)self.first_h * 60;
    int hi_min = (int)self.end_h   * 60 - (int)snap_minutes;
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
    return [self item_rect:item midnight:[self midnight]];
}

/*
 * item_rect:midnight: — canvas rectangle for a scheduled item, the day's
 * midnight given.
 *
 * Inputs:
 *   item — the item
 *   mid  — the displayed day's local midnight
 *
 * Output:
 *   block frame in canvas coordinates.
 */
- (NSRect)
item_rect:(TasksDayItem *)item midnight:(int64_t)mid
{
    CGFloat   left = tasks_look_day_planner_label_w + tasks_look_day_planner_block_inset;
    CGFloat   all  = NSWidth(self.bounds) - left - tasks_look_day_planner_block_inset;
    NSUInteger n   = item.columns > 0 ? item.columns : 1;
    CGFloat   w    = all / (CGFloat)n;
    CGFloat   x    = left + w * (CGFloat)item.column;
    if (n > 1) w  -= tasks_look_day_planner_block_inset;
    CGFloat y = [self y_for_unix:item.start_unix midnight:mid];
    CGFloat h = (item.duration / 60.0) * tasks_look_day_planner_hour_h;
    CGFloat min_h = tasks_look_day_planner_resize_h * 2.0 + 4.0;
    if (h < min_h) h = min_h;
    return NSMakeRect(x, y, w, h);
}

/*
 * item_named: — the item of a task.
 *
 * Inputs:
 *   task_id — the task
 *
 * Output:
 *   the item, or nil when the day has none for it.
 */
- (TasksDayItem *)
item_named:(int64_t)task_id
{
    for (TasksDayItem *item in self.planner.items) {
        if (!item.is_event && item.task_id == task_id) return item;
    }
    return nil;
}

/*
 * redraw_block_from:to: — invalidate what a block's move or resize
 * uncovers and covers: its old frame and its new one, with room for the
 * stroke, rather than the whole canvas per frame of the drag.
 *
 * Inputs:
 *   before — the block's frame before the change
 *   after  — its frame after
 */
- (void)
redraw_block_from:(NSRect)before to:(NSRect)after
{
    [self setNeedsDisplayInRect:NSInsetRect(NSUnionRect(before, after), -2.0, -2.0)];
}

/*
 * set_drop_y: — move the drop indicator, redrawing only where it was and
 * where it is.
 *
 * Inputs:
 *   y — the indicator's canvas Y, or NAN for none
 */
- (void)
set_drop_y:(CGFloat)y
{
    CGFloat w = NSWidth(self.bounds);
    if (!isnan(self.drop_y)) [self setNeedsDisplayInRect:NSMakeRect(0, self.drop_y - 2.0, w, 4.0)];
    self.drop_y = y;
    if (!isnan(y)) [self setNeedsDisplayInRect:NSMakeRect(0, y - 2.0, w, 4.0)];
}

/*
 * redraw_now_line — the clock moved: redraw the current-time line where
 * it was drawn and where it now belongs, and nothing else.
 */
- (void)
redraw_now_line
{
    CGFloat w   = NSWidth(self.bounds);
    CGFloat now = [self y_for_unix:(int64_t)time(NULL)];
    [self setNeedsDisplayInRect:NSMakeRect(0, self.now_y - 6.0, w, 12.0)];
    [self setNeedsDisplayInRect:NSMakeRect(0, now - 6.0, w, 12.0)];
}

/* ── Drawing helpers ──────────────────────────────────────────────────── */

/*
 * format_block_time — build the "H:MM–H:MM (Xhr Ymin)" subtitle string.
 *
 * Inputs:
 *   start_unix   — block start as unix seconds
 *   duration_min — block length in minutes
 *
 * Output:
 *   autoreleased NSString.
 */
static NSString *
format_block_time(int64_t start_unix, int duration_min)
{
    time_t    ts   = (time_t)start_unix;
    time_t    te   = (time_t)(start_unix + (int64_t)duration_min * 60);
    struct tm s    = *localtime(&ts);
    struct tm e    = *localtime(&te);

    int sh = s.tm_hour % 12; if (sh == 0) sh = 12;
    int eh = e.tm_hour % 12; if (eh == 0) eh = 12;

    NSString *start_str = s.tm_min == 0
        ? [NSString stringWithFormat:@"%d", sh]
        : [NSString stringWithFormat:@"%d:%02d", sh, s.tm_min];
    NSString *end_str   = e.tm_min == 0
        ? [NSString stringWithFormat:@"%d", eh]
        : [NSString stringWithFormat:@"%d:%02d", eh, e.tm_min];

    int hrs  = duration_min / 60;
    int mins = duration_min % 60;
    NSString *dur;
    if      (mins == 0) dur = [NSString stringWithFormat:@"%dhr",        hrs];
    else if (hrs  == 0) dur = [NSString stringWithFormat:@"%dmin",       mins];
    else                dur = [NSString stringWithFormat:@"%dhr %dmin",  hrs, mins];

    return [NSString stringWithFormat:@"%@–%@ (%@)", start_str, end_str, dur];
}

/* ── Drawing ──────────────────────────────────────────────────────────── */

/* What every frame draws with, made once: the fonts, the attribute
 * dictionaries, the hour labels and their sizes.  The colours are dynamic
 * (tasks_color_dimmed), so one set serves light and dark. */
typedef struct {
    NSFont       *label_font;
    NSDictionary *label_attrs;
    NSDictionary *title_attrs;
    NSDictionary *time_attrs;
    NSDictionary *event_attrs;               /* a calendar event's title */
    CGFloat       title_h;
    CGFloat       time_h;
    NSArray<NSString *> *hour_labels;         /* indexed by hour, 0-23 */
    CGFloat       hour_label_h;
    NSColor      *acc_fill_n, *acc_bord_n, *done_fill_n, *done_bord_n;
    NSColor      *acc_fill_s, *acc_bord_s, *done_fill_s, *done_bord_s;
} canvas_look_t;

/*
 * canvas_look — the shared drawing resources.
 *
 * Output:
 *   the resources (static storage).
 */
static const canvas_look_t *
canvas_look(void)
{
    static canvas_look_t   look;
    static dispatch_once_t once;

    dispatch_once(&once, ^{
        look.label_font = [NSFont monospacedDigitSystemFontOfSize:10.0
                                                           weight:NSFontWeightRegular];
        look.label_attrs = @{ NSFontAttributeName:            look.label_font,
                              NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
        NSFont *title_font = [NSFont systemFontOfSize:tasks_look_row_small_font];
        NSFont *time_font  = [NSFont systemFontOfSize:tasks_look_row_small_font - 1.0];
        look.title_attrs = @{ NSFontAttributeName:            title_font,
                              NSForegroundColorAttributeName: NSColor.labelColor };
        look.time_attrs  = @{ NSFontAttributeName:            time_font,
                              NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
        look.event_attrs = @{ NSFontAttributeName:            title_font,
                              NSForegroundColorAttributeName: NSColor.secondaryLabelColor };
        look.title_h = title_font.ascender - title_font.descender + title_font.leading;
        look.time_h  = time_font.ascender  - time_font.descender  + time_font.leading;

        NSMutableArray<NSString *> *labels = [NSMutableArray array];
        for (NSInteger h = 0; h < 24; h++) {
            NSString *text;
            if (h == 0)       text = @"12 AM";
            else if (h < 12)  text = [NSString stringWithFormat:@"%ld AM", (long)h];
            else if (h == 12) text = @"12 PM";
            else              text = [NSString stringWithFormat:@"%ld PM", (long)(h - 12)];
            [labels addObject:text];
        }
        look.hour_labels  = [labels copy];
        look.hour_label_h = [labels.firstObject sizeWithAttributes:look.label_attrs].height;

        /* Task blocks — two tones: normal and selected (darker) */
        NSColor *accent = NSColor.controlAccentColor;
        NSColor *second = NSColor.secondaryLabelColor;
        look.acc_fill_n  = tasks_color_dimmed(accent, 18);
        look.acc_bord_n  = tasks_color_dimmed(accent, 55);
        look.done_fill_n = tasks_color_dimmed(second, 10);
        look.done_bord_n = tasks_color_dimmed(second, 28);
        look.acc_fill_s  = tasks_color_dimmed(accent, 38);
        look.acc_bord_s  = tasks_color_dimmed(accent, 82);
        look.done_fill_s = tasks_color_dimmed(second, 22);
        look.done_bord_s = tasks_color_dimmed(second, 52);
    });
    return &look;
}

/*
 * drawRect: — draw hour grid, task blocks, drop indicator, current time line.
 * Only what meets the dirty rectangle is laid out or drawn.
 *
 * Inputs:
 *   dirty — the rectangle to redraw
 */
- (void)
drawRect:(NSRect)dirty
{
    const canvas_look_t *look = canvas_look();
    NSInteger start_h = self.first_h;
    NSInteger end_h   = self.end_h;
    CGFloat   hour_h  = tasks_look_day_planner_hour_h;
    CGFloat   pad     = tasks_look_day_planner_pad_v;
    CGFloat   label_w = tasks_look_day_planner_label_w;
    int64_t   mid     = [self midnight];

    [NSColor.textBackgroundColor setFill];
    NSRectFill(dirty);

    /* Thin separator between label column and block area */
    NSRect col_rule = NSMakeRect(label_w - 0.5, 0, 1.0, NSHeight(self.bounds));
    if (NSIntersectsRect(col_rule, dirty)) {
        [NSColor.separatorColor setFill];
        NSRectFill(col_rule);
    }

    for (NSInteger h = start_h; h <= end_h; h++) {
        CGFloat y = pad + (h - start_h) * hour_h;

        NSRect line = NSMakeRect(0, y - 0.5, NSWidth(self.bounds), 1.0);
        if (NSIntersectsRect(line, dirty)) {
            [NSColor.separatorColor setFill];
            NSRectFill(line);
        }

        if (h < end_h) {
            NSRect lrec = NSMakeRect(2.0, y - look->hour_label_h - 2.0, label_w - 4.0,
                                     look->hour_label_h);
            if (NSIntersectsRect(lrec, dirty)) {
                [look->hour_labels[(NSUInteger)h] drawInRect:lrec
                                                          withAttributes:look->label_attrs];
            }
        }
    }

    for (TasksDayItem *item in self.planner.items) {
        NSRect r = [self item_rect:item midnight:mid];
        if (!NSIntersectsRect(r, dirty)) continue;

        if (item.is_event) {
            [self draw_event:item in:r look:look];
            continue;
        }

        NSBezierPath *path =
            [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 0.5, 0.5)
                                            xRadius:4.0 yRadius:4.0];
        BOOL sel = [self.selected_ids containsObject:@(item.task_id)];
        if (item.status == TASK_STATUS_DONE) {
            [sel ? look->done_fill_s : look->done_fill_n setFill];   [path fill];
            [sel ? look->done_bord_s : look->done_bord_n setStroke]; [path stroke];
        } else {
            [sel ? look->acc_fill_s : look->acc_fill_n setFill];   [path fill];
            [sel ? look->acc_bord_s : look->acc_bord_n setStroke]; [path stroke];
        }

        NSRect content = NSInsetRect(r, 4.0, 3.0);

        /* Title on the first line */
        NSRect title_rect = NSMakeRect(content.origin.x, content.origin.y,
                                       content.size.width, look->title_h);
        [item.title drawInRect:title_rect withAttributes:look->title_attrs];

        /* Time / duration on the second line, if there is room */
        NSRect time_rect = NSMakeRect(content.origin.x,
                                      NSMaxY(title_rect) + 1.0,
                                      content.size.width, look->time_h);
        if (NSMaxY(time_rect) <= NSMaxY(content)) {
            [item.time_text drawInRect:time_rect withAttributes:look->time_attrs];
        }
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
        CGFloat now_y = [self y_for_unix:(int64_t)time(NULL) midnight:mid];
        self.now_y = now_y;
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

/*
 * draw_event:in:look: — draw a calendar event: a flat tint of its
 * calendar's colour with a solid bar down the left edge, unlike a task's
 * bordered block, so that what can be moved and what cannot tell apart.
 *
 * Inputs:
 *   item — the event
 *   r    — its frame
 *   look — the shared drawing resources
 */
- (void)
draw_event:(TasksDayItem *)item in:(NSRect)r look:(const canvas_look_t *)look
{
    NSRect body = NSInsetRect(r, 0.5, 0.5);
    [item.event_fill setFill];
    [[NSBezierPath bezierPathWithRoundedRect:body xRadius:4.0 yRadius:4.0] fill];
    [item.event_bar setFill];
    NSRectFill(NSMakeRect(NSMinX(body), NSMinY(body) + 1.0, event_bar_w, NSHeight(body) - 2.0));

    NSRect content = NSInsetRect(r, 4.0, 3.0);
    content.origin.x   += event_bar_w;
    content.size.width -= event_bar_w;
    NSRect title_rect = NSMakeRect(content.origin.x, content.origin.y,
                                   content.size.width, look->title_h);
    [item.title drawInRect:title_rect withAttributes:look->event_attrs];
    NSRect time_rect = NSMakeRect(content.origin.x, NSMaxY(title_rect) + 1.0,
                                  content.size.width, look->time_h);
    if (NSMaxY(time_rect) <= NSMaxY(content))
        [item.time_text drawInRect:time_rect withAttributes:look->time_attrs];
}

/*
 * event_at: — the calendar event under a point.
 *
 * Inputs:
 *   pt — canvas coordinates
 *
 * Output:
 *   the event, or nil.
 */
- (TasksDayItem *)
event_at:(NSPoint)pt
{
    int64_t mid = [self midnight];
    for (TasksDayItem *item in self.planner.items) {
        if (item.is_event && NSPointInRect(pt, [self item_rect:item midnight:mid])) return item;
    }
    return nil;
}

/*
 * view:stringForToolTip:point:userData: — a calendar event's tooltip:
 * its title and times (the block may be too short to show them).
 *
 * Inputs:
 *   view  — this canvas
 *   tag   — the tooltip rectangle (unused)
 *   point — where the pointer is
 *   data  — unused
 *
 * Output:
 *   the text, or nil when no event is under the pointer.
 */
- (NSString *)
view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void *)data
{
    TasksDayItem *item = [self event_at:point];
    if (item == nil) return nil;
    return [NSString stringWithFormat:@"%@\n%@ · Google Calendar", item.title, item.time_text];
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
    BOOL ok = ([pb availableTypeFromArray:@[tasks_row_drag_type]]   != nil ||
               [pb availableTypeFromArray:@[tasks_card_drag_type]] != nil);
    if (!ok) return NSDragOperationNone;

    NSPoint pt = [self convertPoint:info.draggingLocation fromView:nil];
    [self set_drop_y:[self y_for_unix:[self unix_for_y:pt.y]]];
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
    [self set_drop_y:[self y_for_unix:[self unix_for_y:pt.y]]];
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
    [self set_drop_y:NAN];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info { return YES; }

/*
 * performDragOperation: — schedule each dropped task at the target time.
 * A card dragged from the Kanban board that is still New also moves to
 * In Progress (a list row keeps its status), and the whole window is then
 * refreshed so the card changes lane — on the next run-loop pass, because
 * this runs inside the drag's own handler.
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
    [self set_drop_y:NAN];
    NSPoint pt    = [self convertPoint:info.draggingLocation fromView:nil];
    int64_t start = [self unix_for_y:pt.y];

    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    NSPasteboard *pb = info.draggingPasteboard;

    /* Kanban puts a comma-separated list in one item */
    NSString *kanban_str = [pb stringForType:tasks_card_drag_type];
    NSUInteger n_cards   = 0;
    if (kanban_str.length > 0) {
        for (NSString *part in [kanban_str componentsSeparatedByString:@","]) {
            int64_t tid = part.longLongValue;
            if (tid > 0) [ids addObject:@(tid)];
        }
        n_cards = ids.count;
    }

    /* Task list puts one item per row */
    for (NSPasteboardItem *item in pb.pasteboardItems) {
        NSString *s = [item stringForType:tasks_row_drag_type];
        if (s.length > 0) {
            int64_t tid = s.longLongValue;
            if (tid > 0 && ![ids containsObject:@(tid)])
                [ids addObject:@(tid)];
        }
    }

    if (ids.count == 0) return NO;

    TasksLibraryWindow *library    = self.planner.library;
    TaskApp            *app        = library.app;
    NSInteger           offset_min = 0;
    BOOL                promoted   = NO;
    BOOL                failed     = NO;
    for (NSUInteger i = 0; i < ids.count; i++) {
        int64_t tid = ids[i].longLongValue;
        failed = !task_db_task_set_scheduled(app->db, tid, start + offset_min * 60, 60) ||
                 failed;
        offset_min += (NSInteger)snap_minutes;
        if (i >= n_cards) continue;

        Task *task = task_db_task_get(app->db, tid);
        if (task != NULL && task->status == TASK_STATUS_NEW) {
            failed   = !task_db_task_set_status(app->db, tid, TASK_STATUS_IN_PROGRESS) || failed;
            promoted = YES;
        }
        task_free(task);
    }
    if (failed) {
        task_app_write_failed(app, "The schedule");   /* refreshes everything */
    } else if (promoted) {
        __weak TasksLibraryWindow *weak_library = library;
        dispatch_async(dispatch_get_main_queue(), ^{ [weak_library full_refresh]; });
    } else {
        [self.planner reload];
    }
    gcal_sync_kick();
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
 * mouseDown: — select the clicked block, then begin a resize on the grip
 * stripe or arm a pending move on the block body.  A double-click opens the
 * editor.  Cmd+click toggles the clicked block into/out of the selection;
 * a plain click in empty space clears the selection.
 *
 * Inputs:
 *   event — the mouse-down event
 */
- (void)
mouseDown:(NSEvent *)event
{
    NSPoint pt        = [self convertPoint:event.locationInWindow fromView:nil];
    self.resize_id    = 0;
    self.resizing     = NO;
    self.move_id      = 0;
    self.move_pending = NO;
    self.moving       = NO;

    int64_t mid = [self midnight];
    for (TasksDayItem *item in self.planner.items) {
        if (item.is_event) continue;
        NSRect r = [self item_rect:item midnight:mid];
        if (!NSPointInRect(pt, r)) continue;

        /* Double-click: open the editor */
        if (event.clickCount == 2) {
            tasks_editor_open(self.planner.library.app, item.task_id);
            return;
        }

        /* Update selection */
        NSNumber *tid = @(item.task_id);
        if (event.modifierFlags & NSEventModifierFlagCommand) {
            if ([self.selected_ids containsObject:tid])
                [self.selected_ids removeObject:tid];
            else
                [self.selected_ids addObject:tid];
        } else {
            [self.selected_ids removeAllObjects];
            [self.selected_ids addObject:tid];
        }
        [self setNeedsDisplay:YES];

        CGFloat ez      = MIN(tasks_look_day_planner_resize_h, r.size.height * 0.4);
        NSRect  bot_edge = NSMakeRect(r.origin.x, NSMaxY(r) - ez, r.size.width, ez);
        NSRect  top_edge = NSMakeRect(r.origin.x, r.origin.y,     r.size.width, ez);
        if (NSPointInRect(pt, bot_edge)) {
            self.resizing             = YES;
            self.resize_top           = NO;
            self.resize_id            = item.task_id;
            self.resize_start_y       = pt.y;
            self.resize_orig_duration = item.duration;
        } else if (NSPointInRect(pt, top_edge)) {
            self.resizing               = YES;
            self.resize_top             = YES;
            self.resize_id              = item.task_id;
            self.resize_start_y         = pt.y;
            self.resize_orig_start_unix = item.start_unix;
            self.resize_orig_end_unix   = item.start_unix + (int64_t)item.duration * 60;
        } else {
            /* Block body: arm a pending move; promote to moving in mouseDragged: */
            self.move_pending         = YES;
            self.move_id              = item.task_id;
            self.move_orig_start_unix = item.start_unix;
            self.move_start_mouse_y   = pt.y;
        }
        return;
    }

    /* Clicked empty space: clear selection */
    if (self.selected_ids.count > 0) {
        [self.selected_ids removeAllObjects];
        [self setNeedsDisplay:YES];
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

        TasksDayItem *item = [self item_named:self.resize_id];
        if (item == nil) return;
        int64_t mid    = [self midnight];
        NSRect  before = [self item_rect:item midnight:mid];

        if (self.resize_top) {
            /* Top edge: move start time, keep end time fixed */
            int64_t new_start = self.resize_orig_start_unix + (int64_t)delta * 60;
            int64_t new_end   = self.resize_orig_end_unix;
            int64_t lo        = mid + (int64_t)self.first_h * 3600;
            if (new_start < lo)
                new_start = lo;
            if (new_start > new_end - (int64_t)snap_minutes * 60)
                new_start = new_end - (int64_t)snap_minutes * 60;
            item.start_unix = new_start;
            item.duration   = (int)((new_end - new_start) / 60);
        } else {
            /* Bottom edge: keep start time fixed, move end time */
            int new_dur = self.resize_orig_duration + delta;
            if (new_dur < (int)snap_minutes) new_dur = (int)snap_minutes;
            if (new_dur > 8 * 60)           new_dur = 8 * 60;
            item.duration = new_dur;
        }
        [self redraw_block_from:before to:[self item_rect:item midnight:mid]];
        return;
    }

    /* Promote a pending move to an active one once the drag clears the threshold. */
    if (self.move_pending && fabs(pt.y - self.move_start_mouse_y) >= 5.0) {
        self.move_pending = NO;
        self.moving       = YES;
    }

    if (self.moving) {
        CGFloat dy        = pt.y - self.move_start_mouse_y;
        int     delta_min = (int)round((dy / tasks_look_day_planner_hour_h) * 60.0);
        delta_min = (delta_min / (int)snap_minutes) * (int)snap_minutes;

        TasksDayItem *item = [self item_named:self.move_id];
        if (item == nil) return;
        int64_t mid       = [self midnight];
        NSRect  before    = [self item_rect:item midnight:mid];
        int64_t new_start = self.move_orig_start_unix + (int64_t)delta_min * 60;
        int64_t lo        = mid + (int64_t)self.first_h * 3600;
        int64_t hi        = mid + (int64_t)self.end_h   * 3600
                            - (int64_t)item.duration * 60;
        if (new_start < lo) new_start = lo;
        if (new_start > hi) new_start = hi;
        item.start_unix = new_start;
        [self redraw_block_from:before to:[self item_rect:item midnight:mid]];
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
            if (!item.is_event && item.task_id == self.resize_id) {
                if (!task_db_task_set_scheduled(self.planner.library.app->db, item.task_id,
                                                item.start_unix, item.duration))
                    task_app_write_failed(self.planner.library.app, "The new length");
                break;
            }
        }
        self.resize_id = 0;
        [self.planner committed_block];
        return;
    }

    if (self.moving) {
        self.moving = NO;
        for (TasksDayItem *item in self.planner.items) {
            if (!item.is_event && item.task_id == self.move_id) {
                if (!task_db_task_set_scheduled(self.planner.library.app->db, item.task_id,
                                                item.start_unix, item.duration))
                    task_app_write_failed(self.planner.library.app, "The new time");
                break;
            }
        }
        self.move_id = 0;
        [self.planner committed_block];
        return;
    }

    /* Click that never became a drag — selection was handled in mouseDown. */
    self.move_pending = NO;
    self.move_id      = 0;
}

/*
 * resetCursorRects — open-hand cursor on block bodies; resize cursor on grips.
 */
- (void)
resetCursorRects
{
    int64_t mid = [self midnight];
    [self addCursorRect:self.bounds cursor:NSCursor.arrowCursor];
    [self removeAllToolTips];
    for (TasksDayItem *item in self.planner.items) {
        NSRect  r    = [self item_rect:item midnight:mid];
        if (item.is_event) {
            [self addToolTipRect:r owner:self userData:NULL];
            continue;
        }
        CGFloat ez   = MIN(tasks_look_day_planner_resize_h, r.size.height * 0.4);
        NSRect  top  = NSMakeRect(r.origin.x, r.origin.y,           r.size.width, ez);
        NSRect  bot  = NSMakeRect(r.origin.x, NSMaxY(r) - ez,       r.size.width, ez);
        CGFloat bh   = r.size.height - 2.0 * ez;
        NSRect  body = NSMakeRect(r.origin.x, r.origin.y + ez,      r.size.width, MAX(bh, 0.0));
        [self addCursorRect:top  cursor:NSCursor.resizeUpDownCursor];
        [self addCursorRect:body cursor:NSCursor.openHandCursor];
        [self addCursorRect:bot  cursor:NSCursor.resizeUpDownCursor];
    }
}

/*
 * menuForEvent: — build the right-click context menu.
 *
 * If the click landed on an unselected block, that block is selected (others
 * deselected) before the menu is shown.  Returns nil for clicks on empty space.
 *
 * Inputs:
 *   event — the right-click event
 *
 * Output:
 *   context menu, or nil.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    NSPoint pt = [self convertPoint:event.locationInWindow fromView:nil];

    TasksDayItem *clicked = nil;
    int64_t       mid     = [self midnight];
    for (TasksDayItem *item in self.planner.items) {
        if (!item.is_event && NSPointInRect(pt, [self item_rect:item midnight:mid])) {
            clicked = item;
            break;
        }
    }
    if (!clicked) return nil;

    NSNumber *cid = @(clicked.task_id);
    if (![self.selected_ids containsObject:cid]) {
        [self.selected_ids removeAllObjects];
        [self.selected_ids addObject:cid];
        [self setNeedsDisplay:YES];
    }

    NSUInteger n    = self.selected_ids.count;
    NSString  *lbl  = n > 1
        ? [NSString stringWithFormat:@"Remove %lu Items from Schedule", (unsigned long)n]
        : @"Remove from Schedule";

    NSMenu     *menu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *it   = [menu addItemWithTitle:lbl
                                       action:@selector(remove_selected_from_schedule:)
                                keyEquivalent:@""];
    it.target = self.planner;
    return menu;
}

@end

/* ── Layout ───────────────────────────────────────────────────────────── */

/*
 * layout_columns — place overlapping blocks side by side, the way a
 * calendar does: blocks are taken by start (the longer first on a tie),
 * each goes in the leftmost column free at its start, and every block of
 * a run of mutually overlapping ones shares that run's column count.
 *
 * Inputs:
 *   items — the day's blocks; their column and columns are set
 */
static void
layout_columns(NSArray<TasksDayItem *> *items)
{
    NSArray<TasksDayItem *> *sorted =
        [items sortedArrayUsingComparator:^NSComparisonResult(TasksDayItem *a, TasksDayItem *b) {
            if (a.start_unix != b.start_unix) return a.start_unix < b.start_unix ? NSOrderedAscending
                                                                                 : NSOrderedDescending;
            if (a.duration != b.duration) return a.duration > b.duration ? NSOrderedAscending
                                                                         : NSOrderedDescending;
            return NSOrderedSame;
        }];

    NSMutableArray<NSNumber *>     *col_end = [NSMutableArray array]; /* per column */
    NSMutableArray<TasksDayItem *> *run     = [NSMutableArray array];
    int64_t run_end = INT64_MIN;

    for (TasksDayItem *item in sorted) {
        int64_t end = item.start_unix + (int64_t)MAX(item.duration, 1) * 60;
        if (item.start_unix >= run_end && run.count > 0) {
            for (TasksDayItem *done in run) done.columns = col_end.count;
            [run removeAllObjects];
            [col_end removeAllObjects];
        }
        NSUInteger col = 0;
        while (col < col_end.count && col_end[col].longLongValue > item.start_unix) col++;
        if (col == col_end.count) [col_end addObject:@(end)];
        else                      col_end[col] = @(end);
        item.column = col;
        [run addObject:item];
        run_end = run.count == 1 ? end : MAX(run_end, end);
    }
    for (TasksDayItem *done in run) done.columns = col_end.count;
}

/*
 * calendar_color — a Google calendar's "#rrggbb" colour.
 *
 * Inputs:
 *   hex — the colour, or "" when unknown
 *
 * Output:
 *   the colour; the system blue when it cannot be read.
 */
static NSColor *
calendar_color(const char *hex)
{
    unsigned int rgb = 0;
    if (hex == NULL || hex[0] != '#' || sscanf(hex + 1, "%6x", &rgb) != 1)
        return NSColor.systemBlueColor;
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xff) / 255.0
                               green:((rgb >> 8) & 0xff) / 255.0
                                blue:(rgb & 0xff) / 255.0
                               alpha:1.0];
}

/* ── TasksDayPlanner ──────────────────────────────────────────────────── */

/*
 * canvas_height — the canvas height for the hours it shows, with the
 * padding above the first hour line and below the last.
 *
 * Inputs:
 *   canvas — the canvas, its first_h and end_h set
 *
 * Output:
 *   height in points.
 */
static CGFloat
canvas_height(TasksDayCanvas *canvas)
{
    return tasks_look_day_planner_pad_v * 2.0 +
           (CGFloat)(canvas.end_h - canvas.first_h) * tasks_look_day_planner_hour_h;
}

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

    _canvas         = [[TasksDayCanvas alloc]
                           initWithFrame:NSMakeRect(0, 0, tasks_look_day_planner_w, 0)];
    _canvas.first_h      = tasks_look_day_planner_start_h;
    _canvas.end_h        = tasks_look_day_planner_end_h;
    [_canvas setFrameSize:NSMakeSize(tasks_look_day_planner_w, canvas_height(_canvas))];
    _canvas.planner      = self;
    _canvas.drop_y       = NAN;
    _canvas.selected_ids = [NSMutableSet set];
    [_canvas registerForDraggedTypes:@[tasks_row_drag_type, tasks_card_drag_type]];

    _scroll                       = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.documentView          = _canvas;
    _scroll.hasVerticalScroller   = YES;
    _scroll.hasHorizontalScroller = NO;
    _scroll.autohidesScrollers    = YES;
    _scroll.drawsBackground       = NO;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;

    /* The clip view is the visible area: the panel minus any scroller that
     * takes space.  Its frame changes when the panel resizes and when the
     * scroller appears, so the canvas follows it rather than the panel. */
    _scroll.contentView.postsFrameChangedNotifications = YES;
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(sync_canvas_width:)
                                                 name:NSViewFrameDidChangeNotification
                                               object:_scroll.contentView];

    [self build_header];
    [self sync_clock_timer];
    return self;
}

/*
 * sync_canvas_width: — make the canvas exactly as wide as the scroll view's
 * visible area, so nothing is laid out under a scroller.  Floored at the
 * hour-label column plus the block insets so item rectangles keep a
 * non-negative width while the panel is collapsed.
 *
 * Inputs:
 *   note — NSViewFrameDidChangeNotification from the clip view (unused)
 */
- (void)
sync_canvas_width:(NSNotification *)note
{
    CGFloat least = tasks_look_day_planner_label_w + tasks_look_day_planner_block_inset * 2.0;
    CGFloat cw    = MAX(NSWidth(self.scroll.contentView.bounds), least);
    NSRect  cf    = self.canvas.frame;
    if (cf.size.width != cw) {
        cf.size.width = cw;
        self.canvas.frame = cf;
    }
}

/*
 * build_header — add the date navigation bar and the scroll view, wired with
 * Auto Layout.
 *
 * Layout:  [Today][cal]  ·  [date label]  ·  [‹][›]
 *           left-aligned     center            right-aligned
 *
 * Called once from init.
 */
- (void)
build_header
{
    NSView *header = [[NSView alloc] initWithFrame:NSZeroRect];
    header.translatesAutoresizingMaskIntoConstraints = NO;

    /* Left section ─ Today button + calendar icon button */
    NSButton *today       = [NSButton buttonWithTitle:@"Today"
                                               target:self
                                               action:@selector(go_today:)];
    today.controlSize     = NSControlSizeSmall;
    today.bezelStyle      = NSBezelStyleRounded;
    today.translatesAutoresizingMaskIntoConstraints = NO;
    _today_button = today;

    NSImage  *cal_img = [NSImage imageWithSystemSymbolName:@"calendar"
                                      accessibilityDescription:@"Pick a date"];
    NSButton *cal_btn = [NSButton buttonWithImage:cal_img
                                           target:self
                                           action:@selector(pick_displayed_date:)];
    cal_btn.bordered  = NO;
    cal_btn.toolTip   = @"Pick a date";
    cal_btn.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *left = [[NSView alloc] initWithFrame:NSZeroRect];
    left.translatesAutoresizingMaskIntoConstraints = NO;
    [left addSubview:today];
    [left addSubview:cal_btn];
    [NSLayoutConstraint activateConstraints:@[
        [today.leadingAnchor  constraintEqualToAnchor:left.leadingAnchor],
        [today.centerYAnchor  constraintEqualToAnchor:left.centerYAnchor],
        [cal_btn.leadingAnchor constraintEqualToAnchor:today.trailingAnchor constant:2.0],
        [cal_btn.centerYAnchor constraintEqualToAnchor:left.centerYAnchor],
        [cal_btn.trailingAnchor constraintEqualToAnchor:left.trailingAnchor],
        [left.topAnchor       constraintEqualToAnchor:today.topAnchor],
        [left.bottomAnchor    constraintEqualToAnchor:today.bottomAnchor],
    ]];

    /* Center section ─ date label */
    NSTextField *label  = [NSTextField labelWithString:@""];
    label.font          = [NSFont systemFontOfSize:12.0 weight:NSFontWeightMedium];
    label.alignment     = NSTextAlignmentCenter;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    _date_label = label;

    /* Right section ─ prev / next arrows */
    NSButton *prev     = [NSButton buttonWithTitle:@""
                                            target:self
                                            action:@selector(go_prev_day:)];
    prev.image         = [NSImage imageWithSystemSymbolName:@"chevron.left"
                                       accessibilityDescription:nil];
    prev.imagePosition = NSImageOnly;
    prev.controlSize   = NSControlSizeSmall;
    prev.bezelStyle    = NSBezelStyleRounded;
    prev.translatesAutoresizingMaskIntoConstraints = NO;

    NSButton *next     = [NSButton buttonWithTitle:@""
                                            target:self
                                            action:@selector(go_next_day:)];
    next.image         = [NSImage imageWithSystemSymbolName:@"chevron.right"
                                       accessibilityDescription:nil];
    next.imagePosition = NSImageOnly;
    next.controlSize   = NSControlSizeSmall;
    next.bezelStyle    = NSBezelStyleRounded;
    next.translatesAutoresizingMaskIntoConstraints = NO;

    NSView *right = [[NSView alloc] initWithFrame:NSZeroRect];
    right.translatesAutoresizingMaskIntoConstraints = NO;
    [right addSubview:prev];
    [right addSubview:next];
    [NSLayoutConstraint activateConstraints:@[
        [prev.leadingAnchor  constraintEqualToAnchor:right.leadingAnchor],
        [prev.centerYAnchor  constraintEqualToAnchor:right.centerYAnchor],
        [next.leadingAnchor  constraintEqualToAnchor:prev.trailingAnchor constant:0.0],
        [next.centerYAnchor  constraintEqualToAnchor:right.centerYAnchor],
        [next.trailingAnchor constraintEqualToAnchor:right.trailingAnchor],
        [right.topAnchor     constraintEqualToAnchor:prev.topAnchor],
        [right.bottomAnchor  constraintEqualToAnchor:prev.bottomAnchor],
    ]];

    [header addSubview:left];
    [header addSubview:label];
    [header addSubview:right];

    /* Left flush, right flush, label centered — give the center label a
     * lower-priority centering constraint so it yields gracefully when space
     * is tight rather than forcing the side sections to overlap. */
    NSLayoutConstraint *cx = [label.centerXAnchor constraintEqualToAnchor:header.centerXAnchor];
    cx.priority = NSLayoutPriorityDefaultHigh;

    [NSLayoutConstraint activateConstraints:@[
        [left.leadingAnchor   constraintEqualToAnchor:header.leadingAnchor  constant:6.0],
        [left.centerYAnchor   constraintEqualToAnchor:header.centerYAnchor],

        [label.centerYAnchor  constraintEqualToAnchor:header.centerYAnchor],
        [label.leadingAnchor  constraintGreaterThanOrEqualToAnchor:left.trailingAnchor  constant:4.0],
        [label.trailingAnchor constraintLessThanOrEqualToAnchor:right.leadingAnchor constant:-4.0],
        cx,

        [right.trailingAnchor constraintEqualToAnchor:header.trailingAnchor constant:-6.0],
        [right.centerYAnchor  constraintEqualToAnchor:header.centerYAnchor],
    ]];

    /* 1 pt separator below the header */
    NSView *sep = [[NSView alloc] initWithFrame:NSZeroRect];
    sep.wantsLayer = YES;
    sep.layer.backgroundColor = NSColor.separatorColor.CGColor;
    sep.translatesAutoresizingMaskIntoConstraints = NO;

    /* The all-day strip: the day's all-day calendar events, one line,
     * collapsed to nothing on a day without any. */
    NSTextField *all_day = [NSTextField labelWithString:@""];
    all_day.font          = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    all_day.textColor     = NSColor.secondaryLabelColor;
    all_day.lineBreakMode = NSLineBreakByTruncatingTail;
    all_day.hidden        = YES;
    all_day.translatesAutoresizingMaskIntoConstraints = NO;
    [all_day setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                      forOrientation:NSLayoutConstraintOrientationHorizontal];
    _all_day_label  = all_day;
    _all_day_height = [all_day.heightAnchor constraintEqualToConstant:0.0];

    [self addSubview:header];
    [self addSubview:all_day];
    [self addSubview:sep];
    [self addSubview:_scroll];

    [NSLayoutConstraint activateConstraints:@[
        [header.topAnchor      constraintEqualToAnchor:self.topAnchor],
        [header.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [header.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [header.heightAnchor   constraintEqualToConstant:header_h],

        [all_day.topAnchor      constraintEqualToAnchor:header.bottomAnchor],
        [all_day.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor constant:8.0],
        [all_day.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-8.0],
        _all_day_height,

        [sep.topAnchor         constraintEqualToAnchor:all_day.bottomAnchor],
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
 * update_header_controls — sync the date label text and Today button enabled
 * state to the current days_offset, and the clock timer with it: the
 * current-time line is drawn on today only.
 */
- (void)
update_header_controls
{
    time_t    t     = (time_t)midnight_for_offset(_days_offset);
    struct tm local = *localtime(&t);
    char      buf[64];
    strftime(buf, sizeof(buf), "%a, %b %-d", &local);
    _date_label.stringValue = @(buf);
    _today_button.enabled   = (_days_offset != 0);
    [self sync_clock_timer];
}

/*
 * sync_clock_timer — run the timer that moves the current-time line only
 * while the line is on screen: the panel shown, today displayed.  A hidden
 * panel's timer dirtied a canvas nobody saw every 30 seconds.  The
 * tolerance lets the system fire it alongside other work.
 */
- (void)
sync_clock_timer
{
    BOOL wanted = !self.hidden && _days_offset == 0 && !self.is_shut_down;

    if (!wanted) {
        [self.clock_timer invalidate];
        self.clock_timer = nil;
        return;
    }
    if (self.clock_timer != nil) return;

    __weak TasksDayPlanner *ws = self;
    self.clock_timer = [NSTimer scheduledTimerWithTimeInterval:30.0
                                                       repeats:YES
                                                         block:^(NSTimer *t) {
        [ws.canvas redraw_now_line];
    }];
    self.clock_timer.tolerance = 5.0;
}

/*
 * setHidden: — the window shows or hides the panel: the clock runs only
 * while it shows.
 *
 * Inputs:
 *   hidden — whether the panel is hidden
 */
- (void)
setHidden:(BOOL)hidden
{
    [super setHidden:hidden];
    [self sync_clock_timer];
}

/*
 * pick_displayed_date: — a calendar in a tool panel to jump to any date.
 *
 * Inputs:
 *   sender — the calendar button (unused)
 */
- (void)
pick_displayed_date:(id)sender
{
    time_t        t      = (time_t)midnight_for_offset(_days_offset);
    NSDatePicker *picker = [[NSDatePicker alloc] initWithFrame:NSZeroRect];
    picker.datePickerStyle    = NSDatePickerStyleClockAndCalendar;
    picker.datePickerElements = NSDatePickerElementFlagYearMonthDay;
    picker.dateValue          = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)t];
    NSSize size = picker.fittingSize;
    picker.translatesAutoresizingMaskIntoConstraints = NO;
    [picker.widthAnchor  constraintEqualToConstant:size.width].active  = YES;
    [picker.heightAnchor constraintEqualToConstant:size.height].active = YES;

    __weak TasksDayPlanner *ws = self;
    TasksToolPanel *panel = [TasksToolPanel
        panel_with_title:@"Go to Date" hero_title:@"Go to Date" subtitle:@"" symbol:@"calendar"
                 content:picker buttons:@[@"Go", @"Cancel"] destructive:-1
                 handler:^NSString *(NSInteger index) {
        TasksDayPlanner *s = ws;
        if (!s || index != 0) return nil;
        NSCalendar       *cal    = NSCalendar.currentCalendar;
        NSDate           *today  = [cal startOfDayForDate:[NSDate date]];
        NSDate           *chosen = [cal startOfDayForDate:picker.dateValue];
        NSDateComponents *diff   = [cal components:NSCalendarUnitDay
                                          fromDate:today toDate:chosen options:0];
        s->_days_offset = (NSInteger)diff.day;
        [s update_header_controls];
        [s reload];
        [s scroll_to_now];
        return nil;
    }];
    [panel show_over:self.window];
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

    gint64 lo, hi;
    task_day_bounds((gint)_days_offset, &lo, &hi);
    TaskDatabase *db    = self.library.app->db;
    GPtrArray    *tasks = task_db_tasks_scheduled_for_day(db, lo, hi);

    /* The old items are the tooltip rectangles' subject: drop those first. */
    [self.canvas removeAllToolTips];
    [self.items removeAllObjects];
    NSMutableSet<NSNumber *> *present = [NSMutableSet set];
    for (guint i = 0; i < tasks->len; i++) {
        Task          *t    = g_ptr_array_index(tasks, i);
        TasksDayItem  *item = [[TasksDayItem alloc] init];
        item.task_id    = t->id;
        item.title      = tasks_title_string(t, @"Untitled");
        item.start_unix = t->scheduled_start;
        item.duration   = t->scheduled_duration > 0 ? t->scheduled_duration
                                                    : SHARED_DB_TASK_SCHEDULED_DURATION_DEFAULT;
        item.status     = t->status;
        [self.items addObject:item];
        [present addObject:@(t->id)];
    }
    task_ptr_array_free_tasks(tasks);

    [self add_events_from:lo to:hi];
    [self fit_hours_from:lo];
    layout_columns(self.items);

    /* A reload (a sync finishing, an edit elsewhere) keeps the selection
     * of every block still on the day. */
    [self.canvas.selected_ids intersectSet:present];

    [self.canvas setNeedsDisplay:YES];
    [self.canvas.window invalidateCursorRectsForView:self.canvas];
}

/*
 * add_events_from:to: — add the day's Google Calendar events (the sync's
 * cache, gcal_store.h) while the sync is switched on: timed ones as
 * blocks, all-day ones to the strip under the header.  Signing out empties
 * the cache, so the switch is the only thing to ask.
 *
 * Inputs:
 *   lo — the day's local midnight
 *   hi — the next local midnight
 */
- (void)
add_events_from:(gint64)lo to:(gint64)hi
{
    NSMutableArray<NSString *> *all_day = [NSMutableArray array];

    if (task_config_get_bool(TASK_CFG_GCAL_ENABLED)) {
        GPtrArray *events = gcal_store_events_for_day(self.library.app->db, lo, hi);
        for (guint i = 0; i < events->len; i++) {
            const gcal_event_t *e     = g_ptr_array_index(events, i);
            NSString           *title = *e->title != '\0' ? tasks_string(e->title) : @"Busy";
            if (e->all_day) {
                [all_day addObject:title];
                continue;
            }
            int64_t start = MAX(e->start, lo);
            int64_t end   = MIN(e->end, hi);
            NSColor *base = calendar_color(e->color);

            TasksDayItem *item = [[TasksDayItem alloc] init];
            item.is_event   = YES;
            item.title      = title;
            item.start_unix = start;
            item.duration   = (int)MAX((end - start) / 60, 1);
            item.event_fill = tasks_color_dimmed(base, 22);
            item.event_bar  = base;
            [self.items addObject:item];
        }
        g_ptr_array_free(events, TRUE);
    }

    NSString *text = [all_day componentsJoinedByString:@" · "];
    self.all_day_label.stringValue = text;
    self.all_day_label.toolTip     = all_day.count > 0 ? text : nil;
    self.all_day_label.hidden      = all_day.count == 0;
    self.all_day_height.constant   = all_day.count > 0 ? all_day_h : 0.0;
}

/*
 * fit_hours_from: — show business hours, widened to whole hours that take
 * in every block of the day, and size the canvas to them.  A wider top
 * shifts everything down, so the scroll position follows to keep the same
 * times in view.
 *
 * Inputs:
 *   lo — the day's local midnight
 */
- (void)
fit_hours_from:(gint64)lo
{
    NSInteger first = tasks_look_day_planner_start_h;
    NSInteger end   = tasks_look_day_planner_end_h;
    for (TasksDayItem *item in self.items) {
        int64_t from = item.start_unix - lo;
        int64_t to   = from + (int64_t)item.duration * 60;
        first = MIN(first, (NSInteger)MAX(from, 0) / 3600);
        end   = MAX(end, (NSInteger)((MIN(to, 24 * 3600) + 3599) / 3600));
    }

    NSInteger shift = self.canvas.first_h - first;
    if (shift == 0 && end == self.canvas.end_h) return;

    self.canvas.first_h = first;
    self.canvas.end_h   = end;
    NSPoint origin = self.scroll.contentView.bounds.origin;
    [self.canvas setFrameSize:NSMakeSize(NSWidth(self.canvas.frame), canvas_height(self.canvas))];
    origin.y = MAX(0.0, origin.y + shift * tasks_look_day_planner_hour_h);
    [self.canvas scrollPoint:origin];
}

/*
 * committed_block — a move or a stretch was written: lay the blocks out
 * again (the block may now overlap others, or no longer), and let the
 * calendar sync push it.
 */
- (void)
committed_block
{
    layout_columns(self.items);
    [self.canvas setNeedsDisplay:YES];
    [self.canvas.window invalidateCursorRectsForView:self.canvas];
    gcal_sync_kick();
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
 * remove_selected_from_schedule: — clear the schedule fields for every
 * selected block and reload the view.
 *
 * Inputs:
 *   sender — unused (called from the context menu)
 */
- (void)
remove_selected_from_schedule:(id)sender
{
    NSSet<NSNumber *> *ids    = [self.canvas.selected_ids copy];
    BOOL               failed = NO;
    for (NSNumber *tid in ids)
        failed = !task_db_task_set_scheduled(self.library.app->db, tid.longLongValue, 0, 0) ||
                 failed;
    [self.canvas.selected_ids removeAllObjects];
    if (failed) task_app_write_failed(self.library.app, "Removing from the schedule");
    [self reload];
    [self.library refresh_tasks];
    gcal_sync_kick();
}

/*
 * shut_down — stop the clock timer and stop watching the scroll view.
 * Idempotent.
 * Documented in day_planner.h.
 */
- (void)
shut_down
{
    if (self.is_shut_down) return;
    self.is_shut_down = YES;
    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:NSViewFrameDidChangeNotification
                                                  object:self.scroll.contentView];
    [self sync_clock_timer];        /* shut down: stops it */
}

@end
