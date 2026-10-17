#import "kanban.h"
#import "editor_window.h"
#import "look.h"
#import "row_text.h"
#import "ui_util.h"

#include "task_board.h"
#include "task_config.h"

/*
 * The Kanban board is four small classes, outermost first:
 *
 *   TasksKanban        the pane.  Owns the rows, the board made from them
 *                      (task_board.h: which card is in which lane, in what
 *                      order), the selection and the drag.
 *   TasksKanbanCanvas  the scroll view's document view: a flipped container,
 *                      so the three lanes scroll vertically as one.
 *   TasksKanbanLane    one lane: draws its heading, its tinted box, the
 *                      card shadows and the insertion bar; stacks its
 *                      cards; is the drop target of a card drag.
 *   TasksKanbanCard    one card: draws itself, reports clicks, starts a
 *                      drag from its grip.
 *
 * WHAT IS DECIDED WHERE.  Everything with arithmetic in it — lanes, the
 * saved order, the Done lane's order and cap, the slot under the pointer,
 * what a drop changes — is task_board.h, plain C with tests.  This file
 * turns that board into views and feeds the mouse in.
 *
 * REFRESH.  show_rows: never tears the board down.  Cards are kept by task
 * id: a card whose task is still shown keeps its view (and so its hover,
 * its place in a drag, its drawing), has its text replaced only when it
 * changed, and is moved only when its frame changed.  The canvas stays in
 * the scroll view, so the scroll position survives by itself.
 *
 * THE DRAG is AppKit's own dragging session: it supplies the translucent
 * image that follows the pointer, Escape to cancel, and the slide back on a
 * refused drop.  What is being dragged is kept HERE (drag_ids), not on the
 * pasteboard, because a card drag never leaves the board.  Every end of a
 * drag — a drop, a refused drop, Escape, the pane being hidden — goes
 * through cancel_drag, the one place that puts the board back.
 */

/* The pasteboard type that marks a card drag. */
NSPasteboardType const tasks_card_drag_type = @TASKS_BUNDLE_ID ".kanban-card";

/* Layout the look.h Kanban section has no entry for.  From the GTK build. */
static const CGFloat lane_heading_gap  = 4.0;   /* a lane's heading to its box   */
static const CGFloat lane_empty_pad    = 10.0;  /* above and below the prompt of
                                                 * an empty lane                 */
static const CGFloat done_link_gap     = 4.0;   /* last Done card to the link    */
static const CGFloat card_shadow_shift = 2.0;   /* the shadow falls right and
                                                 * down by this much             */
static const CGFloat card_shadow_inset = 1.0;   /* ... from a shape this much
                                                 * smaller than the card         */
static const CGFloat badge_from_right  = 30.0;  /* the count badge's left edge,
                                                 * from the ghost's right edge   */
static const CGFloat badge_from_top    = 4.0;
static const CGFloat badge_pad_h       = 5.0;   /* badge edge to its number      */
static const CGFloat badge_pad_v       = 1.0;
static const CGFloat drag_threshold    = 4.0;   /* a press on the grip becomes a
                                                 * drag after this many points   */

@class TasksKanbanCard;
@class TasksKanbanLane;

/* ── What the cards and lanes use of the pane ───────────────────────── */

@interface TasksKanban () <NSDraggingSource>

/* The window hosting the pane; the shared state is read through it. */
@property (nonatomic, weak) TasksLibraryWindow *library;

/* The scroll view and its document view, which holds the three lanes. */
@property (nonatomic, strong) NSScrollView *scroll;
@property (nonatomic, strong) NSView *canvas;

/* The lanes, indexed by TaskStatus. */
@property (nonatomic, copy) NSArray<TasksKanbanLane *> *lanes;

/* Every card on the board, by task id.  Replaced on each refresh; a card
 * found here is reused rather than rebuilt. */
@property (nonatomic, copy) NSDictionary<NSNumber *, TasksKanbanCard *> *cards;

/* The selected task ids, and the card a Shift-click extends from (the last
 * one plainly or Command-clicked; 0 = none). */
@property (nonatomic, strong) NSMutableSet<NSNumber *> *selection;
@property (nonatomic) int64_t anchor_id;

/* YES while the Done lane shows every completed task instead of the most
 * recent few.  Reset when the sidebar selection changes. */
@property (nonatomic) BOOL done_show_all;

/* The task ids being dragged, in display order; nil when no drag is in
 * flight.  Set by begin_drag_from:event:, cleared by cancel_drag. */
@property (nonatomic, copy) NSArray<NSNumber *> *drag_ids;

/* A card was pressed: select, or open the editor on a double-click. */
- (void)card_pressed:(TasksKanbanCard *)card event:(NSEvent *)event;

/* The press ended without a drag: a plain click collapses the selection. */
- (void)card_released:(TasksKanbanCard *)card event:(NSEvent *)event;

/* The pointer left the grip far enough: start dragging the selection. */
- (void)begin_drag_from:(TasksKanbanCard *)card event:(NSEvent *)event;

/* The context menu for a card, after making it part of the selection. */
- (NSMenu *)menu_for_card:(TasksKanbanCard *)card;

/* Select exactly one card; nil selects nothing. */
- (void)select_only:(NSNumber *)task_id;

/* The dragged cards were released over a lane. */
- (BOOL)drop_on_lane:(TaskStatus)lane slot:(size_t)slot;

@end

/* ── Shared drawing ─────────────────────────────────────────────────── */

/*
 * grip_glyph — the dotted grip drawn at a card's left edge, the one part of
 * a card a drag starts from.
 *
 * Output:
 *   shared attributed string.  Its colour, like that of the lanes' own
 *   dimmed text below, is the system's secondary label colour: it is
 *   resolved when drawn, so one string serves light and dark.
 */
static NSAttributedString *
grip_glyph(void)
{
    static NSAttributedString *glyph;
    static dispatch_once_t once;

    dispatch_once(&once, ^{
        glyph = [[NSAttributedString alloc]
            initWithString:@"⠿"
                attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:shared_look_ui_font],
                              NSForegroundColorAttributeName: NSColor.secondaryLabelColor }];
    });
    return glyph;
}

/*
 * grip_width — the width of a card's grip strip: the glyph and the room
 * either side of it.  A press left of this may start a drag.
 *
 * Output:
 *   the width in points.
 */
