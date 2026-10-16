/*
 * tool_panel — the app's own dialog: a small floating utility window, in
 * the style of the Files and Notes apps.  Wherever a system alert, an
 * alert sheet or a custom sheet would appear, Tasks shows one of these
 * instead: tasks_ui_notice and tasks_ui_confirm (ui_util.h) are built on
 * it; Edit Tag and the date pickers build their own with their controls.
 *
 * A tool panel is a utility-titlebar panel with a solid blue hero strip
 * (symbol, bold title, subtitle) at the top, the caller's controls or a
 * message below it, and a button bar at the bottom.  It is not modal: it
 * floats above ordinary windows until the user clicks a button, presses
 * Return (the default button), or presses Escape or the close box, which
 * dismiss it without an answer — so Escape never answers a destructive
 * question.  Return never destroys either: a destructive button is never
 * the default (tasks_ui_confirm puts Cancel there).  The app's own text
 * size (tasks_look_ui_font) throughout, as in Notes.
 *
 * The same panel as Notes' tool_panel.h, apart from the prefix: keep them
 * alike.  Also here: the hero strip itself, which the reminders window
 * shares.
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
 *   subtitle — the second line, in the subtitle colour; nil or empty for
 *              none
 */
- (void)set_title:(NSString *)title subtitle:(NSString *)subtitle;

/*
 * show_problem: — a one-line problem in the subtitle's place, highlighted
 * (a tool panel's handler refusing an answer).  set_title:subtitle: puts
 * the subtitle back.
 *
 * Inputs:
 *   problem — the line
 */
- (void)show_problem:(NSString *)problem;

@end

/*
 * TasksToolPanel — one panel on screen.  It keeps itself alive while it
 * is showing and lets go of its handlers when it closes.
 */
@interface TasksToolPanel : NSObject

/*
 * panel_with_title:hero_title:subtitle:symbol:content:buttons:destructive:handler:
 * — build a panel that asks for something: the caller's controls above
 * its buttons (Edit Tag's OK / Cancel, a date's Set / Clear / Cancel).
 * show_over: displays it.
 *
 * Inputs:
 *   window_title — title bar text
 *   hero_title   — bold line in the hero strip
 *   subtitle     — second line in the hero strip; problems show there too
 *   symbol       — SF Symbols name for the hero icon; may be nil
 *   content      — the controls, sized by their own fittingSize (fixed
 *                  widths and heights); not retained after close
 *   buttons      — button labels, at least one: [0] is the default (Return)
 *                  and sits rightmost, the rest follow to its left
 *   destructive  — index of the button that takes the destructive look,
 *                  never 0; -1 for none
 *   handler      — called with the index of the clicked button (not when
 *                  the panel is dismissed); returns nil to close the panel
 *                  or a one-line problem to show in the hero strip (the
 *                  panel stays open)
 *
 * Output:
 *   the panel, not yet shown.
 */
+ (instancetype)panel_with_title:(NSString *)window_title
                      hero_title:(NSString *)hero_title
                        subtitle:(NSString *)subtitle
                          symbol:(NSString *)symbol
                         content:(NSView *)content
                         buttons:(NSArray<NSString *> *)buttons
                     destructive:(NSInteger)destructive
                         handler:(NSString *(^)(NSInteger index))handler;

/*
 * show_message:message:symbol:buttons:destructive:over:handler: — show a
 * message in a tool panel and return at once: the replacement for a
 * system alert.
 *
 * Inputs:
 *   title       — headline, in the hero strip
 *   message     — wrapped, selectable body text (long text scrolls); may
 *                 be nil
 *   symbol      — SF Symbols name for the hero icon; may be nil
 *   buttons     — button labels, at least one: [0] is the default (Return)
 *                 and sits rightmost, the rest follow to its left
 *   destructive — index of the button that takes the destructive look,
 *                 never 0; -1 for none
 *   parent      — window to centre over; nil = the screen
 *   handler     — called once on the main thread with the index of the
 *                 clicked button, or -1 when the panel was dismissed
 *                 (Escape or the close box) without one; may be nil
 */
+ (void)show_message:(NSString *)title
             message:(NSString *)message
              symbol:(NSString *)symbol
             buttons:(NSArray<NSString *> *)buttons
         destructive:(NSInteger)destructive
                over:(NSWindow *)parent
             handler:(void (^)(NSInteger index))handler;

/*
 * show_over: — centre the panel over a window (the screen if nil), bring it
 * forward and make it key.
 *
 * Inputs:
 *   parent — window to centre over; may be nil
 */
- (void)show_over:(NSWindow *)parent;

/*
 * window — the underlying panel, e.g. to set the first responder.
 *
 * Output:
 *   the NSPanel.
 */
- (NSPanel *)window;

@end
