#import "favorites_bar.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

#include "shared_order.h"
#include "task_config.h"

/* What the bar says while no tag is a favorite. */
static NSString * const empty_hint =
    @"No favorites yet — right-click a tag in the sidebar and choose Add to Favorites";

@interface TasksFavoritesBar ()
@property (nonatomic, weak)   TasksLibraryWindow *library;
@property (nonatomic, strong) NSScrollView       *scroll;
@property (nonatomic, strong) NSView             *strip;
@property (nonatomic, strong) NSTextField        *hint;
@property (nonatomic, assign) BOOL                is_shut_down;
@property (nonatomic, assign) CGFloat             content_w;  /* buttons + padding */
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
        [self.hint.leadingAnchor constraintEqualToAnchor:self.leadingAnchor
                                                constant:shared_look_favorites_pad * 2],
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
 * update_selection — light the selected tag's button.
 * Documented in favorites_bar.h.
 */
- (void)
update_selection
{
    if (!self.library || self.is_shut_down) return;

    int64_t selected = task_sel_tag_id(self.library.selection);
    for (SharedFavoriteButton *btn in self.strip.subviews) {
        btn.state = btn.tag_id == selected ? NSControlStateValueOn : NSControlStateValueOff;
    }
}

/*
 * reload — rebuild the buttons from the favorite tags, in the saved order
 * (TASK_CFG_FAVORITES_ORDER), new favorites at the end by name.
 * Documented in favorites_bar.h.
 */
- (void)
reload
{
    if (!self.library || self.is_shut_down) return;

    for (NSView *v in self.strip.subviews.copy) [v removeFromSuperview];

    __weak TasksFavoritesBar *ws = self;
    NSMutableArray<SharedFavoriteButton *> *buttons = [NSMutableArray array];
    GPtrArray *tags = task_db_tags(self.library.app->db, TASK_TAGS_CARRIED);
    for (guint i = 0; i < tags->len; i++) {
        const TaskTag *tag = g_ptr_array_index(tags, i);
        if (!tag->favorite) continue;
        SharedFavoriteButton *btn = [[SharedFavoriteButton alloc] init_with_title:tasks_tag_title(tag)
                                                                           tag_id:tag->id];
        btn.target       = self;
        btn.action       = @selector(btn_clicked:);
        btn.reordered    = ^(NSArray<SharedFavoriteButton *> *order) { [ws save_order:order]; };
        btn.context_menu = ^NSMenu *(SharedFavoriteButton *button) { return [ws menu_for:button]; };
        [buttons addObject:btn];
    }
    task_ptr_array_free_tags(tags);

    /* By name from the database; the saved order first, the rest after. */
    int64_t *ids = g_new(int64_t, buttons.count + 1);
    for (NSUInteger i = 0; i < buttons.count; i++) ids[i] = buttons[i].tag_id;
    gchar *saved = task_config_get(TASK_CFG_FAVORITES_ORDER);
    int   *order = shared_order_permutation(ids, (int)buttons.count, saved);
    if (order != NULL) {
        NSArray<SharedFavoriteButton *> *by_name = buttons.copy;
        for (NSUInteger i = 0; i < by_name.count; i++) buttons[i] = by_name[(NSUInteger)order[i]];
    }
    free(order);
    g_free(saved);
    g_free(ids);

    for (SharedFavoriteButton *btn in buttons) [self.strip addSubview:btn];
    self.content_w   = shared_ui_place_favorites(buttons, NO);
    self.hint.hidden = buttons.count > 0;
    self.strip.frame = NSMakeRect(0, 0, MAX(self.content_w, NSWidth(self.scroll.bounds)),
                                  shared_look_favorites_bar_h);
    [self update_selection];
}

/*
 * save_order: — a button was dragged into place: remember the row's
 * order (TASK_CFG_FAVORITES_ORDER).
 *
 * Inputs:
 *   order — the buttons, left to right
 */
- (void)
save_order:(NSArray<SharedFavoriteButton *> *)order
{
    if (self.is_shut_down) return;
    int64_t *ids = g_new(int64_t, order.count + 1);
    for (NSUInteger i = 0; i < order.count; i++) ids[i] = order[i].tag_id;
    char *text = shared_order_text(ids, (int)order.count);
    task_config_set(TASK_CFG_FAVORITES_ORDER, text);
    free(text);
    g_free(ids);
}

/*
 * menu_for: — a right-click on a favorite: Edit Tag… and Remove from
 * Favorites, for its tag.
 *
 * Inputs:
 *   button — the button
 *
 * Output: the menu; empty once the window has gone.
 */
- (NSMenu *)
menu_for:(SharedFavoriteButton *)button
{
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    if (self.library != nil && !self.is_shut_down) {
        [menu addItem:shared_command_menu_item(@"edit-tag", @(button.tag_id))];
        [menu addItem:shared_command_menu_item(@"remove-favorite", @(button.tag_id))];
    }
    return menu;
}

/*
 * btn_clicked: — a favorite was clicked: select its tag.
 *
 * Inputs:
 *   sender — the button
 */
- (void)
btn_clicked:(SharedFavoriteButton *)sender
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