static CGFloat
grip_width(void)
{
    static CGFloat width;
    static dispatch_once_t once;

    /* Measured once: every card asks on every draw and cursor-rect reset. */
    dispatch_once(&once, ^{
        width = ceil(grip_glyph().size.width) + 2.0 * tasks_look_card_grip_pad;
    });
    return width;
}

/*
 * centred_style — the paragraph style of a lane's own text: centred, cut
 * with an ellipsis rather than wrapped.
 *
 * Output:
 *   new paragraph style.
 */
static NSParagraphStyle *
centred_style(void)
{
    NSMutableParagraphStyle *style = [[NSMutableParagraphStyle alloc] init];

    style.alignment     = NSTextAlignmentCenter;
    style.lineBreakMode = NSLineBreakByTruncatingTail;
    return style;
}

/*
 * lane_heading — a lane's heading: its name in bold over the number of
 * tasks it holds.
 *
 * Inputs:
 *   status — the lane
 *   total  — how many tasks it holds (not how many cards a capped Done
 *            lane shows)
 *
 * Output:
 *   new attributed string of two centred lines.
 */
static NSAttributedString *
lane_heading(TaskStatus status, NSUInteger total)
{
    NSParagraphStyle *style = centred_style();
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc]
        initWithString:tasks_string(task_status_label(status))
            attributes:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:shared_look_ui_font],
                          NSForegroundColorAttributeName: NSColor.labelColor,
                          NSParagraphStyleAttributeName: style }];
    NSString *count = [NSString stringWithFormat:@"\n%lu task%@", (unsigned long)total,
                                                 total == 1 ? @"" : @"s"];

    [text appendAttributedString:[[NSAttributedString alloc]
        initWithString:count
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:shared_look_ui_font],
                          NSForegroundColorAttributeName: NSColor.secondaryLabelColor,
                          NSParagraphStyleAttributeName: style }]];
    return text;
}

/*
 * empty_prompt — what an empty lane says, so that it still reads as a place
 * to drop a card.
 *
 * Output:
 *   shared attributed string: italic, dimmed, centred.
 */
static NSAttributedString *
empty_prompt(void)
{
    static NSAttributedString *prompt;
    static dispatch_once_t once;

    dispatch_once(&once, ^{
        NSFont *italic = [NSFontManager.sharedFontManager
            convertFont:[NSFont systemFontOfSize:shared_look_ui_font]
            toHaveTrait:NSItalicFontMask];
        prompt = [[NSAttributedString alloc]
            initWithString:@"Drop a task here"
                attributes:@{ NSFontAttributeName: italic,
                              NSForegroundColorAttributeName: NSColor.secondaryLabelColor,
                              NSParagraphStyleAttributeName: centred_style() }];
    });
    return prompt;
}

/*
 * draw_badge — the count on the ghost of a multi-card drag: a white number
 * on a rounded plate near the ghost's top-right corner.
 *
 * Inputs:
 *   count — how many cards are being dragged
 *   ghost — the ghost image's rectangle (unflipped: y grows upwards)
 */
static void
draw_badge(NSUInteger count, NSRect ghost)
{
    NSAttributedString *number = [[NSAttributedString alloc]
        initWithString:[NSString stringWithFormat:@"%lu", (unsigned long)count]
            attributes:@{ NSFontAttributeName:
                              [NSFont boldSystemFontOfSize:shared_look_ui_font],
                          NSForegroundColorAttributeName: NSColor.whiteColor }];
    NSSize  size  = number.size;
    NSRect  plate = NSMakeRect(NSMaxX(ghost) - badge_from_right,
                               NSMaxY(ghost) - badge_from_top - size.height - 2.0 * badge_pad_v,
                               size.width + 2.0 * badge_pad_h, size.height + 2.0 * badge_pad_v);
    CGFloat radius = MIN(tasks_look_badge_radius, NSHeight(plate) / 2.0);

    [tasks_color(TasksColorBadge) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:plate xRadius:radius yRadius:radius] fill];
    [number drawAtPoint:NSMakePoint(NSMinX(plate) + badge_pad_h, NSMinY(plate) + badge_pad_v)];
}

/* ── Card ───────────────────────────────────────────────────────────── */

@interface TasksKanbanCard : NSView

/* The pane, which owns the selection and the drag. */
@property (nonatomic, weak) TasksKanban *kanban;

/* The task this card stands for.  A card never changes task. */
@property (nonatomic, readonly) int64_t task_id;

/* What the card says (row_text.h) and the height that text needs. */
@property (nonatomic, strong) NSAttributedString *text;
@property (nonatomic) CGFloat text_height;

/* How the card is drawn: selected, under the pointer, being dragged. */
@property (nonatomic) BOOL selected;
@property (nonatomic) BOOL hovered;
@property (nonatomic) BOOL dragged;

/* The press in progress: where it began, whether it began on the grip (so
 * may become a drag), and whether it did become one. */
@property (nonatomic) NSPoint press_point;
@property (nonatomic) BOOL grip_pressed;
@property (nonatomic) BOOL drag_started;

@end

@implementation TasksKanbanCard

/*
 * init_with_kanban:task_id: — make the card for one task.  It has no text
 * and no frame yet; the pane gives it both.
 *
 * Inputs:
 *   kanban  — the pane; held weakly
 *   task_id — the task
 *
 * Output:
 *   the card.
 */
- (instancetype)
init_with_kanban:(TasksKanban *)kanban task_id:(int64_t)task_id
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _kanban  = kanban;
    _task_id = task_id;

    /* One area that follows the visible part of the card, whatever its
     * frame becomes: the hover border needs enter and exit only. */
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil]];
    return self;
}

/*
 * isFlipped — y grows downwards, so the text is laid out from the top.
 *
 * Output:
 *   YES.
 */
- (BOOL)
isFlipped
{
    return YES;
}

/*
 * show_text: — set what the card says.  Text equal to what is shown changes
 * nothing, which is what keeps an untouched card from redrawing on refresh.
 *
 * Inputs:
 *   text — the row's text from tasks_row_text
 */
- (void)
show_text:(NSAttributedString *)text
{
    if ([text isEqualToAttributedString:self.text]) return;

    /* A row's text is one line per paragraph whatever the width
     * (row_text.h), so the height is measured once, here, not per layout. */
    self.text         = text;
    self.text_height  = tasks_row_text_height(text);
    self.needsDisplay = YES;
}

/*
 * fit_height — the height the card needs: its text and the padding above
 * and below it.
 *
 * Output:
 *   the height in points.
 */
