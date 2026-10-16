/*
 * tool_panel.m — see tool_panel.h.  Ported from the Notes app's
 * tool_panel.m (itself from Files), cut down to what Tasks asks of it: a
 * message and its buttons.
 */
#import "tool_panel.h"
#import "look.h"
#import "ui_util.h"

/* ── The hero strip ─────────────────────────────────────────────────── */

@interface TasksHero ()
@property (nonatomic, strong, readwrite) NSTextField *title_label;
@property (nonatomic, strong, readwrite) NSTextField *subtitle_label;
@end

/*
 * hero_label — one line of white-on-blue hero text that truncates rather
 * than widen the strip.
 *
 * Inputs:
 *   font  — its font
 *   color — its colour
 *
 * Output:
 *   new, empty label.
 */
static NSTextField *
hero_label(NSFont *font, NSColor *color)
{
    NSTextField *label = [NSTextField labelWithString:@""];

    label.font                 = font;
    label.textColor            = color;
    label.lineBreakMode        = NSLineBreakByTruncatingTail;
    label.maximumNumberOfLines = 1;
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

@implementation TasksHero

/*
 * init_with_symbol: — the strip: the symbol, then the title over the
 * subtitle, centred on the strip's height.
 * Inputs/output are documented in tool_panel.h.
 */
- (instancetype)
init_with_symbol:(NSString *)symbol
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    self.translatesAutoresizingMaskIntoConstraints = NO;

    NSImage *sym = symbol ? [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil]
                          : nil;
    sym = [sym imageWithSymbolConfiguration:
           [NSImageSymbolConfiguration configurationWithPointSize:tasks_look_hero_icon
                                                           weight:NSFontWeightMedium]];
    NSImageView *icon     = tasks_image_view(sym, tasks_look_hero_icon);
    icon.contentTintColor = NSColor.whiteColor;
    icon.hidden           = sym == nil;

    _title_label    = hero_label([NSFont boldSystemFontOfSize:tasks_look_ui_font],
                                 NSColor.whiteColor);
    _subtitle_label = hero_label([NSFont systemFontOfSize:tasks_look_ui_font],
                                 tasks_color(TasksColorHeroSubtitle));
    _subtitle_label.hidden = YES;

    /* A hidden subtitle leaves the stack, so a lone title is centred. */
    NSStackView *text = [NSStackView stackViewWithViews:@[_title_label, _subtitle_label]];
    text.orientation  = NSUserInterfaceLayoutOrientationVertical;
    text.alignment    = NSLayoutAttributeLeading;
    text.spacing      = tasks_look_hero_gap;

    NSStackView *row = [NSStackView stackViewWithViews:@[icon, text]];
    row.orientation  = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment    = NSLayoutAttributeCenterY;
    row.spacing      = tasks_look_hero_icon_gap;
    row.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:row];

    [NSLayoutConstraint activateConstraints:@[
        [self.heightAnchor constraintEqualToConstant:tasks_look_hero_h],
        [row.leadingAnchor constraintEqualToAnchor:self.leadingAnchor
                                          constant:tasks_look_hero_pad],
        [row.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor
                                                     constant:-tasks_look_hero_pad],
        [row.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

/*
 * set_title:subtitle: — what the strip says.
 * Inputs are documented in tool_panel.h.
 */
- (void)
set_title:(NSString *)title subtitle:(NSString *)subtitle
{
    self.title_label.stringValue    = title ?: @"";
    self.subtitle_label.stringValue = subtitle ?: @"";
    self.subtitle_label.hidden      = subtitle.length == 0;
}

/*
 * isOpaque — the strip fills its whole frame.
 *
 * Output:
 *   YES.
 */
- (BOOL)
isOpaque
{
    return YES;
}

/*
 * drawRect: — fill the strip with the hero blue.
 *
 * Inputs:
 *   dirtyRect — the area to redraw (unused: the whole strip is filled)
 */
- (void)
drawRect:(NSRect)dirtyRect
{
    [tasks_color(TasksColorHero) setFill];
    NSRectFill(self.bounds);
}

@end

/* ── The panel ──────────────────────────────────────────────────────── */

/*
 * TasksToolWindow — the panel's window: Escape closes it, which reports a
 * dismissal (index -1) to its TasksToolPanel.
 */
@interface TasksToolWindow : NSPanel
@end

@implementation TasksToolWindow

/*
 * cancelOperation: — Escape, from the panel or a field in it: close.
 *
 * Inputs:
 *   sender — the responder that sent it (unused)
 */
- (void)
cancelOperation:(id)sender
{
    [self close];
}

@end

/*
 * TasksToolPanel — one panel on screen: its window and its completion.
 * It keeps itself alive (live_panels) until its window closes.
 */
@interface TasksToolPanel : NSObject <NSWindowDelegate>
@property (nonatomic, strong) TasksToolWindow *window;
@property (nonatomic, copy)   void (^done)(NSInteger index);
@property (nonatomic, assign) NSInteger chosen;     /* the button clicked; -1 none */
@end

/* Panels on screen. */
static NSMutableSet<TasksToolPanel *> *live_panels;

/*
 * message_view — the wrapped, selectable message in a fixed-size view;
 * text taller than tasks_look_tool_msg_max_h scrolls.
 *
 * Inputs:
 *   message — the text
 *
 * Output:
 *   a view tasks_look_tool_msg_w wide.
 */
static NSView *
message_view(NSString *message)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:message];
    label.selectable   = YES;
    label.preferredMaxLayoutWidth = tasks_look_tool_msg_w;
    CGFloat h = ceil([label.cell cellSizeForBounds:
                      NSMakeRect(0, 0, tasks_look_tool_msg_w, CGFLOAT_MAX)].height);
    CGFloat shown_h = MIN(h, tasks_look_tool_msg_max_h);

    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, tasks_look_tool_msg_w, shown_h)];
    if (h <= tasks_look_tool_msg_max_h) {
        label.frame = box.bounds;
        [box addSubview:label];
    } else {
        label.frame = NSMakeRect(0, 0, tasks_look_tool_msg_w, h);
        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:box.bounds];
        scroll.hasVerticalScroller = YES;
        scroll.drawsBackground     = NO;
        scroll.documentView        = label;
        [box addSubview:scroll];
    }
    [NSLayoutConstraint activateConstraints:@[
        [box.widthAnchor  constraintEqualToConstant:tasks_look_tool_msg_w],
        [box.heightAnchor constraintEqualToConstant:shown_h],
    ]];
    return box;
}

