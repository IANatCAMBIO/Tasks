/*
 * tool_panel — the app's own dialog: a small floating utility window, in
 * the style of the Files and Notes apps.  Wherever a system alert or an
 * alert sheet would appear, Tasks shows one of these instead
 * (tasks_ui_notice and tasks_ui_confirm in ui_util.h are built on it).
 *
 * A tool panel is a utility-titlebar panel with a solid blue hero strip
 * (symbol and bold title) at the top, the message below it, and a button
 * bar at the bottom.  It is not modal: it floats above ordinary windows
 * until the user clicks a button, presses Return (the default button), or
 * presses Escape or the close box, which dismiss it without an answer —
 * so Escape never answers a destructive question.
 *
 * Also here: the hero strip itself, which the reminders window shares.
 *
 * Main thread only.
 */

#import <Cocoa/Cocoa.h>

/*
 * TasksHero — the solid blue strip at the top of a tool panel and of the
 * reminders window: a white symbol, a bold title and a dimmer subtitle
 * under it.  It is tasks_look_hero_h tall (it carries that constraint); the
 * caller pins its top and sides.
 */
@interface TasksHero : NSView

@property (nonatomic, strong, readonly) NSTextField *title_label;
@property (nonatomic, strong, readonly) NSTextField *subtitle_label;  /* hidden while empty */

/*
 * init_with_symbol: — build the strip with empty labels.
 *
 * Inputs:
 *   symbol — SF Symbols name for the icon; nil or unknown shows none
 *
 * Output:
 *   the strip.
 */
- (instancetype)init_with_symbol:(NSString *)symbol;

/*
 * set_title:subtitle: — what the strip says.  The title sits on the
 * strip's centre line when there is no subtitle, above it when there is.
 *
 * Inputs:
 *   title    — the bold line
 *   subtitle — the second line; nil or empty for none
 */
- (void)set_title:(NSString *)title subtitle:(NSString *)subtitle;

@end

/*
 * tasks_tool_panel_show — show a message in a tool panel and return at once.
 *
 * Inputs:
 *   parent      — window to centre over; nil centres it on the screen
 *   title       — the headline, in the hero strip
 *   message     — wrapped, selectable body text (long text scrolls); may be
 *                 nil
 *   symbol      — SF Symbols name for the hero icon; may be nil
 *   buttons     — button labels, at least one: [0] is the default (Return)
 *                 and sits rightmost, the rest follow to its left.  Return
 *                 must never destroy anything, so a destructive choice is
 *                 never [0] (tasks_ui_confirm puts Cancel there)
 *   destructive — index of the button that takes the destructive look;
 *                 -1 for none
 *   done        — called once, with the index of the clicked button, or -1
 *                 when the panel was dismissed (Escape, the close box)
 *                 without one; may be nil
 */
void tasks_tool_panel_show(NSWindow *parent, NSString *title, NSString *message,
                           NSString *symbol, NSArray<NSString *> *buttons, NSInteger destructive,
                           void (^done)(NSInteger index));