- (CGFloat)
fit_height
{
    return self.text_height + 2.0 * tasks_look_card_pad;
}

/*
 * show_selected: — draw the card as selected or not.
 *
 * Inputs:
 *   selected — whether its task is in the pane's selection
 */
- (void)
show_selected:(BOOL)selected
{
    if (selected == self.selected) return;

    self.selected     = selected;
    self.needsDisplay = YES;
}

/*
 * show_dragged: — dim the card while its ghost is in flight, and tell the
 * lane, which leaves the shadow off a dimmed card: a crisp shadow under a
 * faded card reads as the shadow having come loose.
 *
 * Inputs:
 *   dragged — whether the card is part of the drag in flight
 */
- (void)
show_dragged:(BOOL)dragged
{
    if (dragged == self.dragged) return;

    self.dragged                = dragged;
    self.alphaValue             = dragged ? tasks_look_card_src_alpha : 1.0;
    self.superview.needsDisplay = YES;
}

/*
 * ghost_with_count: — the image that follows the pointer during a drag: the
 * card as it looks now, translucent, with a count badge when it stands for
 * several cards.  Take it BEFORE the card is dimmed.
 *
 * Inputs:
 *   count — how many cards the drag moves
 *
 * Output:
 *   new image, the card's size.
 */
- (NSImage *)
ghost_with_count:(NSUInteger)count
{
    NSRect bounds = self.bounds;
    NSBitmapImageRep *rep = [self bitmapImageRepForCachingDisplayInRect:bounds];

    [self cacheDisplayInRect:bounds toBitmapImageRep:rep];
    return [NSImage imageWithSize:bounds.size
                          flipped:NO
                   drawingHandler:^BOOL(NSRect rect) {
        [rep drawInRect:rect
               fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver
               fraction:tasks_look_card_ghost_alpha
         respectFlipped:YES
                  hints:nil];
        if (count > 1) draw_badge(count, rect);
        return YES;
    }];
}

/*
 * drawRect: — the card: its fill and border, the grip, the text.
 *
 * Inputs:
 *   dirty — the part to redraw (unused: a card is small, it is drawn whole)
 */
- (void)
drawRect:(NSRect)dirty
{
    NSRect  bounds = self.bounds;
    NSColor *border;

    [NSColor.textBackgroundColor setFill];
    NSRectFill(bounds);
    if (self.selected) {
        [tasks_color(TasksColorCardSelected) setFill];
        NSRectFillUsingOperation(bounds, NSCompositingOperationSourceOver);
        border = NSColor.controlAccentColor;
    } else {
        border = tasks_color(self.hovered ? TasksColorCardBorderHover : TasksColorCardBorder);
    }
    [border setStroke];
    [NSBezierPath strokeRect:NSInsetRect(bounds, 0.5, 0.5)];

    NSAttributedString *grip = grip_glyph();
    [grip drawAtPoint:NSMakePoint(tasks_look_card_grip_pad,
                                  floor((NSHeight(bounds) - grip.size.height) / 2.0))];

    CGFloat left = grip_width() + tasks_look_card_pad;
    [self.text drawWithRect:NSMakeRect(left, tasks_look_card_pad,
                                       NSWidth(bounds) - left - tasks_look_card_pad,
                                       self.text_height)
                    options:NSStringDrawingUsesLineFragmentOrigin |
                            NSStringDrawingUsesFontLeading];
}

/*
 * resetCursorRects — the open hand over the grip says "drag from here".
 */
- (void)
resetCursorRects
{
    [self addCursorRect:NSMakeRect(0.0, 0.0, grip_width(), NSHeight(self.bounds))
                 cursor:NSCursor.openHandCursor];
}

/*
 * mouseEntered: — the pointer is over the card: darken its border.
 *
 * Inputs:
 *   event — the tracking event (unused)
 */
- (void)
mouseEntered:(NSEvent *)event
{
    self.hovered      = YES;
    self.needsDisplay = YES;
}

/*
 * mouseExited: — the pointer left the card.
 *
 * Inputs:
 *   event — the tracking event (unused)
 */
- (void)
mouseExited:(NSEvent *)event
{
    self.hovered      = NO;
    self.needsDisplay = YES;
}

/*
 * mouseDown: — a press: the pane selects (or opens the editor on a double
 * click); a press on the grip is remembered because it may become a drag.
 *
 * Inputs:
 *   event — the mouse-down event
 */
- (void)
mouseDown:(NSEvent *)event
{
    self.press_point  = [self convertPoint:event.locationInWindow fromView:nil];
    self.grip_pressed = self.press_point.x < grip_width();
    self.drag_started = NO;
    [self.kanban card_pressed:self event:event];
}

/*
 * mouseDragged: — the pointer moved with the button down.  Once a press on
 * the grip has travelled past the threshold it becomes a drag, once.
 *
 * Inputs:
 *   event — the mouse-dragged event
 */
- (void)
mouseDragged:(NSEvent *)event
{
    NSPoint at = [self convertPoint:event.locationInWindow fromView:nil];

    if (!self.grip_pressed || self.drag_started) return;
    if (hypot(at.x - self.press_point.x, at.y - self.press_point.y) < drag_threshold) return;

    self.drag_started = YES;
    [NSCursor.closedHandCursor push];
    [self.kanban begin_drag_from:self event:event];
}

/*
 * mouseUp: — the press ended.  If it never became a drag it was a click,
 * which the pane may answer by collapsing a multi-selection.
 *
 * Inputs:
 *   event — the mouse-up event
 */
- (void)
mouseUp:(NSEvent *)event
{
    if (!self.drag_started) [self.kanban card_released:self event:event];
}

/*
 * menuForEvent: — a right-click or Control-click: the shared task menu,
 * after the pane has made this card part of the selection.
 *
 * Inputs:
 *   event — the click (unused)
 *
 * Output:
 *   the menu, or nil when there is nothing to act on.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    return [self.kanban menu_for_card:self];
}

@end

/* ── Lane ───────────────────────────────────────────────────────────── */

@interface TasksKanbanLane : NSView

/* The pane, which carries out a drop. */
@property (nonatomic, weak) TasksKanban *kanban;

/* Which lane this is: the status a card dropped here takes. */
@property (nonatomic, readonly) TaskStatus status;

/* The cards shown, top to bottom, and how many tasks the lane holds (more
 * than the cards only when the Done lane is capped). */