/*
 * button_w — the width a tool-panel button is given: its title's, padded,
 * never narrower than tasks_look_tool_btn_w.
 *
 * Inputs:
 *   button — the button
 *
 * Output:
 *   the width in points.
 */
static CGFloat
button_w(NSButton *button)
{
    return MAX(tasks_look_tool_btn_w, button.fittingSize.width + tasks_look_tool_btn_pad);
}

@implementation TasksToolPanel

/*
 * button_clicked: — a button answers the panel: remember which, close.
 *
 * Inputs:
 *   sender — the button; its tag is its index
 */
- (void)
button_clicked:(NSButton *)sender
{
    self.chosen = sender.tag;
    [self.window close];
}

/*
 * windowWillClose: — report the answer (or -1 for a dismissal), then let
 * go of the completion, which may hold the caller's objects, and of the
 * panel's own retention.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    void (^done)(NSInteger) = self.done;
    self.done = nil;
    [live_panels removeObject:self];
    if (done) done(self.chosen);
}

@end

/*
 * tasks_tool_panel_show — lay the panel out (hero, message, buttons), centre
 * it and bring it forward.
 * Inputs are documented in tool_panel.h.
 */
void
tasks_tool_panel_show(NSWindow *parent, NSString *title, NSString *message,
                      NSString *symbol, NSArray<NSString *> *buttons, NSInteger destructive,
                      void (^done)(NSInteger index))
{
    TasksToolPanel *p = [[TasksToolPanel alloc] init];
    p.done   = done;
    p.chosen = -1;

    NSMutableArray<NSButton *> *controls = [NSMutableArray array];
    CGFloat bar_w = 2 * tasks_look_tool_btn_margin - tasks_look_tool_btn_gap;
    for (NSUInteger i = 0; i < buttons.count; i++) {
        NSButton *b = [NSButton buttonWithTitle:buttons[i] target:p
                                         action:@selector(button_clicked:)];
        b.tag                  = (NSInteger)i;
        b.hasDestructiveAction = (NSInteger)i == destructive;
        b.translatesAutoresizingMaskIntoConstraints = NO;
        [controls addObject:b];
        bar_w += tasks_look_tool_btn_gap + button_w(b);
    }
    controls.firstObject.keyEquivalent = @"\r";

    NSView *content = message_view(message ?: @"");
    NSSize  cs      = content.fittingSize;
    CGFloat w = MAX(MAX(tasks_look_tool_min_w, cs.width + 2 * tasks_look_tool_pad), bar_w);
    CGFloat h = tasks_look_hero_h + tasks_look_tool_pad + cs.height + tasks_look_tool_pad +
                1.0 + tasks_look_tool_bar_h;

    TasksToolWindow *window = [[TasksToolWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, w, h)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.title                  = @TASKS_APP_NAME;
    window.floatingPanel          = YES;
    window.hidesOnDeactivate      = NO;
    window.becomesKeyOnlyIfNeeded = NO;
    window.releasedWhenClosed     = NO;
    window.delegate               = p;
    p.window                      = window;
    NSView *cv = window.contentView;

    TasksHero *hero = [[TasksHero alloc] init_with_symbol:symbol];
    [hero set_title:title subtitle:nil];
    [cv addSubview:hero];

    NSBox *rule = [[NSBox alloc] init];
    rule.boxType = NSBoxSeparator;
    rule.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:rule];

    content.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:content];

    [NSLayoutConstraint activateConstraints:@[
        [hero.topAnchor        constraintEqualToAnchor:cv.topAnchor],
        [hero.leadingAnchor    constraintEqualToAnchor:cv.leadingAnchor],
        [hero.trailingAnchor   constraintEqualToAnchor:cv.trailingAnchor],
        [content.topAnchor     constraintEqualToAnchor:hero.bottomAnchor
                                              constant:tasks_look_tool_pad],
        [content.centerXAnchor constraintEqualToAnchor:cv.centerXAnchor],
        [rule.leadingAnchor    constraintEqualToAnchor:cv.leadingAnchor],
        [rule.trailingAnchor   constraintEqualToAnchor:cv.trailingAnchor],
        [rule.bottomAnchor     constraintEqualToAnchor:cv.bottomAnchor
                                              constant:-tasks_look_tool_bar_h],
        [rule.heightAnchor     constraintEqualToConstant:1.0],
    ]];

    /* The buttons, right-aligned: [0] rightmost, the rest to its left. */
    NSButton *right = nil;
    for (NSButton *b in controls) {
        [cv addSubview:b];
        [NSLayoutConstraint activateConstraints:@[
            [b.trailingAnchor constraintEqualToAnchor:right ? right.leadingAnchor : cv.trailingAnchor
                                             constant:right ? -tasks_look_tool_btn_gap
                                                            : -tasks_look_tool_btn_margin],
            [b.bottomAnchor   constraintEqualToAnchor:cv.bottomAnchor
                                             constant:-(tasks_look_tool_bar_h -
                                                        tasks_look_tool_btn_h) / 2.0],
            [b.widthAnchor    constraintEqualToConstant:button_w(b)],
            [b.heightAnchor   constraintEqualToConstant:tasks_look_tool_btn_h],
        ]];
        right = b;
    }

    if (!live_panels) live_panels = [NSMutableSet set];
    [live_panels addObject:p];
    if (parent) {
        NSRect pf = parent.frame;
        [window setFrameOrigin:NSMakePoint(NSMidX(pf) - w / 2.0, NSMidY(pf) - NSHeight(window.frame) / 2.0)];
    } else {
        [window center];
    }
    [NSApp activate];
    [window makeKeyAndOrderFront:nil];
}
