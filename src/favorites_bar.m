#import "favorites_bar.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

/* ── Constants ───────────────────────────────────────────────────────── */

static const CGFloat bar_pad      = 6.0;   /* leading padding of button strip */
static const CGFloat btn_text_pad = 16.0;  /* horizontal text padding in button */

/* What the bar says while no tag is a favorite. */
static NSString * const empty_hint =
    @"No favorites yet — right-click a tag in the sidebar and choose Edit Tag…";

/* ── The classes, declared ───────────────────────────────────────────── */

/*
 * TasksFavoriteBtn — NSButton carrying the tag it stands for.  The "on"
 * state is painted explicitly via a layer background because most
 * NSBezelStyle variants only show pressed state while the mouse is held,
 * not persistently.
 */
@interface TasksFavoriteBtn : NSButton
@property (nonatomic, assign) int64_t tag_id;
@end

@interface TasksFavoritesBar ()
@property (nonatomic, weak)   TasksLibraryWindow *library;
@property (nonatomic, strong) NSScrollView       *scroll;
@property (nonatomic, strong) NSView             *strip;
@property (nonatomic, strong) NSTextField        *hint;
@property (nonatomic, assign) BOOL                is_shut_down;
@property (nonatomic, assign) CGFloat             content_w;  /* buttons + padding */
@end

/* ── Button subclass ─────────────────────────────────────────────────── */

@implementation TasksFavoriteBtn

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
 * menuForEvent: — a right-click on a favorite: Edit Tag… for its tag.
 *
 * Inputs:
 *   event — the right mouse-down (unused)
 *
 * Output:
 *   the menu; empty once the window has gone.
 */
- (NSMenu *)
menuForEvent:(NSEvent *)event
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    NSView *view = self.superview;

    while (view != nil && ![view isKindOfClass:TasksFavoritesBar.class]) view = view.superview;
    TasksLibraryWindow *library = ((TasksFavoritesBar *)view).library;
    if (library != nil) {
        [menu addItem:tasks_command_menu_item(TasksCommandEditTag, @(self.tag_id), library)];
    }
    return menu;
}

@end

/* ── Horizontal-scroll view ──────────────────────────────────────────── */

/*
 * TasksFavoritesScroll — an NSScrollView that redirects a vertical wheel
 * gesture to horizontal scrolling, so a normal two-finger swipe pans the
 * button strip without a visible scrollbar.
 */
@interface TasksFavoritesScroll : NSScrollView
@end

@implementation TasksFavoritesScroll

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

@implementation TasksFavoritesBar