@property (nonatomic, copy) NSArray<TasksKanbanCard *> *cards;
@property (nonatomic) NSUInteger total;

/* The heading drawn above the box: the name over the task count. */
@property (nonatomic, strong) NSAttributedString *heading;

/* The Done lane's "Show all N completed" link; nil in the other lanes. */
@property (nonatomic, strong) NSButton *link;

/* Whether cards cast a shadow (the kanban_shadow setting). */
@property (nonatomic) BOOL shadowed;

/* While a drag is over the lane: the lane is tinted, and the insertion bar
 * is drawn at mark_slot (-1 = no bar). */
@property (nonatomic) BOOL targeted;
@property (nonatomic) NSInteger mark_slot;

@end

@implementation TasksKanbanLane

/*
 * init_with_kanban:status: — make one empty lane.
 *
 * Inputs:
 *   kanban — the pane; held weakly
 *   status — which lane
 *
 * Output:
 *   the lane.
 */
- (instancetype)
init_with_kanban:(TasksKanban *)kanban status:(TaskStatus)status
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _kanban    = kanban;
    _status    = status;
    _cards     = @[];
    _heading   = lane_heading(status, 0);
    _mark_slot = -1;
    [self registerForDraggedTypes:@[tasks_card_drag_type]];

    if (status == TASK_STATUS_DONE) {
        _link = [NSButton buttonWithTitle:@"" target:kanban action:@selector(done_link_clicked:)];
        _link.bordered = NO;
        _link.hidden   = YES;
        [self addSubview:_link];
    }
    return self;
}

/*
 * isFlipped — y grows downwards: cards stack from the top.
 *
 * Output:
 *   YES.
 */
- (BOOL)
isFlipped
{
    return YES;
}

/*
 * show_cards:total: — replace the cards the lane shows.  A card already in
 * the lane stays where it is in the view tree; one arriving from another
 * lane is moved; one no longer listed is removed (unless another lane has
 * already taken it).
 *
 * Inputs:
 *   cards — the cards to show, top to bottom
 *   total — how many tasks the lane holds
 */
- (void)
show_cards:(NSArray<TasksKanbanCard *> *)cards total:(NSUInteger)total
{
    NSSet<TasksKanbanCard *> *staying = [NSSet setWithArray:cards];

    if ([cards isEqualToArray:self.cards] && total == self.total) return;

    for (TasksKanbanCard *card in self.cards) {
        if (card.superview == self && ![staying containsObject:card]) [card removeFromSuperview];
    }
    for (TasksKanbanCard *card in cards) {
        if (card.superview != self) [self addSubview:card];
    }
    if (total != self.total) self.heading = lane_heading(self.status, total);
    self.cards        = cards;
    self.total        = total;
    self.needsDisplay = YES;
}

/*
 * show_link: — show, retitle or hide the Done lane's link.
 *
 * Inputs:
 *   title — what a click will do ("Show all 14 completed"), or nil to hide
 *           the link
 */
- (void)
show_link:(NSString *)title
{
    self.link.hidden = title == nil;
    if (title == nil) return;

    self.link.attributedTitle = [[NSAttributedString alloc]
        initWithString:title
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:shared_look_ui_font],
                          NSForegroundColorAttributeName: shared_ui_color(shared_look_link),
                          NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle) }];
    [self.link sizeToFit];
}

/*
 * show_shadows: — turn the card shadows on or off.
 *
 * Inputs:
 *   shadowed — the kanban_shadow setting
 */
- (void)
show_shadows:(BOOL)shadowed
{
    if (shadowed == self.shadowed) return;

    self.shadowed     = shadowed;
    self.needsDisplay = YES;
}

/*
 * show_drop_target:mark: — show where a drag would land: the lane's tint
 * and the insertion bar.  Redraws only on a change, as the pointer reports
 * the same slot many times over.
 *
 * Inputs:
 *   targeted — whether the drag is over this lane
 *   slot     — the slot to draw the bar at, or -1 for no bar
 */
- (void)
show_drop_target:(BOOL)targeted mark:(NSInteger)slot
{
    if (targeted == self.targeted && slot == self.mark_slot) return;

    self.targeted     = targeted;
    self.mark_slot    = slot;
    self.needsDisplay = YES;
}

/*
 * box_top — where the lane's tinted box begins, below the heading.
 *
 * Output:
 *   the y coordinate.
 */
- (CGFloat)
box_top
{
    return ceil(self.heading.size.height) + lane_heading_gap;
}

/*
 * arrange_for_width: — stack the cards (and the Done link) for a lane of
 * the given width.  A card whose frame is already right is not touched.
 *
 * Inputs:
 *   width — the lane's width
 *
 * Output:
 *   the height the lane needs to hold everything.
 */
- (CGFloat)
arrange_for_width:(CGFloat)width
{
    CGFloat pad    = tasks_look_lane_pad;
    CGFloat y      = [self box_top] + pad;
    CGFloat bottom = y;             /* the end of the last thing placed     */

    for (TasksKanbanCard *card in self.cards) {
        NSRect frame = NSMakeRect(pad, y, width - 2.0 * pad, [card fit_height]);
        if (!NSEqualRects(card.frame, frame)) {
            card.frame        = frame;
            self.needsDisplay = YES;    /* its shadow moved with it         */
        }
        bottom = NSMaxY(frame);
        y      = bottom + tasks_look_lane_gap;
    }
    if (self.cards.count == 0) bottom += 2.0 * lane_empty_pad + ceil(empty_prompt().size.height);

    if (self.link != nil && !self.link.hidden) {
        NSSize size = self.link.frame.size;
        self.link.frame = NSMakeRect(floor((width - size.width) / 2.0), bottom + done_link_gap,
                                     size.width, size.height);
        bottom = NSMaxY(self.link.frame);
        [self.window invalidateCursorRectsForView:self];
    }
    return bottom + pad;
}

/*
 * mark_rect — where the insertion bar is drawn for mark_slot: in the middle
 * of the gap the dropped cards would open up.
 *
 * Output:
 *   the bar's rectangle.
 */
- (NSRect)
mark_rect
{
    NSUInteger count = self.cards.count;
    NSUInteger slot  = (NSUInteger)self.mark_slot;
    CGFloat    pad   = tasks_look_lane_pad;
    CGFloat    middle;

    if (count == 0 || slot == 0) {
        middle = [self box_top] + pad / 2.0;
    } else if (slot >= count) {
        middle = NSMaxY(self.cards[count - 1].frame) + tasks_look_lane_gap / 2.0;
    } else {
        middle = (NSMaxY(self.cards[slot - 1].frame) + NSMinY(self.cards[slot].frame)) / 2.0;
    }
    return NSMakeRect(pad, round(middle - tasks_look_card_mark_h / 2.0),
                      NSWidth(self.bounds) - 2.0 * pad, tasks_look_card_mark_h);
}

/*
 * draw_shadows: — the cards' drop shadows.  The lane draws them, under the
 * cards, so that they follow light and dark like every other colour here
 * and need no layer per card.  Each is cast by a shape the card then covers.
 *
 * Inputs:
 *   dirty — the part being redrawn
 */
- (void)
draw_shadows:(NSRect)dirty
{
    CGFloat   reach  = card_shadow_shift + tasks_look_card_shadow_blur;
    NSShadow *shadow = [[NSShadow alloc] init];

    shadow.shadowColor      = tasks_color(TasksColorCardShadow);
    shadow.shadowOffset     = NSMakeSize(card_shadow_shift, -card_shadow_shift);
    shadow.shadowBlurRadius = tasks_look_card_shadow_blur;

    [NSGraphicsContext saveGraphicsState];
    [shadow set];
    [NSColor.textBackgroundColor setFill];
    for (TasksKanbanCard *card in self.cards) {
        if (card.dragged || !NSIntersectsRect(NSInsetRect(card.frame, -reach, -reach), dirty)) {
            continue;
        }
        [[NSBezierPath bezierPathWithRect:NSInsetRect(card.frame, card_shadow_inset,
                                                      card_shadow_inset)] fill];
    }
    [NSGraphicsContext restoreGraphicsState];
}

/*
 * drawRect: — the lane: heading, tinted box (in the target tint while a
 * drag is over it), the prompt of an empty lane, the card shadows and the
 * insertion bar.
 *
 * Inputs:
 *   dirty — the part to redraw
 */
- (void)
drawRect:(NSRect)dirty
{
    NSRect  bounds = self.bounds;
    CGFloat top    = [self box_top];
    NSRect  box    = NSMakeRect(0.0, top, NSWidth(bounds), NSHeight(bounds) - top);

    [self.heading drawWithRect:NSMakeRect(0.0, 0.0, NSWidth(bounds), top)
                       options:NSStringDrawingUsesLineFragmentOrigin];

    [tasks_color(self.targeted ? TasksColorLaneTarget : TasksColorLaneFill) setFill];
    NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
    [tasks_color(TasksColorLaneBorder) setStroke];
    [NSBezierPath strokeRect:NSInsetRect(box, 0.5, 0.5)];

    if (self.cards.count == 0) {
        NSAttributedString *prompt = empty_prompt();
        [prompt drawWithRect:NSMakeRect(tasks_look_lane_pad,
                                        top + tasks_look_lane_pad + lane_empty_pad,
                                        NSWidth(bounds) - 2.0 * tasks_look_lane_pad,
                                        ceil(prompt.size.height))
                     options:NSStringDrawingUsesLineFragmentOrigin];
    }
    if (self.shadowed) [self draw_shadows:dirty];
    if (self.mark_slot >= 0) {
        [NSColor.controlAccentColor setFill];
        NSRectFill([self mark_rect]);
    }
}

/*
 * resetCursorRects — the pointing hand over the Done lane's link.
 */
- (void)
resetCursorRects
{
    if (self.link != nil && !self.link.hidden) {
        [self addCursorRect:self.link.frame cursor:NSCursor.pointingHandCursor];
    }
}

/*
 * mouseDown: — a click on the lane itself, not on a card: nothing is
 * selected any more, and the board takes the keyboard.
 *
 * Inputs:
 *   event — the mouse-down event (unused)
 */
- (void)
mouseDown:(NSEvent *)event
{
    [self.window makeFirstResponder:self.kanban];
    [self.kanban select_only:nil];
}

/*
 * slot_at: — the insertion slot under a pointer position (the two-half
 * rule, task_board_slot_at).
 *
 * Inputs:
 *   location — the pointer, in window coordinates
 *
 * Output:
 *   the slot, 0 .. the number of cards.
 */
- (size_t)
slot_at:(NSPoint)location
{
    size_t  count = self.cards.count;
    double *mids  = calloc(count > 0 ? count : 1, sizeof *mids);
    if (mids == NULL) abort();

    for (size_t i = 0; i < count; i++) mids[i] = NSMidY(self.cards[i].frame);
    size_t slot = task_board_slot_at(mids, count, [self convertPoint:location fromView:nil].y);
    free(mids);
    return slot;
}

/*
 * drag_moved: — a card drag entered the lane or moved within it: show where
 * it would land.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   NSDragOperationMove while one of this board's own card drags is in
 *   flight, else NSDragOperationNone.
 */
- (NSDragOperation)
drag_moved:(id<NSDraggingInfo>)info
{
    if (self.kanban.drag_ids == nil) return NSDragOperationNone;

    /* No insertion bar over Done: that lane sorts by completion, so a bar
     * would promise a position the drop ignores. */
    NSInteger slot = self.status == TASK_STATUS_DONE
        ? -1 : (NSInteger)[self slot_at:info.draggingLocation];
    [self show_drop_target:YES mark:slot];
    return NSDragOperationMove;
}

/*
 * draggingEntered: — a drag came over the lane.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   the operation a drop here would perform.
 */
- (NSDragOperation)
draggingEntered:(id<NSDraggingInfo>)info
{
    return [self drag_moved:info];
}

/*
 * draggingUpdated: — the drag moved within the lane.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   the operation a drop here would perform.
 */
- (NSDragOperation)
draggingUpdated:(id<NSDraggingInfo>)info
{
    return [self drag_moved:info];
}

/*
 * draggingExited: — the drag left the lane: it is no longer the target.
 *
 * Inputs:
 *   info — the drag (unused)
 */
- (void)
draggingExited:(id<NSDraggingInfo>)info
{
    [self show_drop_target:NO mark:-1];
}

/*
 * performDragOperation: — the cards were released over the lane.
 *
 * Inputs:
 *   info — the drag
 *
 * Output:
 *   YES when the pane took the drop.
 */
- (BOOL)
performDragOperation:(id<NSDraggingInfo>)info
{
    return [self.kanban drop_on_lane:self.status slot:[self slot_at:info.draggingLocation]];
}

@end