/*
 * init_with_library: — build the bar.
 * Inputs/output are documented in favorites_bar.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library = library;

    self.scroll = [[TasksFavoritesScroll alloc] initWithFrame:NSZeroRect];
    self.scroll.hasHorizontalScroller = NO;
    self.scroll.hasVerticalScroller   = NO;
    self.scroll.drawsBackground       = NO;
    self.scroll.translatesAutoresizingMaskIntoConstraints = NO;

    self.strip               = [[NSView alloc] initWithFrame:NSZeroRect];
    self.scroll.documentView = self.strip;

    self.hint           = [NSTextField labelWithString:empty_hint];
    self.hint.font      = [NSFont systemFontOfSize:shared_look_ui_font];
    self.hint.textColor = NSColor.secondaryLabelColor;
    self.hint.translatesAutoresizingMaskIntoConstraints = NO;

    /* The bar spans the window: a rule along its bottom parts it from the
     * sidebar and the tasks below.  Wider than tall, so it is horizontal. */
    NSBox *rule  = [[NSBox alloc] initWithFrame:NSMakeRect(0, 0, 100, 1)];
    rule.boxType = NSBoxSeparator;
    rule.translatesAutoresizingMaskIntoConstraints = NO;

    [self addSubview:self.scroll];
    [self addSubview:self.hint];
    [self addSubview:rule];
    [NSLayoutConstraint activateConstraints:@[
        [self.scroll.topAnchor constraintEqualToAnchor:self.topAnchor],
        [self.scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [self.scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [self.scroll.bottomAnchor constraintEqualToAnchor:rule.topAnchor],
        [rule.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [rule.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [rule.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [rule.heightAnchor constraintEqualToConstant:1.0],
        [self.hint.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:bar_pad * 2],
        [self.hint.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

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

    CGFloat sw    = NSWidth(self.scroll.bounds);
    CGFloat new_w = MAX(self.content_w, sw);
    NSRect  sf    = self.strip.frame;

    if (fabs(sf.size.width - new_w) > 0.5) {
        sf.size.width    = new_w;
        self.strip.frame = sf;

        NSPoint origin = self.scroll.contentView.bounds.origin;
        CGFloat max_x  = MAX(0.0, new_w - sw);
        if (origin.x > max_x) {
            origin.x = max_x;
            [self.scroll.contentView scrollToPoint:origin];
            [self.scroll reflectScrolledClipView:self.scroll.contentView];
        }
    }
}

/*
 * buttons — the strip's buttons, left to right.
 *
 * Output:
 *   the buttons.
 */
- (NSArray<TasksFavoriteBtn *> *)
buttons
{
    NSMutableArray<TasksFavoriteBtn *> *found = [NSMutableArray array];
    for (NSView *view in self.strip.subviews) {
        if ([view isKindOfClass:TasksFavoriteBtn.class]) [found addObject:(TasksFavoriteBtn *)view];
    }
    [found sortUsingComparator:^NSComparisonResult(TasksFavoriteBtn *a, TasksFavoriteBtn *b) {
        return [@(NSMinX(a.frame)) compare:@(NSMinX(b.frame))];
    }];
    return found;
}

/*
 * update_selection — light the selected tag's button.
 * Documented in favorites_bar.h.
 */
- (void)
update_selection
{
    if (!self.library || self.is_shut_down) return;

    int64_t selected = task_sel_tag_id(self.library.selection);
    for (TasksFavoriteBtn *btn in [self buttons]) {
        btn.state = btn.tag_id == selected ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

/*
 * reload — rebuild the buttons from the favorite tags.
 * Documented in favorites_bar.h.
 */
- (void)
reload
{
    if (!self.library || self.is_shut_down) return;

    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];

    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont systemFontOfSize:shared_look_ui_font] };
    CGFloat       x     = bar_pad;
    CGFloat       cy    = shared_look_favorites_bar_h / 2.0;
    GPtrArray    *tags  = task_db_tags(self.library.app->db, TASK_TAGS_CARRIED);

    for (guint i = 0; i < tags->len; i++) {
        const TaskTag *tag = g_ptr_array_index(tags, i);
        if (!tag->favorite) continue;

        NSString         *title = tasks_tag_title(tag);
        CGFloat           w     = MAX(shared_look_favorite_btn_min_w,
                                      ceil([title sizeWithAttributes:attrs].width) + btn_text_pad);
        TasksFavoriteBtn *btn   = [[TasksFavoriteBtn alloc] initWithFrame:
                                      NSMakeRect(x, cy - shared_look_favorite_btn_h / 2.0, w, shared_look_favorite_btn_h)];
        btn.title         = title;
        btn.bezelStyle    = NSBezelStyleAccessoryBar;
        btn.buttonType    = NSButtonTypeToggle;
        btn.imagePosition = NSNoImage;
        btn.font          = [NSFont systemFontOfSize:shared_look_ui_font];
        btn.target        = self;
        btn.action        = @selector(btn_clicked:);
        btn.tag_id        = tag->id;
        [self.strip addSubview:btn];
        x += w + shared_look_favorite_gap;
    }
    task_ptr_array_free_tags(tags);

    self.hint.hidden = self.strip.subviews.count > 0;
    self.content_w   = x + bar_pad;
    self.strip.frame = NSMakeRect(0, 0, MAX(self.content_w, NSWidth(self.scroll.bounds)),
                                  shared_look_favorites_bar_h);
    [self update_selection];
}

/*
 * btn_clicked: — a favorite was clicked: select its tag.
 *
 * Inputs:
 *   sender — the button
 */
- (void)
btn_clicked:(TasksFavoriteBtn *)sender
{
    if (self.is_shut_down) return;
    [self.library select:(task_sel_t){ task_sel_tag, sender.tag_id }];
    [self update_selection];
}

/*
 * shut_down — stop responding.
 * Documented in favorites_bar.h.
 */
- (void)
shut_down
{
    self.is_shut_down = YES;
    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];
}

@end