/* ── Canvas ─────────────────────────────────────────────────────────── */

@interface TasksKanbanCanvas : NSView
@end

@implementation TasksKanbanCanvas

/*
 * isFlipped — y grows downwards, so the lanes start at the top of the
 * scroll view and the board grows towards the bottom.
 *
 * Output:
 *   YES.
 */
- (BOOL)
isFlipped
{
    return YES;
}

@end

/* ── Pane ───────────────────────────────────────────────────────────── */

@implementation TasksKanban {
    task_row_list_t *_rows;         /* owned; NULL while the pane is hidden */
    task_board_t    *_board;        /* owned; borrows _rows; NULL with it   */
}

/*
 * init_with_library: — build the pane.
 * Inputs/output are documented in kanban.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library   = library;
    _cards     = @{};
    _selection = [NSMutableSet set];

    self.canvas = [[TasksKanbanCanvas alloc] initWithFrame:NSZeroRect];
    NSMutableArray<TasksKanbanLane *> *lanes = [NSMutableArray array];
    for (int status = 0; status < TASK_STATUS_N_VALUES; status++) {
        TasksKanbanLane *lane = [[TasksKanbanLane alloc] init_with_kanban:self
                                                                   status:(TaskStatus)status];
        [self.canvas addSubview:lane];
        [lanes addObject:lane];
    }
    self.lanes = lanes;

    /* The scroller is not auto-hidden: where scrollers take room (the
     * "always show" setting) one that came and went would change the width
     * the lanes are laid out for. */
    self.scroll = [[NSScrollView alloc] initWithFrame:self.bounds];
    self.scroll.autoresizingMask    = NSViewWidthSizable | NSViewHeightSizable;
    self.scroll.hasVerticalScroller = YES;
    self.scroll.drawsBackground     = NO;
    self.scroll.documentView        = self.canvas;
    [self addSubview:self.scroll];
    return self;
}

/*
 * dealloc — a pane released without shut_down still frees its rows.
 */
- (void)
dealloc
{
    task_board_free(_board);
    task_row_list_free(_rows);
}

/*
 * acceptsFirstResponder — the board takes the keyboard, so that a click on
 * it moves focus out of the search field and the task commands (which are
 * disabled while text is being edited) apply to the selected cards.
 *
 * Output:
 *   YES.
 */
- (BOOL)
acceptsFirstResponder
{
    return YES;
}

/* ── Layout ──────────────────────────────────────────────────────────── */

/*
 * arrange_lanes — give the three lanes equal widths across the visible
 * width and one height: the tallest lane's, and at least the visible
 * height, so that an empty lane is a drop target all the way down.  The
 * canvas takes the resulting size; the board never scrolls sideways.
 */
- (void)
arrange_lanes
{
    NSSize  visible = self.scroll.contentSize;
    CGFloat gap     = tasks_look_lane_gap;
    CGFloat width   = floor((visible.width - gap * (TASK_STATUS_N_VALUES + 1)) /
                            TASK_STATUS_N_VALUES);
    CGFloat height  = visible.height - 2.0 * gap;

    /* Not laid out by the window yet (or squeezed to nothing): wait. */
    if (width <= 2.0 * tasks_look_lane_pad) return;

    for (TasksKanbanLane *lane in self.lanes) {
        height = MAX(height, [lane arrange_for_width:width]);
    }
    for (NSUInteger i = 0; i < self.lanes.count; i++) {
        self.lanes[i].frame = NSMakeRect(gap + i * (width + gap), gap, width, height);
    }
    [self.canvas setFrameSize:NSMakeSize(visible.width, height + 2.0 * gap)];
}

/*
 * layout — the pane was resized: the lanes follow.
 */
- (void)
layout
{
    [super layout];
    [self arrange_lanes];
}

/* ── Showing rows ────────────────────────────────────────────────────── */

/*
 * rebuild — make the board from the rows in hand and show it, reusing every
 * card whose task is still on the board.  Called for each refresh and when
 * the Done lane is expanded or collapsed.
 */
- (void)
rebuild
{
    TasksLibraryWindow *library  = self.library;
    size_t              done_cap = self.done_show_all ? SIZE_MAX : (size_t)tasks_look_done_cap;
    NSMutableDictionary<NSNumber *, TasksKanbanCard *> *kept = [NSMutableDictionary dictionary];

    task_board_free(_board);
    _board = _rows != NULL && library != nil
        ? task_board_build(library.selection, _rows, done_cap) : NULL;

    for (int status = 0; status < TASK_STATUS_N_VALUES; status++) {
        const task_board_lane_t *lane = _board != NULL ? &_board->lanes[status] : NULL;
        NSMutableArray<TasksKanbanCard *> *lane_cards = [NSMutableArray array];

        for (size_t i = 0; lane != NULL && i < lane->count; i++) {
            const task_row_t *row = lane->cards[i];
            NSNumber         *key = @(row->id);
            TasksKanbanCard  *card = self.cards[key]
                ?: [[TasksKanbanCard alloc] init_with_kanban:self task_id:row->id];

            [card show_text:tasks_row_text(row, NO)];
            [card show_selected:[self.selection containsObject:key]];
            [card show_dragged:[self.drag_ids containsObject:key]];
            [lane_cards addObject:card];
            kept[key] = card;
        }
        [self.lanes[(NSUInteger)status] show_cards:lane_cards
                                             total:lane != NULL ? lane->total : 0];
    }

    /* A refresh can be started by a card's own event handler (a command
     * from its context menu), and that card may be one that has just left
     * the board.  Keep the cards that left alive until the handler has
     * returned: the block's only job is to hold them for one more pass. */
    NSMutableArray<TasksKanbanCard *> *gone = [NSMutableArray array];
    for (NSNumber *key in self.cards) {
        if (kept[key] == nil) [gone addObject:self.cards[key]];
    }
    if (gone.count > 0) dispatch_async(dispatch_get_main_queue(), ^{ [gone removeAllObjects]; });
    self.cards = kept;

    /* The selection keeps only the tasks still on the board. */
    [self.selection intersectSet:[NSSet setWithArray:kept.allKeys]];
    if (self.selection.count == 0) self.anchor_id = 0;

    /* The link names what a click will do. */
    size_t    done_total = _board != NULL ? _board->lanes[TASK_STATUS_DONE].total : 0;
    NSString *link_title = nil;
    if (done_total > (size_t)tasks_look_done_cap) {
        link_title = self.done_show_all
            ? @"Show recent only"
            : [NSString stringWithFormat:@"Show all %zu completed", done_total];
    }
    [self.lanes[TASK_STATUS_DONE] show_link:link_title];

    [self arrange_lanes];
}

/*
 * show_rows: — replace what the board shows.
 * The contract is documented in library_window.h.
 */
- (void)
show_rows:(task_row_list_t *)rows
{
    /* The board borrows the rows: it goes first. */
    task_board_free(_board);
    _board = NULL;
    task_row_list_free(_rows);
    _rows = rows;

    if (rows == NULL) {
        [self cancel_drag];
        [self.selection removeAllObjects];
    } else {
        /* The settings the board draws by are read on every refresh, so a
         * change in Settings shows without the two windows knowing of each
         * other. */
        BOOL shadowed = task_config_get_bool(TASK_CFG_KANBAN_SHADOW);
        for (TasksKanbanLane *lane in self.lanes) [lane show_shadows:shadowed];
    }
    [self rebuild];
}

/*
 * selected_task_ids — the selected cards' tasks: lane by lane, top to
 * bottom.
 * Output is documented in library_window.h.
 */
- (NSArray<NSNumber *> *)
selected_task_ids
{
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];

    for (int status = 0; _board != NULL && status < TASK_STATUS_N_VALUES; status++) {
        for (size_t i = 0; i < _board->lanes[status].count; i++) {
            NSNumber *key = @(_board->lanes[status].cards[i]->id);
            if ([self.selection containsObject:key]) [ids addObject:key];
        }
    }
    return ids;
}

/*
 * view_changed — another sidebar row was selected: the selection, the
 * scroll position and the Done lane's expansion belonged to the old view.
 */
- (void)
view_changed
{
    [self.selection removeAllObjects];
    self.anchor_id     = 0;
    self.done_show_all = NO;
    [self.canvas scrollPoint:NSZeroPoint];
}

/*
 * cancel_drag — end a card drag and put the board back as it was: the
 * dragged cards at full strength, no lane tinted, no insertion bar.  THE
 * ONE EXIT of a drag: a drop, a refused drop, Escape and the pane being
 * hidden or shut down all come through here, so no end path can leave a
 * card dimmed or a bar on screen.  Does nothing when no drag is in flight.
 */
- (void)
cancel_drag
{
    if (self.drag_ids == nil) return;

    for (NSNumber *key in self.drag_ids) [self.cards[key] show_dragged:NO];
    self.drag_ids = nil;
    for (TasksKanbanLane *lane in self.lanes) [lane show_drop_target:NO mark:-1];
}

/*
 * shut_down — the window is closing: end any drag, free the rows and empty
 * the board.  Idempotent.
 */
- (void)
shut_down
{
    [self show_rows:NULL];
}

/* ── Selection ───────────────────────────────────────────────────────── */

/*
 * restyle_cards — the selection changed under the user: draw every card as
 * selected or not, in place, and tell the window (the status bar's count).
 * Never a refresh: a refresh would be work for nothing, and the card under
 * the pointer may be about to start a drag.  (A refresh restyles its cards
 * itself, in rebuild, and says nothing.)
 */
- (void)
restyle_cards
{
    for (NSNumber *key in self.cards) {
        [self.cards[key] show_selected:[self.selection containsObject:key]];
    }
    [self.library selection_changed];
}

/*
 * select_only: — collapse the selection to one card, which becomes the
 * Shift-click anchor.
 *
 * Inputs:
 *   task_id — the card's task, or nil to select nothing
 */
- (void)
select_only:(NSNumber *)task_id
{
    [self.selection removeAllObjects];
    if (task_id != nil) [self.selection addObject:task_id];
    self.anchor_id = task_id.longLongValue;
    [self restyle_cards];
}

/*
 * extend_selection_to: — a Shift-click: select the run of cards from the
 * anchor to the clicked one.  A run exists WITHIN ONE LANE only; with the
 * anchor in another lane (or no anchor) the click just adds its card.  The
 * anchor does not move.
 *
 * Inputs:
 *   task_id — the clicked card's task
 */
- (void)
extend_selection_to:(int64_t)task_id
{
    TaskStatus lane, anchor_lane;
    size_t     index, anchor_index;

    [self.selection addObject:@(task_id)];
    if (!task_board_find(_board, task_id, &lane, &index) ||
        !task_board_find(_board, self.anchor_id, &anchor_lane, &anchor_index) ||
        lane != anchor_lane) {
        return;
    }
    for (size_t i = MIN(index, anchor_index); i <= MAX(index, anchor_index); i++) {
        [self.selection addObject:@(_board->lanes[lane].cards[i]->id)];
    }
}

/*
 * card_pressed:event: — a card was pressed.  A double click opens the
 * editor.  Otherwise the selection changes ON THE PRESS, as it does in a
 * list: Shift extends, Command toggles, a plain press outside the selection
 * collapses to the card.  A plain press INSIDE a multi-selection keeps it —
 * that is what lets a drag of several cards start from any of them — and
 * the collapse waits for the release (card_released:event:).
 *
 * Inputs:
 *   card  — the card
 *   event — the mouse-down event
 */
- (void)
card_pressed:(TasksKanbanCard *)card event:(NSEvent *)event
{
    TasksLibraryWindow *library = self.library;
    NSNumber           *key     = @(card.task_id);

    if (library == nil || _board == NULL) return;
    [self.window makeFirstResponder:self];

    if (event.clickCount == 2) {
        tasks_editor_open(library.app, card.task_id);
    } else if (event.modifierFlags & NSEventModifierFlagShift) {
        [self extend_selection_to:card.task_id];
    } else if (event.modifierFlags & NSEventModifierFlagCommand) {
        if ([self.selection containsObject:key]) {
            [self.selection removeObject:key];
        } else {
            [self.selection addObject:key];
        }
        self.anchor_id = card.task_id;
    } else if (![self.selection containsObject:key]) {
        [self select_only:key];
    } else {
        self.anchor_id = card.task_id;
    }
    [self restyle_cards];
}

/*
 * card_released:event: — a press ended without becoming a drag.  If it was
 * a plain click on one card of a multi-selection, the selection collapses
 * to that card now.
 *
 * Inputs:
 *   card  — the card
 *   event — the mouse-up event
 */
- (void)
card_released:(TasksKanbanCard *)card event:(NSEvent *)event
{
    NSEventModifierFlags held = event.modifierFlags &
                                (NSEventModifierFlagShift | NSEventModifierFlagCommand);

    if (held == 0 && self.selection.count > 1) [self select_only:@(card.task_id)];
}

/*
 * menu_for_card: — the context menu for a card.  AppKit does not change a
 * selection on a right-click, so it is done here: a card inside the
 * selection keeps it, a card outside collapses it to that card.  The menu
 * itself is the window's, shared with the list.
 *
 * Inputs:
 *   card — the card that was right-clicked
 *
 * Output:
 *   the menu, or nil when the window has gone.
 */
- (NSMenu *)
menu_for_card:(TasksKanbanCard *)card
{
    NSNumber *key = @(card.task_id);

    [self.window makeFirstResponder:self];
    if (![self.selection containsObject:key]) [self select_only:key];
    return [self.library task_context_menu];
}

/*
 * selectAll: — Edit > Select All: every card on the board.
 *
 * Inputs:
 *   sender — the menu item (unused)
 */
- (void)
selectAll:(id)sender
{
    [self.selection addObjectsFromArray:self.cards.allKeys];
    [self restyle_cards];
}

/*
 * done_link_clicked: — the Done lane's link: flip between the most recent
 * few and every completed task.  The rows in hand already hold them all,
 * so the board is rebuilt without asking the window for a refresh.
 *
 * Inputs:
 *   sender — the link (unused)
 */
- (void)
done_link_clicked:(id)sender
{
    self.done_show_all = !self.done_show_all;
    [self rebuild];
}

/* ── Drag and drop ───────────────────────────────────────────────────── */

/*
 * begin_drag_from:event: — start dragging.  What moves is the selection
 * when the gripped card is part of it, else that card alone (a
 * Command-click on the grip may just have deselected it).  The ghost is the
 * gripped card, with a count badge when it stands for several.
 *
 * Inputs:
 *   card  — the card whose grip was dragged
 *   event — the mouse-dragged event that crossed the threshold
 */
- (void)
begin_drag_from:(TasksKanbanCard *)card event:(NSEvent *)event
{
    NSNumber            *key    = @(card.task_id);
    NSArray<NSNumber *> *moving = [self selected_task_ids];

    if (![moving containsObject:key]) moving = @[key];

    /* Within the board drag_ids is the truth; the ids on the pasteboard,
     * comma separated, are what the day planner reads when a card is
     * dropped on it (tasks_card_drag_type, kanban.h). */
    NSPasteboardItem *content = [[NSPasteboardItem alloc] init];
    [content setString:[moving componentsJoinedByString:@","] forType:tasks_card_drag_type];
    NSDraggingItem *item = [[NSDraggingItem alloc] initWithPasteboardWriter:content];
    [item setDraggingFrame:card.bounds contents:[card ghost_with_count:moving.count]];

    self.drag_ids = moving;
    for (NSNumber *moving_key in moving) [self.cards[moving_key] show_dragged:YES];
    [card beginDraggingSessionWithItems:@[item] event:event source:self];
}

/*
 * draggingSession:sourceOperationMaskForDraggingContext: — a card can only
 * be moved, and only within this app (in practice: onto this board's
 * lanes, the only views that accept the type).
 *
 * Inputs:
 *   session — the drag (unused)
 *   context — where the pointer is: inside or outside the app
 *
 * Output:
 *   the operations allowed there.
 */
- (NSDragOperation)
draggingSession:(NSDraggingSession *)session
sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    return context == NSDraggingContextWithinApplication ? NSDragOperationMove
                                                         : NSDragOperationNone;
}

/*
 * ignoreModifierKeysForDraggingSession: — a modifier held during the drag
 * must not turn the move into a copy or a link, which this drag does not
 * offer: the Command or Shift that built the selection is often still down.
 *
 * Inputs:
 *   session — the drag (unused)
 *
 * Output:
 *   YES.
 */
- (BOOL)
ignoreModifierKeysForDraggingSession:(NSDraggingSession *)session
{
    return YES;
}

/*
 * draggingSession:endedAtPoint:operation: — the drag is over, however it
 * ended (dropped, refused, Escape).  A drop has already been applied by
 * drop_on_lane:slot:, which cleans up itself; this covers every other end.
 *
 * Inputs:
 *   session   — the drag (unused)
 *   point     — where it ended (unused)
 *   operation — what the destination did (unused)
 */
- (void)
draggingSession:(NSDraggingSession *)session
   endedAtPoint:(NSPoint)point
      operation:(NSDragOperation)operation
{
    [NSCursor pop];
    [self cancel_drag];
}

/*
 * drop_on_lane:slot: — the dragged cards were released over a lane.  The
 * drag is ended first; then the core carries out the drop's two halves
 * (task_board_drop: the status of each card, and the saved order).  When
 * anything changed the selection moves to the dropped cards and the window
 * is refreshed — on the next run-loop pass, because this runs inside the
 * drag's own handler.
 *
 * Inputs:
 *   lane — the lane dropped on
 *   slot — the insertion slot in that lane, counted against the cards on
 *          screen
 *
 * Output:
 *   YES when a drag of this board's cards was in flight, so the drop was
 *   taken (even if it changed nothing); NO otherwise.
 */
- (BOOL)
drop_on_lane:(TaskStatus)lane slot:(size_t)slot
{
    TasksLibraryWindow  *library = self.library;
    NSArray<NSNumber *> *moving  = self.drag_ids;

    [self cancel_drag];
    if (moving == nil || library == nil || _board == NULL) return NO;

    int64_t *ids = calloc(moving.count, sizeof *ids);
    if (ids == NULL) abort();
    for (NSUInteger i = 0; i < moving.count; i++) ids[i] = moving[i].longLongValue;

    /* An order is never saved from a board a search has filtered: the cards
     * on screen are only the matches (task_order.h).  The status half of
     * the drop still runs. */
    bool changed = task_board_drop(library.app, library.selection, _board, ids, moving.count,
                                   lane, slot, library.search == NULL);
    free(ids);
    if (!changed) return YES;

    [self.selection removeAllObjects];
    [self.selection addObjectsFromArray:moving];
    self.anchor_id = moving[0].longLongValue;
    [self restyle_cards];

    __weak TasksKanban *weak_self = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        TasksKanban *kanban = weak_self;
        if (!kanban || kanban->_rows == NULL) return;   /* gone, or hidden meanwhile */
        [kanban.library full_refresh];
    });
    return YES;
}

@end
