#import "settings_window.h"
#import "look.h"
#import "ui_util.h"
#import "task_notify.h"
#import "hotkey.h"

#include "backup.h"
#include "gcal_store.h"
#include "gcal_sync.h"
#include "oauth.h"
#include "task_config.h"
#include "task_health.h"
#include "task_ops.h"
#include "task_paths.h"

#include <time.h>

/* ── Metrics and limits ─────────────────────────────────────────────── */

/* Space under a section heading (the gap BETWEEN sections is
 * tasks_look_section_gap). */
static const CGFloat heading_gap = 8.0;

/* The Database plate: a bordered panel in the text background colour, so
 * what the database IS reads as a different kind of thing from the
 * controls below that change it. */
static const CGFloat plate_radius  = 6.0;
static const CGFloat plate_pad_h   = 10.0;  /* border to text, left and right */
static const CGFloat plate_pad_v   = 8.0;   /* ... above and below             */
static const CGFloat plate_col_gap = 8.0;   /* label column to value column    */
static const CGFloat plate_row_gap = 4.0;
static const CGFloat plate_value_w = 270.0; /* where a long path wraps: the
                                             * plate's width less the labels
                                             * and the "copy" link            */
static const CGFloat plate_part_gap = 5.0;  /* between the parts of one value:
                                             * the light and its verdict, a
                                             * value and its "copy" link      */

/* How much of the 64-character digest is shown: enough to compare at a
 * glance, short enough to sit on one line.  The whole of it is in the
 * tooltip and is what "copy" copies. */
static const NSUInteger sha_head = 8;
static const NSUInteger sha_tail = 4;

/* The backup schedule's interval field.  The interval is in minutes, 0
 * meaning "only when Back Up Now is pressed"; its range and step are the
 * shared module's (SHARED_BACKUP_INTERVAL_MIN_MAX / _STEP). */
static const CGFloat   interval_field_w     = 56.0;  /* five digits  */
/* The calendar sync's interval: up to a day, a minute at a time. */
static const NSInteger gcal_interval_max    = 1440;
static const CGFloat   keep_field_w         = 44.0;  /* three digits */
static const CGFloat   stepper_gap          = 2.0;   /* a field to its stepper */

/* ── The Appearance options ─────────────────────────────────────────── */

/* One on/off setting: a checkbox that writes a bool key.  All of them
 * change what the task pane (or the planner and reminders beside it) draws,
 * and those re-read their settings on every refresh, so one action serves
 * the whole table. */
typedef struct {
    NSString * __unsafe_unretained title;
    const char                    *key;      /* a TASK_CFG_* bool key */
    NSString * __unsafe_unretained tooltip;  /* nil for none          */
} option_spec_t;

static const option_spec_t appearance_options[] = {
    { @"Show task titles in bold",          TASK_CFG_BOLD_TITLES,   nil },
    { @"Show drop shadows on Kanban cards", TASK_CFG_KANBAN_SHADOW,
      @"Lifts each card off its lane." },
    { @"Count only business days (Mon\u2013Fri) in the 3-Day Forecast",
      TASK_CFG_FORECAST_BUSINESS_DAYS,
      @"On a Friday the forecast runs to Tuesday; past-due tasks are always shown." },
    { @"Show task counts next to tags in the sidebar",
      TASK_CFG_SIDEBAR_COUNTS,
      @"How many open tasks (not Done) carry each tag." },
};
#define APPEARANCE_OPTION_COUNT (sizeof(appearance_options) / sizeof(appearance_options[0]))

/* ── View factories ─────────────────────────────────────────────────── */

/*
 * form_content_w — the width a full-width row of the form has: the window's
 * width less the margin on each side.
 *
 * Output:
 *   the width in points.
 */
static CGFloat
form_content_w(void)
{
    return tasks_look_settings_w - 2 * tasks_look_form_margin;
}

/*
 * section_label — a bold section heading.
 *
 * Inputs:
 *   title — heading text
 *
 * Output:
 *   configured label.
 */
static NSTextField *
section_label(NSString *title)
{
    NSTextField *label = [NSTextField labelWithString:title];

    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

/*
 * small_label — a one-line label in the small system font: the plate's
 * text.
 *
 * Inputs:
 *   text  — the text
 *   color — its colour
 *
 * Output:
 *   configured label.
 */
static NSTextField *
small_label(NSString *text, NSColor *color)
{
    NSTextField *label = [NSTextField labelWithString:text];

    label.font      = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor = color;
    return label;
}

/*
 * wrapping_label — small text that wraps at a given width.  The width is
 * stated because a wrapping label has no fitting height without one, and
 * the window is sized from its content's fitting height.
 *
 * Inputs:
 *   color — its colour
 *   width — where it wraps, in points
 *
 * Output:
 *   configured, empty label.
 */
static NSTextField *
wrapping_label(NSColor *color, CGFloat width)
{
    NSTextField *label = [NSTextField wrappingLabelWithString:@""];

    label.font                    = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor               = color;
    label.preferredMaxLayoutWidth = width;
    return label;
}

/*
 * plate_value — a value of the Database plate: small, selectable (so it
 * can be copied), wrapping where a long path needs to.
 *
 * Output:
 *   configured, empty label.
 */
static NSTextField *
plate_value(void)
{
    NSTextField *label = wrapping_label(NSColor.labelColor, plate_value_w);

    label.selectable = YES;
    return label;
}

/*
 * copy_link — a borderless, link-coloured "copy" button to sit beside a
 * value, as in Files' Get Info.
 *
 * Inputs:
 *   target — receives the action
 *   action — sent on a click
 *
 * Output:
 *   configured button.
 */
static NSButton *
copy_link(id target, SEL action)
{
    NSButton *button = [NSButton buttonWithTitle:@"" target:target action:action];

    button.bordered        = NO;
    button.attributedTitle = [[NSAttributedString alloc] initWithString:@"copy" attributes:@{
        NSForegroundColorAttributeName: tasks_color(TasksColorLink),
        NSFontAttributeName:            [NSFont systemFontOfSize:NSFont.labelFontSize],
    }];
    return button;
}

/*
 * value_row — the parts of one plate value side by side on one baseline.
 *
 * Inputs:
 *   parts — the views, left to right
 *
 * Output:
 *   the row.
 */
static NSStackView *
value_row(NSArray<NSView *> *parts)
{
    NSStackView *row = [NSStackView stackViewWithViews:parts];

    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment   = NSLayoutAttributeFirstBaseline;
    row.spacing     = plate_part_gap;
    return row;
}

/*
 * number_field — a right-aligned field for a small whole number.  Its
 * action is sent when editing ends by any route (Return, Tab, a click
 * elsewhere), so a typed value is not lost by leaving the field.
 *
 * Inputs:
 *   width  — the field's width in points
 *   target — receives the action
 *   action — sent when editing ends
 *
 * Output:
 *   configured field.
 */
static NSTextField *
number_field(CGFloat width, id target, SEL action)
{
    NSTextField *field = [NSTextField textFieldWithString:@""];

    field.alignment = NSTextAlignmentRight;
    field.target    = target;
    field.action    = action;
    field.cell.sendsActionOnEndEditing = YES;
    [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

/*
 * number_stepper — the stepper beside a number_field.  It also holds the
 * pair's range, which is what a typed value is clamped to.
 *
 * Inputs:
 *   lo, hi — the range
 *   step   — what one click adds or takes away
 *   target — receives the action
 *   action — sent on each click
 *
 * Output:
 *   configured stepper.
 */
static NSStepper *
number_stepper(NSInteger lo, NSInteger hi, NSInteger step, id target, SEL action)
{
    NSStepper *stepper = [[NSStepper alloc] initWithFrame:NSZeroRect];

    stepper.minValue   = lo;
    stepper.maxValue   = hi;
    stepper.increment  = step;
    stepper.valueWraps = NO;
    stepper.target     = target;
    stepper.action     = action;
    return stepper;
}

/*
 * button_row — push buttons hugging the right edge of a full-width row, in
 * the order given.  An empty view takes the spare width.
 *
 * Inputs:
 *   buttons — the buttons, left to right
 *
 * Output:
 *   the row.
 */
static NSStackView *
button_row(NSArray<NSButton *> *buttons)
{
    NSView *spacer = [[NSView alloc] initWithFrame:NSZeroRect];

    [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                       forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row =
        [NSStackView stackViewWithViews:[@[spacer] arrayByAddingObjectsFromArray:buttons]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    [row.widthAnchor constraintEqualToConstant:form_content_w()].active = YES;
    return row;
}

/*
 * copy_to_pasteboard — replace the general pasteboard's contents with a
 * string.
 *
 * Inputs:
 *   text — the string
 */
static void
copy_to_pasteboard(NSString *text)
{
    NSPasteboard *board = NSPasteboard.generalPasteboard;

    [board clearContents];
    [board setString:text forType:NSPasteboardTypeString];
}

/*
 * TasksSettingsForm — the scroll view's document: a view whose origin is
 * its TOP-left corner, so that the form starts at the top of the window
 * and a form taller than the window scrolls down from there.  (A plain
 * view's origin is bottom-left, and a scroll view would open on the form's
 * last rows.)
 */
@interface TasksSettingsForm : NSView
@end

@implementation TasksSettingsForm

/*
 * isFlipped — y grows downwards in this view.
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

/* ── The window ─────────────────────────────────────────────────────── */

@interface TasksSettingsWindow : NSWindowController <NSWindowDelegate, NSMenuDelegate>
@property (nonatomic, assign) TaskApp *app;              /* borrowed; outlives the window */
@property (nonatomic, strong) NSScrollView *scroll;      /* the window's content          */
@property (nonatomic, strong) NSStackView *form;         /* every row, top to bottom      */

/* The Database plate: five facts about one file, refreshed together. */
@property (nonatomic, strong) NSTextField *health_light; /* the round indicator           */
@property (nonatomic, strong) NSTextField *health_label; /* the verdict and when          */
@property (nonatomic, strong) NSTextField *path_label;
@property (nonatomic, strong) NSTextField *data_label;   /* "42 tasks with 8 tags"        */
@property (nonatomic, strong) NSTextField *size_label;
@property (nonatomic, strong) NSTextField *sha_label;    /* the digest, shortened         */
@property (nonatomic, strong) NSButton *sha_copy;        /* its "copy" link               */
@property (nonatomic, copy) NSString *sha256;            /* the whole digest; nil when the
                                                          * file could not be read        */
@property (nonatomic, strong) NSButton *update_button;
/* The id of the probe whose answer the plate is waiting for; 0 for none.
 * An answer carrying any other id is from a superseded request. */
@property (nonatomic, assign) NSUInteger awaited_probe;
/* Whether that probe is an Update (it ran the integrity check and its
 * outcome is announced) rather than the read made when the window opens. */
@property (nonatomic, assign) BOOL checking;

/* Backups. */
@property (nonatomic, strong) NSButton *backup_box;      /* the master switch             */
@property (nonatomic, strong) NSTextField *interval_field;
@property (nonatomic, strong) NSStepper *interval_stepper;
@property (nonatomic, strong) NSTextField *keep_field;
@property (nonatomic, strong) NSStepper *keep_stepper;
@property (nonatomic, strong) NSTextField *folder_label; /* "Backing up to <folder>"      */
@property (nonatomic, strong) NSTextField *fate_label;   /* the "alongside the database"
                                                          * warning; hidden otherwise     */
@property (nonatomic, strong) NSTextField *last_label;   /* backup_last_at: when the last
                                                          * backup was made; hidden before
                                                          * the first                     */
@property (nonatomic, strong) NSTextField *error_label;  /* backup_last_error: why the last
                                                          * backup failed; hidden when it
                                                          * did not                       */
@property (nonatomic, strong) NSButton *choose_button;
@property (nonatomic, strong) NSButton *backup_button;
@property (nonatomic, strong) NSTextField *backup_status; /* the last Back Up Now's outcome;
                                                           * hidden until there is one    */

/* New tasks. */
@property (nonatomic, strong) NSPopUpButton *default_tag_popup; /* None, then every tag;
                                                                 * rebuilt as it opens  */

/* Notifications. */
@property (nonatomic, strong) NSButton *notif_tasks_btn;   /* radio: Tasks popup  */
@property (nonatomic, strong) NSButton *notif_native_btn;  /* radio: macOS Notif. */

/* Google Calendar.  Two boxes are rebuilt by refresh_gcal whenever the
 * sign-in, a pass or a choice changes them: the status and its buttons,
 * and the calendar choices.  Between them the switch and the interval stay
 * put, so a pass finishing never takes away a field being typed in. */
@property (nonatomic, strong) NSStackView *gcal_box;
@property (nonatomic, strong) NSStackView *gcal_calendars_box;
@property (nonatomic, strong) NSButton *gcal_switch;
@property (nonatomic, strong) NSTextField *gcal_interval_field;
@property (nonatomic, strong) NSStepper *gcal_interval_stepper;
@property (nonatomic, assign) BOOL gcal_signing_in;        /* the browser is open   */
@property (nonatomic, assign) BOOL gcal_refresh_queued;
@property (nonatomic, assign) guint gcal_listener;         /* task events: a pass ended */

@property (nonatomic, assign) BOOL is_shut_down;

/* Declared here because the C callbacks below call them before their
 * definitions; documented where they are defined. */
- (void)show_probe:(const task_health_probe_t *)probe;
- (void)show_backup_status:(NSString *)text;
- (void)refresh_backup;
- (void)queue_gcal_refresh;
@end

/* The one Settings window while it is open; nil otherwise.  This reference
 * is what owns the controller. */
static TasksSettingsWindow *settings_window;

/* Probe ids count up and are never reused, so an answer can be matched to
 * the window, and the request, that asked for it. */
static NSUInteger last_probe_id;

/* ── Core callbacks ─────────────────────────────────────────────────── */

/*
 * on_gcal_tasks — a task event, which is also how a finished calendar
 * pass announces itself: the section's status line may have changed.
 *
 * Inputs:
 *   app  — the application context (unused)
 *   data — unused
 */
static void
on_gcal_tasks(TaskApp *app, gpointer data)
{
    [settings_window queue_gcal_refresh];
}

/*
 * on_signed_in — the browser sign-in finished (oauth.h).  The outcome is
 * news about the app's sign-in, so it is shown whether or not the window
 * that started it is still open.
 *
 * Inputs:
 *   ok    — whether a grant is now stored
 *   error — why not (borrowed), or NULL
 *   data  — unused
 */
static void
on_signed_in(gboolean ok, const gchar *error, gpointer data)
{
    settings_window.gcal_signing_in = NO;
    if (ok) {
        gcal_sync_settings_changed();
    } else {
        shared_ui_notice(settings_window.window, @"Google sign-in failed",
                        tasks_string(error != NULL ? error : "The sign-in did not finish."), nil);
    }
    [settings_window queue_gcal_refresh];
}

/*
 * on_probe — a database probe has reported (task_health.h).  The answer is
 * shown only if the window that asked is still open and still waiting for
 * THIS probe: the staleness guard for a window closed, or an Update pressed
 * again, while the probe ran.
 *
 * Inputs:
 *   app   — the application context (unused)
 *   probe — what was found (borrowed)
 *   data  — the probe's id, as a pointer-sized integer
 */
static void
on_probe(TaskApp *app, const task_health_probe_t *probe, void *data)
{
    TasksSettingsWindow *window = settings_window;

    if (window == nil || window.awaited_probe != (NSUInteger)data) return;
    [window show_probe:probe];
}

/*
 * on_backup_done — a Back Up Now pass has finished (backup.h).  Reported to
 * whichever Settings window is open now, since the outcome is news about
 * the app's backups rather than about the window that asked; with none
 * open the library's status bar has already said it.
 *
 * Inputs:
 *   app     — the application context (unused)
 *   ok      — whether the pass succeeded
 *   message — its one-line outcome (borrowed)
 *   data    — unused
 */
static void
on_backup_done(TaskApp *app, gboolean ok, const gchar *message, gpointer data)
{
    NSString *text = message != NULL ? tasks_string(message)
                   : ok              ? @"Backed up" : @"Backup failed";

    [settings_window refresh_backup];
    [settings_window show_backup_status:text];
}

@implementation TasksSettingsWindow

/* ── Construction ────────────────────────────────────────────────────── */

/*
 * init_with_app: — build the Settings window, sized to its content.
 *
 * Inputs:
 *   app — the application context (its open database is described)
 *
 * Output:
 *   the controller, owned by the module-level reference.
 */
- (instancetype)
init_with_app:(TaskApp *)app
{
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, tasks_look_settings_w, tasks_look_settings_max_h)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:YES];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _app            = app;
    window.title    = @"Settings";
    window.delegate = self;

    [self build_form];
    [self refresh_gcal];
    _gcal_listener = task_app_listen_tasks(app, on_gcal_tasks, NULL);
    [self refresh_backup];
    [self refresh_database];
    [self fit_window];
    [window center];
    return self;
}

/*
 * appearance_rows — the Appearance section: its heading and one checkbox
 * per entry of appearance_options, each showing its setting.
 *
 * Output:
 *   the section's rows, heading first.
 */
- (NSArray<NSView *> *)
appearance_rows
{
    NSMutableArray<NSView *> *rows = [NSMutableArray arrayWithObject:section_label(@"Appearance")];

    for (NSUInteger i = 0; i < APPEARANCE_OPTION_COUNT; i++) {
        NSButton *box = [NSButton checkboxWithTitle:appearance_options[i].title
                                             target:self
                                             action:@selector(toggle_option:)];
        box.tag     = (NSInteger)i;
        box.toolTip = appearance_options[i].tooltip;
        box.state   = task_config_get_bool(appearance_options[i].key) ? NSControlStateValueOn
                                                                       : NSControlStateValueOff;
        [rows addObject:box];
    }
    return rows;
}

/*
 * database_plate — the bordered panel of five facts about the database.
 *
 * Health leads because it is the one line that can be bad news; the path,
 * the two quantities and the digest follow as what the verdict is about.
 * The values sit in a grid so that they all start at one x: five lines
 * each starting where its own label ended would read as five unrelated
 * sentences.
 *
 * Output:
 *   the plate, as wide as the form's content.
 */
- (NSBox *)
database_plate
{
    NSColor *dim = NSColor.secondaryLabelColor;

    self.health_light = small_label(@"●", NSColor.tertiaryLabelColor);
    self.health_label = small_label(@"", NSColor.labelColor);
    self.path_label   = plate_value();
    self.data_label   = plate_value();
    self.size_label   = plate_value();

    /* The digest is shown shortened, so it is not selectable (a selection
     * would copy the short form); its "copy" link copies the whole of it. */
    self.sha_label      = small_label(@"—", NSColor.labelColor);
    self.sha_label.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize
                                                      weight:NSFontWeightRegular];
    self.sha_copy       = copy_link(self, @selector(copy_sha:));

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[small_label(@"Health:", dim),
          value_row(@[self.health_light, self.health_label])],
        @[small_label(@"Current database:", dim),
          value_row(@[self.path_label, copy_link(self, @selector(copy_path:))])],
        @[small_label(@"Data:", dim),         self.data_label],
        @[small_label(@"Size on disk:", dim), self.size_label],
        @[small_label(@"SHA-256:", dim),
          value_row(@[self.sha_label, self.sha_copy])],
    ]];
    grid.columnSpacing = plate_col_gap;
    grid.rowSpacing    = plate_row_gap;
    grid.rowAlignment  = NSGridRowAlignmentFirstBaseline;
    grid.translatesAutoresizingMaskIntoConstraints = NO;

    NSBox *plate = [[NSBox alloc] initWithFrame:NSZeroRect];
    plate.boxType            = NSBoxCustom;
    plate.titlePosition      = NSNoTitle;
    plate.fillColor          = NSColor.textBackgroundColor;
    plate.borderColor        = NSColor.separatorColor;
    plate.borderWidth        = 1.0;
    plate.cornerRadius       = plate_radius;
    plate.contentViewMargins = NSMakeSize(plate_pad_h, plate_pad_v);
    [plate.widthAnchor constraintEqualToConstant:form_content_w()].active = YES;

    /* The grid keeps its natural width (the trailing edge is only a
     * limit), so no column is stretched to fill the plate. */
    NSView *inside = plate.contentView;
    [inside addSubview:grid];
    [NSLayoutConstraint activateConstraints:@[
        [grid.leadingAnchor constraintEqualToAnchor:inside.leadingAnchor],
        [grid.topAnchor constraintEqualToAnchor:inside.topAnchor],
        [grid.bottomAnchor constraintEqualToAnchor:inside.bottomAnchor],
        [grid.trailingAnchor constraintLessThanOrEqualToAnchor:inside.trailingAnchor],
    ]];
    return plate;
}

/*
 * database_rows — the Database section: its heading, the plate, and under
 * the plate the Update button, which sits under the whole plate rather
 * than beside one line because it renews every line.
 *
 * There is deliberately no control here for WHERE the database is kept: a
 * database somewhere else is opened with File > Open Database File.
 *
 * Output:
 *   the section's rows, heading first.
 */
- (NSArray<NSView *> *)
database_rows
{
    self.update_button = [NSButton buttonWithTitle:@"Update"
                                            target:self
                                            action:@selector(update_database:)];
    self.update_button.toolTip =
        @"Re-read every line above: run PRAGMA integrity_check and PRAGMA foreign_key_check "
        @"against this database, then re-count its tasks and tags and re-read its size and "
        @"SHA-256.";

    return @[section_label(@"Database"), [self database_plate],
             button_row(@[self.update_button])];
}

/*
 * schedule_row — "Every [n] minutes, keeping [m] files": the backup's
 * interval and how many files are retained, each a field with a stepper.
 * The retention bound has a hard floor of one, because a rotation that
 * keeps nothing is not a rotation.
 *
 * Output:
 *   the row.
 */
- (NSStackView *)
schedule_row
{
    self.interval_field   = number_field(interval_field_w, self, @selector(interval_changed:));
    self.interval_stepper = number_stepper(0, SHARED_BACKUP_INTERVAL_MIN_MAX,
                                           SHARED_BACKUP_INTERVAL_MIN_STEP,
                                           self, @selector(interval_changed:));
    self.keep_field       = number_field(keep_field_w, self, @selector(keep_changed:));
    self.keep_stepper     = number_stepper(SHARED_BACKUP_KEEP_MIN, SHARED_BACKUP_KEEP_MAX, 1,
                                           self, @selector(keep_changed:));

    self.interval_field.toolTip =
        @"Minutes between backups. 0 backs up only when you press Back Up Now. Notes "
        @"and Tasks share this setting, and whichever is running makes the backup.";
    self.keep_field.toolTip =
        @"How many backup files to retain. The oldest are removed once a new backup has been "
        @"verified, never before.";

    [self show_number:[self policy:SHARED_SETTING_BACKUP_INTERVAL_MIN]
                field:self.interval_field
              stepper:self.interval_stepper];
    [self show_number:[self policy:SHARED_SETTING_BACKUP_KEEP]
                field:self.keep_field
              stepper:self.keep_stepper];

    NSStackView *row = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Every"], self.interval_field, self.interval_stepper,
        [NSTextField labelWithString:@"minutes, keeping"], self.keep_field, self.keep_stepper,
        [NSTextField labelWithString:@"files"],
    ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment   = NSLayoutAttributeFirstBaseline;
    [row setCustomSpacing:stepper_gap afterView:self.interval_field];
    [row setCustomSpacing:stepper_gap afterView:self.keep_field];
    return row;
}

/*
 * backup_rows — the Backups section: the master switch, the schedule it
 * turns on, where the copies land (with the warning when that is beside
 * the database), when the last backup was made and why the last pass
 * failed when it did (both can show: a copy made whose pruning failed),
 * the two buttons that answer those lines, and the outcome of the last
 * Back Up Now.
 *
 * Output:
 *   the section's rows, heading first.
 */
- (NSArray<NSView *> *)
backup_rows
{
    CGFloat  text_w = form_content_w();
    NSColor *dim    = NSColor.secondaryLabelColor;

    self.backup_box = [NSButton checkboxWithTitle:@"Back up the database automatically"
                                           target:self
                                           action:@selector(toggle_backup:)];
    self.backup_box.toolTip =
        @"Writes a verified copy of the database Notes and Tasks share into a folder of your "
        @"choice on a timer, keeping only the most recent few. Both apps follow these "
        @"settings. Worth pointing at a disk independent of wherever the database itself "
        @"lives, so one mishap cannot take both.";

    self.folder_label  = wrapping_label(dim, text_w);
    self.fate_label    = wrapping_label(dim, text_w);
    self.backup_status = wrapping_label(dim, text_w);
    self.last_label    = wrapping_label(dim, text_w);
    self.error_label   = wrapping_label(NSColor.systemRedColor, text_w);
    self.fate_label.stringValue = @"⚠ alongside the database — change it to survive "
                                  @"losing that folder";
    self.backup_status.hidden   = YES;

    self.choose_button = [NSButton buttonWithTitle:@"Change Folder…"
                                            target:self
                                            action:@selector(choose_folder:)];
    self.backup_button = [NSButton buttonWithTitle:@"Back Up Now"
                                            target:self
                                            action:@selector(back_up_now:)];

    return @[section_label(@"Backups"), self.backup_box, [self schedule_row],
             self.folder_label, self.fate_label, self.last_label, self.error_label,
             button_row(@[self.choose_button, self.backup_button]), self.backup_status];
}

/*
 * new_task_rows — the New Tasks section: the default tag, and whether
 * Quick Task's shortcut works from any app.
 *
 * Output:
 *   the section's rows, heading first.
 */
- (NSArray<NSView *> *)
new_task_rows
{
    self.default_tag_popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    self.default_tag_popup.target        = self;
    self.default_tag_popup.action        = @selector(default_tag_changed:);
    self.default_tag_popup.menu.delegate = self;
    self.default_tag_popup.toolTip       =
        @"New Task gives its task this tag when a view is selected; Quick Task always does.";
    [self menuNeedsUpdate:self.default_tag_popup.menu];

    NSStackView *tag_row = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Default tag:"], self.default_tag_popup]];
    tag_row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    tag_row.alignment   = NSLayoutAttributeFirstBaseline;

    NSButton *hotkey = [NSButton checkboxWithTitle:
                            @"Quick Task (\u2303\u2325\u2318T) works from any app"
                                            target:self
                                            action:@selector(hotkey_toggled:)];
    hotkey.state = task_config_get_bool(TASK_CFG_QUICK_TASK_HOTKEY) ? NSControlStateValueOn
                                                                      : NSControlStateValueOff;

    NSTextField *note = wrapping_label(NSColor.secondaryLabelColor, form_content_w());
    note.stringValue =
        @"Every task has a tag. New Task gives the default tag when a view is selected "
        @"rather than a tag. Quick Task always gives it, and opens its editor in the "
        @"lower-right corner of the screen.";

    return @[section_label(@"New Tasks"), tag_row, hotkey, note];
}

/*
 * menuNeedsUpdate: — the default-tag popup's menu is about to open (or is
 * being built): fill it afresh from the database, so tags made or gone
 * while Settings is open are offered as they now are.  "None" leads.  A
 * default that no task carries just now is offered too, by its name: the
 * next task given it makes the tag again.
 *
 * Inputs:
 *   menu — the popup's menu
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    gchar    *chosen  = task_ops_default_tag(self.app);
    NSString *current = chosen != NULL ? tasks_string(chosen) : nil;
    g_free(chosen);

    [menu removeAllItems];
    NSMenuItem *none = [menu addItemWithTitle:@"None" action:NULL keyEquivalent:@""];
    [menu addItem:NSMenuItem.separatorItem];

    NSMenuItem *selected = none;
    GPtrArray  *tags     = task_db_tags(self.app->db, TASK_TAGS_ALL);
    for (guint i = 0; i < tags->len; i++) {
        const TaskTag *tag  = g_ptr_array_index(tags, i);
        NSMenuItem    *item = [menu addItemWithTitle:tasks_tag_title(tag) action:NULL
                                       keyEquivalent:@""];
        item.representedObject = tasks_string(tag->name);
        if (current != nil && [current caseInsensitiveCompare:item.representedObject] == NSOrderedSame) {
            selected = item;
        }
    }
    task_ptr_array_free_tags(tags);
    if (current != nil && selected == none) {
        selected = [menu addItemWithTitle:current action:NULL keyEquivalent:@""];
        selected.representedObject = current;
    }
    [self.default_tag_popup selectItem:selected];
}

/*
 * default_tag_changed: — a default tag (or None) was chosen.
 *
 * Inputs:
 *   sender — the popup; the chosen item carries the tag's name, nil for
 *            None
 */
- (void)
default_tag_changed:(NSPopUpButton *)sender
{
    NSString *name = sender.selectedItem.representedObject;
    task_config_set(TASK_CFG_DEFAULT_TAG, name != nil ? name.UTF8String : NULL);
}

/*
 * hotkey_toggled: — the Quick Task shortcut checkbox changed: write the
 * setting and register or unregister the shortcut.  A shortcut another app
 * holds cannot be registered: the box goes back off, the setting with it,
 * and a notice says why.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
hotkey_toggled:(NSButton *)sender
{
    task_config_set_bool(TASK_CFG_QUICK_TASK_HOTKEY, sender.state == NSControlStateValueOn);
    if (tasks_hotkey_apply()) return;

    sender.state = NSControlStateValueOff;
    task_config_set_bool(TASK_CFG_QUICK_TASK_HOTKEY, FALSE);
    shared_ui_notice(self.window, @"The shortcut is in use",
                    @"Another app has already taken \u2303\u2325\u2318T.", nil);
}

/*
 * notification_rows — the Notifications section: a heading and two radio
 * buttons for the delivery mode.
 *
 * Output:
 *   the section's rows, heading first.
 */
- (NSArray<NSView *> *)
notification_rows
{
    gchar *mode_val = task_config_get(TASK_CFG_NOTIF_MODE);
    BOOL native = (mode_val != NULL &&
                   strcmp(mode_val, TASK_NOTIF_MODE_NATIVE) == 0);
    g_free(mode_val);

    self.notif_tasks_btn  = [NSButton radioButtonWithTitle:@"Tasks notification (popup window)"
                                                    target:self
                                                    action:@selector(notif_mode_changed:)];
    self.notif_native_btn = [NSButton radioButtonWithTitle:@"macOS Notification Center"
                                                    target:self
                                                    action:@selector(notif_mode_changed:)];

    self.notif_tasks_btn.tag  = 0;
    self.notif_native_btn.tag = 1;

    self.notif_tasks_btn.state  = native ? NSControlStateValueOff : NSControlStateValueOn;
    self.notif_native_btn.state = native ? NSControlStateValueOn  : NSControlStateValueOff;

    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"Tasks notifications appear in the lower-right corner. "
        @"macOS Notification Center alerts require permission from macOS."];
    note.font      = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;
    note.preferredMaxLayoutWidth = form_content_w();

    return @[section_label(@"Notifications"),
             self.notif_tasks_btn, self.notif_native_btn, note];
}

/*
 * notif_mode_changed: — a notification-mode radio button was clicked:
 * write the new mode.  When native mode is selected, request
 * UNUserNotificationCenter authorization if not yet granted.
 *
 * Inputs:
 *   sender — the radio button; tag 0 = tasks, tag 1 = native
 */
- (void)
notif_mode_changed:(NSButton *)sender
{
    BOOL native = sender.tag == 1;

    /* Keep the pair in sync — only the clicked button fires the action. */
    self.notif_tasks_btn.state  = native ? NSControlStateValueOff : NSControlStateValueOn;
    self.notif_native_btn.state = native ? NSControlStateValueOn  : NSControlStateValueOff;

    task_config_set(TASK_CFG_NOTIF_MODE,
                    native ? TASK_NOTIF_MODE_NATIVE : TASK_NOTIF_MODE_TASKS);

    if (native)
        task_notify_request_native_auth();
}

/* ── Google Calendar ─────────────────────────────────────────────────── */

/*
 * gcal_box_view — an empty vertical box for refresh_gcal to fill.
 *
 * Output:
 *   the box.
 */
static NSStackView *
gcal_box_view(void)
{
    NSStackView *box = [NSStackView stackViewWithViews:@[]];
    box.orientation = NSUserInterfaceLayoutOrientationVertical;
    box.alignment   = NSLayoutAttributeLeading;
    box.spacing     = tasks_look_form_spacing;
    return box;
}

/*
 * gcal_interval_row — "Sync every [n] minutes": the calendar sync's
 * timer, a field with a stepper like the backup's.
 *
 * Output:
 *   the row.
 */
- (NSStackView *)
gcal_interval_row
{
    self.gcal_interval_field   = number_field(interval_field_w, self,
                                              @selector(gcal_interval_changed:));
    self.gcal_interval_stepper = number_stepper(0, gcal_interval_max, 1, self,
                                                @selector(gcal_interval_changed:));
    self.gcal_interval_field.toolTip =
        @"Minutes between syncs, which is how soon changes made in Google Calendar reach the "
        @"planner. Changes made in the planner are sent within seconds whatever this says. "
        @"0 syncs only on those changes and when you press Sync Now.";
    [self show_number:task_config_get_int(TASK_CFG_GCAL_INTERVAL_MIN)
                field:self.gcal_interval_field
              stepper:self.gcal_interval_stepper];

    NSStackView *row = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Sync every"], self.gcal_interval_field,
        self.gcal_interval_stepper, [NSTextField labelWithString:@"minutes"]]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment   = NSLayoutAttributeFirstBaseline;
    [row setCustomSpacing:stepper_gap afterView:self.gcal_interval_field];
    return row;
}

/*
 * gcal_rows — the Google Calendar section: its heading, the status box,
 * the switch, the interval and the calendars box.
 *
 * Output:
 *   the section's rows, heading first.
 */
- (NSArray<NSView *> *)
gcal_rows
{
    self.gcal_box           = gcal_box_view();
    self.gcal_calendars_box = gcal_box_view();
    self.gcal_switch = [self gcal_checkbox:@"Sync the day planner with Google Calendar"
                                    action:@selector(gcal_toggle:)
                                        on:task_config_get_bool(TASK_CFG_GCAL_ENABLED)];
    self.gcal_switch.toolTip =
        @"Tasks dragged onto the day planner become events in your calendar, and moving, "
        @"stretching or removing them changes the event. Events moved or deleted in Google "
        @"Calendar change the planner too. Events from the calendars you show are drawn in "
        @"the planner so you can schedule around them.";
    return @[section_label(@"Google Calendar"), self.gcal_box, self.gcal_switch,
             [self gcal_interval_row], self.gcal_calendars_box];
}

/*
 * gcal_checkbox:action:on: — one checkbox of the section.
 *
 * Inputs:
 *   title  — its label
 *   action — its action
 *   on     — its state
 *
 * Output:
 *   the checkbox.
 */
- (NSButton *)
gcal_checkbox:(NSString *)title action:(SEL)action on:(BOOL)on
{
    NSButton *box = [NSButton checkboxWithTitle:title target:self action:action];
    box.state     = on ? NSControlStateValueOn : NSControlStateValueOff;
    return box;
}

/*
 * gcal_calendar_rows:into: — the calendar choices: the popup choosing
 * where sessions go, and a checkbox per calendar for whose events the
 * planner shows.  The list is the one the last pass stored; before the
 * first pass there is nothing to choose from yet.
 *
 * Inputs:
 *   cals — gcal_calendar_t* (borrowed)
 *   rows — receives the views
 */
- (void)
gcal_calendar_rows:(GPtrArray *)cals into:(NSMutableArray<NSView *> *)rows
{
    gchar *push = task_config_get(TASK_CFG_GCAL_PUSH_CALENDAR);
    gchar *show = task_config_get(TASK_CFG_GCAL_SHOW_CALENDARS);
    gchar **shown = show != NULL ? g_strsplit(show, ",", -1) : NULL;

    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    popup.target = self;
    popup.action = @selector(gcal_push_changed:);
    for (guint i = 0; i < cals->len; i++) {
        const gcal_calendar_t *c = g_ptr_array_index(cals, i);
        if (!c->writable) continue;
        [popup addItemWithTitle:tasks_string(c->summary)];
        popup.lastItem.representedObject = tasks_string(c->id);
        if ((push == NULL && c->is_primary) || (push != NULL && strcmp(push, c->id) == 0))
            [popup selectItem:popup.lastItem];
    }
    NSStackView *push_row = [NSStackView stackViewWithViews:@[
        [NSTextField labelWithString:@"Add scheduled tasks to:"], popup]];
    push_row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    push_row.alignment   = NSLayoutAttributeFirstBaseline;
    [rows addObject:push_row];

    [rows addObject:[NSTextField labelWithString:@"Show events from:"]];
    for (guint i = 0; i < cals->len; i++) {
        const gcal_calendar_t *c  = g_ptr_array_index(cals, i);
        BOOL                   on = shown != NULL ? g_strv_contains((const gchar *const *)shown, c->id)
                                                  : c->is_primary;
        NSButton *box = [self gcal_checkbox:tasks_string(c->summary)
                                     action:@selector(gcal_show_changed:) on:on];
        box.identifier = tasks_string(c->id);
        [rows addObject:box];
    }
    g_strfreev(shown);
    g_free(show);
    g_free(push);
}

/*
 * refresh_gcal — rebuild the section from the sign-in, the last pass and
 * the settings.
 */
- (void)
refresh_gcal
{
    NSColor  *dim      = NSColor.secondaryLabelColor;
    CGFloat   text_w   = form_content_w();
    BOOL      client   = task_oauth_have_client();
    BOOL      signed_in = task_oauth_authenticated();
    NSMutableArray<NSView *> *rows = [NSMutableArray array];

    NSTextField *status = wrapping_label(dim, text_w);
    if (!client) {
        status.stringValue = @"No Google OAuth client is configured for this build. Put the "
                             @"Google Cloud console's Desktop-app client file (" @TASK_OAUTH_CLIENT_FILE
                             @") in the data folder, or build with client_credentials.mk.";
    } else if (self.gcal_signing_in) {
        status.stringValue = @"Waiting for the sign-in in your browser…";
    } else if (!signed_in) {
        status.stringValue = @"Not signed in.";
    } else {
        /* A failure carries Google's own message, which ends in a full
         * stop already; a "Last synced at" line does not. */
        gchar    *line = gcal_sync_status();
        NSString *stop = g_str_has_suffix(line, ".") ? @"" : @".";
        status.stringValue = task_config_get_bool(TASK_CFG_GCAL_ENABLED)
            ? [NSString stringWithFormat:@"Signed in. %@%@", tasks_string(line), stop]
            : @"Signed in. Syncing is off.";
        g_free(line);
    }
    [rows addObject:status];

    NSButton *sign = signed_in
        ? [NSButton buttonWithTitle:@"Sign Out" target:self action:@selector(gcal_sign_out:)]
        : [NSButton buttonWithTitle:@"Sign In to Google…" target:self action:@selector(gcal_sign_in:)];
    sign.enabled = client && !self.gcal_signing_in;
    NSButton *now = [NSButton buttonWithTitle:@"Sync Now" target:self action:@selector(gcal_sync_now:)];
    now.enabled  = gcal_sync_active();
    [rows addObject:button_row(@[sign, now])];

    [self.gcal_box setViews:rows inGravity:NSStackViewGravityTop];

    NSMutableArray<NSView *> *choices = [NSMutableArray array];
    if (signed_in) {
        GPtrArray *cals = gcal_store_calendars(self.app->db);
        if (cals->len > 0) [self gcal_calendar_rows:cals into:choices];
        g_ptr_array_free(cals, TRUE);
    }
    /* The interval means something only while the sync runs, as the
     * backup's does only while backups are on. */
    BOOL on = task_config_get_bool(TASK_CFG_GCAL_ENABLED);
    self.gcal_interval_field.enabled   = on;
    self.gcal_interval_stepper.enabled = on;

    [self.gcal_calendars_box setViews:choices inGravity:NSStackViewGravityTop];
    self.gcal_calendars_box.hidden = choices.count == 0;
    [self fit_window];
}

/*
 * queue_gcal_refresh — refresh_gcal on the next pass of the run loop:
 * the section is rebuilt from callbacks fired while one of its own
 * controls may still be sending its action.
 */
- (void)
queue_gcal_refresh
{
    if (self.is_shut_down || self.gcal_refresh_queued) return;
    self.gcal_refresh_queued = YES;
    __weak TasksSettingsWindow *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        TasksSettingsWindow *w = ws;
        if (w == nil || w.is_shut_down) return;
        w.gcal_refresh_queued = NO;
        [w refresh_gcal];
    });
}

/*
 * gcal_sign_in: — open Google's sign-in in the browser.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
gcal_sign_in:(id)sender
{
    gchar *error = NULL;
    gchar *url   = task_oauth_begin(on_signed_in, NULL, &error);
    if (url == NULL) {
        shared_ui_notice(self.window, @"Cannot sign in to Google", tasks_string(error), nil);
        g_free(error);
        return;
    }
    self.gcal_signing_in = YES;
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:tasks_string(url)]];
    g_free(url);
    [self queue_gcal_refresh];
}

/*
 * gcal_sign_out: — delete the stored sign-in and stop drawing the
 * calendar's events.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
gcal_sign_out:(id)sender
{
    task_oauth_signout();
    gcal_sync_signed_out();
    gcal_sync_settings_changed();
    [self queue_gcal_refresh];
}

/*
 * gcal_sync_now: — run a pass now.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
gcal_sync_now:(id)sender
{
    gcal_sync_now(TRUE);
    [self queue_gcal_refresh];
}

/*
 * gcal_toggle: — the master switch changed.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
gcal_toggle:(NSButton *)sender
{
    task_config_set_bool(TASK_CFG_GCAL_ENABLED, sender.state == NSControlStateValueOn);
    gcal_sync_settings_changed();
    [self queue_gcal_refresh];
}

/*
 * gcal_interval_changed: — the interval's field or stepper changed: write
 * it and restart the sync's timer on the new period.
 *
 * Inputs:
 *   sender — the field or the stepper
 */
- (void)
gcal_interval_changed:(NSControl *)sender
{
    NSInteger minutes = [self show_number:sender.integerValue
                                    field:self.gcal_interval_field
                                  stepper:self.gcal_interval_stepper];

    /* Leaving the field without changing it also lands here, and must not
     * start a pass. */
    if (minutes == task_config_get_int(TASK_CFG_GCAL_INTERVAL_MIN)) return;
    task_config_set_int(TASK_CFG_GCAL_INTERVAL_MIN, (gint)minutes);
    gcal_sync_settings_changed();
}

/*
 * gcal_push_changed: — a different calendar was chosen for sessions:
 * the next pass moves the existing ones there.
 *
 * Inputs:
 *   sender — the popup; the chosen item carries the calendar id
 */
- (void)
gcal_push_changed:(NSPopUpButton *)sender
{
    NSString *cid = sender.selectedItem.representedObject;
    task_config_set(TASK_CFG_GCAL_PUSH_CALENDAR, cid.UTF8String);
    gcal_sync_now(FALSE);
}

/*
 * gcal_show_changed: — a calendar was shown or hidden: write the whole
 * list of ticked calendars and pull again.
 *
 * Inputs:
 *   sender — the checkbox (unused: every box is read)
 */
- (void)
gcal_show_changed:(NSButton *)sender
{
    NSMutableArray<NSString *> *ids = [NSMutableArray array];
    for (NSView *v in self.gcal_calendars_box.views) {
        if ([v isKindOfClass:NSButton.class] && v.identifier.length > 0 &&
            ((NSButton *)v).state == NSControlStateValueOn)
            [ids addObject:v.identifier];
    }
    /* An empty list is a real choice ("show none"), unlike an unset key,
     * which means the primary calendar; a lone comma keeps it non-empty. */
    NSString *value = ids.count > 0 ? [ids componentsJoinedByString:@","] : @",";
    task_config_set(TASK_CFG_GCAL_SHOW_CALENDARS, value.UTF8String);
    gcal_sync_now(FALSE);
}

/*
 * build_form — lay the six sections out in one vertical stack, in the
 * style of Files' Preferences, inside a scroll view that only comes into
 * play when the form is taller than tasks_look_settings_max_h.
 */
- (void)
build_form
{
    NSArray<NSArray<NSView *> *> *sections =
        @[[self appearance_rows], [self new_task_rows], [self notification_rows],
          [self database_rows], [self backup_rows], [self gcal_rows]];
    NSMutableArray<NSView *> *rows = [NSMutableArray array];
    for (NSArray<NSView *> *section in sections) [rows addObjectsFromArray:section];

    NSStackView *form = [NSStackView stackViewWithViews:rows];
    form.orientation  = NSUserInterfaceLayoutOrientationVertical;
    form.alignment    = NSLayoutAttributeLeading;
    form.spacing      = tasks_look_form_spacing;
    form.edgeInsets   = NSEdgeInsetsMake(tasks_look_form_margin, tasks_look_form_margin,
                                         tasks_look_form_margin, tasks_look_form_margin);
    for (NSArray<NSView *> *section in sections) {
        [form setCustomSpacing:heading_gap afterView:section.firstObject];
        if (section != sections.lastObject) {
            [form setCustomSpacing:tasks_look_section_gap afterView:section.lastObject];
        }
    }

    /* The form has one width, the window's, and is pinned by its top-left
     * corner only: its height is its content's, and fit_window sizes the
     * document and the window to that. */
    [form.widthAnchor constraintEqualToConstant:tasks_look_settings_w].active = YES;
    TasksSettingsForm *document = [[TasksSettingsForm alloc] initWithFrame:NSZeroRect];
    form.translatesAutoresizingMaskIntoConstraints = NO;
    [document addSubview:form];
    [form.leadingAnchor constraintEqualToAnchor:document.leadingAnchor].active = YES;
    [form.topAnchor constraintEqualToAnchor:document.topAnchor].active         = YES;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.drawsBackground     = NO;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.documentView        = document;

    self.form               = form;
    self.scroll             = scroll;
    self.window.contentView = scroll;
}

/*
 * fit_window — size the window to the form: its full height up to
 * tasks_look_settings_max_h, beyond which the form scrolls and the window
 * is widened by the scroller so that nothing runs under it.  The top edge
 * stays where it is, so the window grows and shrinks away from its title
 * bar.  Called after anything that changes how many lines the form has.
 */
- (void)
fit_window
{
    [self.form layoutSubtreeIfNeeded];
    NSSize fit     = self.form.fittingSize;
    BOOL   scrolls = fit.height > tasks_look_settings_max_h;
    NSSize visible = NSMakeSize(tasks_look_settings_w,
                                scrolls ? tasks_look_settings_max_h : fit.height);
    NSSize content = [NSScrollView frameSizeForContentSize:visible
                                   horizontalScrollerClass:Nil
                                     verticalScrollerClass:scrolls ? NSScroller.class : Nil
                                                borderType:self.scroll.borderType
                                               controlSize:NSControlSizeRegular
                                             scrollerStyle:self.scroll.scrollerStyle];

    self.scroll.documentView.frame = NSMakeRect(0, 0, tasks_look_settings_w, fit.height);

    NSWindow *window = self.window;
    NSRect    rect   = [window contentRectForFrameRect:window.frame];
    CGFloat   top    = NSMaxY(rect);
    rect.size        = content;
    rect.origin.y    = top - content.height;
    [window setFrame:[window frameRectForContentRect:rect] display:YES];
}

/* ── Appearance ──────────────────────────────────────────────────────── */

/*
 * toggle_option: — an Appearance checkbox changed: write its setting and
 * have the task pane redraw.  The panes re-read their settings on every
 * refresh (the Kanban shadow included), and the same event reloads the
 * day planner and the reminders, so the one event is all it takes.
 *
 * Inputs:
 *   sender — the checkbox; its tag is its index in appearance_options
 */
- (void)
toggle_option:(NSButton *)sender
{
    task_config_set_bool(appearance_options[sender.tag].key,
                         sender.state == NSControlStateValueOn);
    task_app_notify_tasks(self.app);
}

/* ── Database ────────────────────────────────────────────────────────── */

/*
 * refresh_facts — the plate's lines that are known without reading the
 * file: the path, the counts, and the health verdict the database handle
 * holds.  The verdict is READ, never made, here: a plate that ran a check
 * every time it was drawn would be a check nobody asked for.
 */
- (void)
refresh_facts
{
    TaskDatabase *db = self.app->db;
    gint n_tasks = 0;
    gint n_tags  = 0;

    self.path_label.stringValue = tasks_string(shared_db_file(db->shared));

    task_db_totals(db, &n_tasks, &n_tags);
    self.data_label.stringValue =
        [NSString stringWithFormat:@"%d task%s with %d tag%s",
            n_tasks, n_tasks == 1 ? "" : "s", n_tags, n_tags == 1 ? "" : "s"];

    char *summary = NULL;
    char *detail  = NULL;
    task_health_light_t light =
        task_health_describe(task_db_health(db), (int64_t)time(NULL), &summary, &detail);
    self.health_light.textColor   = light == task_health_good ? NSColor.systemGreenColor
                                  : light == task_health_bad  ? NSColor.systemRedColor
                                                              : NSColor.tertiaryLabelColor;
    self.health_label.stringValue = tasks_string(summary);
    /* The detail is sqlite's own words and can run to many lines, so it
     * lives in the tooltip: the line says WHAT, hovering says which. */
    self.health_label.toolTip     = tasks_string(detail);
    free(summary);
    free(detail);
}

/*
 * start_probe: — read the database file in the background (task_health.h)
 * and show the answer when it arrives.  Update is disabled meanwhile, so
 * one request is in flight per window; the id is what tells its answer
 * from one this window is no longer waiting for.
 *
 * Inputs:
 *   check — YES for an Update (run the integrity check too), NO for the
 *           plain read of size and digest
 */
- (void)
start_probe:(BOOL)check
{
    self.awaited_probe         = ++last_probe_id;
    self.checking              = check;
    self.update_button.enabled = NO;
    if (check) {
        /* The verdict on show is about to be replaced: no light for it. */
        self.health_light.textColor   = NSColor.tertiaryLabelColor;
        self.health_label.stringValue = @"Checking…";
    }
    task_health_probe_start(self.app, check, on_probe, (void *)self.awaited_probe);
}

/*
 * refresh_database — fill the plate for the database the app has open: the
 * lines known at once, then the file's size and digest from a background
 * read (no integrity check: that is what Update is for).
 */
- (void)
refresh_database
{
    [self refresh_facts];
    [self start_probe:NO];
}

/*
 * show_probe: — a probe this window was waiting for has reported: renew
 * every line of the plate.  After an Update the outcome also goes to the
 * status bar, because a check that comes back clean changes almost
 * nothing visible and would otherwise look like a button that does nothing.
 *
 * Inputs:
 *   probe — what was found (borrowed, valid during the call)
 */
- (void)
show_probe:(const task_health_probe_t *)probe
{
    BOOL checked = self.checking;

    self.awaited_probe         = 0;
    self.checking              = NO;
    self.update_button.enabled = YES;

    /* The app opened another database while the probe ran: its answer is
     * about a file no longer on show.  Describe the one that is. */
    if (!probe->current) {
        [self refresh_database];
        return;
    }

    [self refresh_facts];
    self.size_label.stringValue = probe->size >= 0
        ? [NSByteCountFormatter stringFromByteCount:probe->size
                                         countStyle:NSByteCountFormatterCountStyleFile]
        : @"unknown";
    [self show_sha:probe->sha256 != NULL ? tasks_string(probe->sha256) : nil];
    [self fit_window];

    const TaskDbHealth *health = task_db_health(self.app->db);
    if (checked && health != NULL) {
        task_app_status(self.app, "%s",
                        health->ok  ? "Database check passed"
                      : health->ran ? "Database check found problems"
                                    : "Database check did not complete");
    }
}

/*
 * show_sha: — show a digest: the short form on the plate, the whole of it
 * in the tooltip, and kept for copy_sha:.
 *
 * The digest is of the file as it stands, hashed when asked rather than
 * stored anywhere: it is the only form of it that can be compared with
 * `shasum -a 256` or with a backup.
 *
 * Inputs:
 *   sha — the hex digest, or nil when the file could not be read
 */
- (void)
show_sha:(NSString *)sha
{
    BOOL readable = sha.length > sha_head + sha_tail;

    self.sha256          = readable ? sha : nil;
    self.sha_copy.hidden = !readable;
    if (!readable) {
        self.sha_label.stringValue = @"—";
        self.sha_label.toolTip     = @"The database file could not be read.";
        return;
    }
    self.sha_label.stringValue = [NSString stringWithFormat:@"%@…%@",
        [sha substringToIndex:sha_head], [sha substringFromIndex:sha.length - sha_tail]];
    self.sha_label.toolTip = [NSString stringWithFormat:
        @"%@\n\nThe file as it stands. A database in use changes with the next edit, so this "
        @"moves.", sha];
}

/*
 * copy_sha: — the digest's "copy" link: copy the whole of it.  The copy is
 * the digest the plate shows, not a fresh hash of a file that has moved on.
 *
 * Inputs:
 *   sender — the link (unused)
 */
- (void)
copy_sha:(id)sender
{
    if (!self.sha256) return;
    copy_to_pasteboard(self.sha256);
    task_app_status(self.app, "SHA-256 copied to the clipboard");
}

/*
 * copy_path: — the path's "copy" link: copy the database file's path.
 *
 * Inputs:
 *   sender — the link (unused)
 */
- (void)
copy_path:(id)sender
{
    copy_to_pasteboard(tasks_string(shared_db_file(self.app->db->shared)));
    task_app_status(self.app, "Database path copied to the clipboard");
}

/*
 * update_database: — Update: run the integrity check and renew every line
 * of the plate, not just the verdict.  The plate is one statement about
 * one file; the counts and the size go stale on their own with every edit
 * made in the library, so they are exactly what someone presses this for.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
update_database:(id)sender
{
    [self start_probe:YES];
}

/* ── Backups ─────────────────────────────────────────────────────────── */

/*
 * show_number:field:stepper: — bring a field and its stepper to one value,
 * held to the stepper's range.
 *
 * Inputs:
 *   value   — the value wanted (a typed one may be out of range)
 *   field   — the pair's field
 *   stepper — the pair's stepper, which holds the range
 *
 * Output:
 *   the value shown.
 */
- (NSInteger)
show_number:(NSInteger)value field:(NSTextField *)field stepper:(NSStepper *)stepper
{
    NSInteger shown = MAX((NSInteger)stepper.minValue, MIN((NSInteger)stepper.maxValue, value));

    field.integerValue   = shown;
    stepper.integerValue = shown;
    return shown;
}

/*
 * refresh_backup — show the backup settings: the switch, which controls it
 * leaves usable, where the copies land, when the last backup was made and
 * why the last pass failed — each line on its own, since a copy can be
 * made by a pass that then failed to prune.
 *
 * The folder line names the RESOLVED folder in every case, from the same
 * call every backup pass asks first (task_backup_dir), so the line cannot
 * promise a folder the copies do not go to — and when that call refuses,
 * the line says why.  A folder that is, or is inside, the database's own
 * gets the warning: those copies are real backups but cannot survive
 * losing that folder.
 */
- (void)
refresh_backup
{
    BOOL on = [self policy:SHARED_SETTING_BACKUP_ENABLED] != 0;

    self.backup_box.state         = on ? NSControlStateValueOn : NSControlStateValueOff;
    self.interval_field.enabled   = on;
    self.interval_stepper.enabled = on;
    self.keep_field.enabled       = on;
    self.keep_stepper.enabled     = on;
    self.choose_button.enabled    = on;
    self.backup_button.enabled    = on;

    char     *why    = NULL;
    char     *dir    = task_backup_dir(self.app, &why);
    NSString *db_dir = [tasks_string(shared_db_file(self.app->db->shared))
                        stringByDeletingLastPathComponent];
    self.folder_label.stringValue = dir != NULL
        ? [NSString stringWithFormat:@"Backing up to %@", tasks_string(dir)]
        : tasks_string(why);
    self.fate_label.hidden = dir == NULL ||
                             !task_path_shares_fate(dir, db_dir.fileSystemRepresentation);
    free(dir);
    free(why);

    NSInteger last_at = [self policy:SHARED_SETTING_BACKUP_LAST_AT];
    self.last_label.hidden      = last_at <= 0;
    self.last_label.stringValue = last_at <= 0 ? @"" :
        [NSString stringWithFormat:@"Last backup: %@",
            [NSDateFormatter localizedStringFromDate:
                [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)last_at]
                                           dateStyle:NSDateFormatterMediumStyle
                                           timeStyle:NSDateFormatterShortStyle]];

    char *failure = NULL;
    shared_settings_get(self.app->db->shared, SHARED_SETTING_BACKUP_LAST_ERROR, &failure, NULL);
    self.error_label.hidden      = failure == NULL || failure[0] == '\0';
    self.error_label.stringValue = self.error_label.hidden
        ? @"" : [NSString stringWithFormat:@"The last backup pass failed: %@", tasks_string(failure)];
    free(failure);
}

/*
 * policy: — one backup-policy value as the shared module applies it
 * (shared_backup_policy_get: the stored value, else its default, held to
 * its range), so the window shows exactly what the backups obey.
 *
 * Inputs:
 *   key — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN, _KEEP or _LAST_AT
 *
 * Output:
 *   the value (the module's default when it cannot be read).
 */
- (NSInteger)
policy:(const char *)key
{
    int64_t value = 0;
    shared_backup_policy_get(self.app->db->shared, key, &value, NULL);
    return (NSInteger)value;
}

/*
 * store_policy:value: — write one backup-policy value to shared_settings,
 * where both apps read it; a write that fails is said under the buttons.
 *
 * Inputs:
 *   key   — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN or _KEEP
 *   value — the new value
 */
- (void)
store_policy:(const char *)key value:(NSInteger)value
{
    char *why = NULL;
    if (shared_settings_set(self.app->db->shared, key, @(value).stringValue.UTF8String,
                            &why) != 0)
        [self show_backup_status:[NSString stringWithFormat:@"The setting was not saved: %@",
                                  tasks_string(why)]];
    free(why);
}

/*
 * toggle_backup: — the master switch: write it to the shared policy.  No
 * folder is asked for: the backup has a default destination, so switching
 * this on always does something; the poll picks the change up by itself.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
toggle_backup:(NSButton *)sender
{
    [self store_policy:SHARED_SETTING_BACKUP_ENABLED value:sender.state == NSControlStateValueOn];
    [self refresh_backup];
}

/*
 * interval_changed: — the interval's field or stepper changed: write the
 * cadence to the shared policy, which the next poll applies.
 *
 * Inputs:
 *   sender — the field or the stepper
 */
- (void)
interval_changed:(NSControl *)sender
{
    NSInteger minutes = [self show_number:sender.integerValue
                                    field:self.interval_field
                                  stepper:self.interval_stepper];

    /* Leaving the field without changing it also lands here, and need not
     * write the database. */
    if (minutes == [self policy:SHARED_SETTING_BACKUP_INTERVAL_MIN]) return;
    [self store_policy:SHARED_SETTING_BACKUP_INTERVAL_MIN value:minutes];
}

/*
 * keep_changed: — the retention field or stepper changed: write the bound
 * to the shared policy; the module reads it at every backup.
 *
 * Inputs:
 *   sender — the field or the stepper
 */
- (void)
keep_changed:(NSControl *)sender
{
    NSInteger keep = [self show_number:sender.integerValue
                                 field:self.keep_field
                               stepper:self.keep_stepper];

    if (keep == [self policy:SHARED_SETTING_BACKUP_KEEP]) return;
    [self store_policy:SHARED_SETTING_BACKUP_KEEP value:keep];
}

/*
 * choose_folder: — Change Folder: pick the backup destination.  The panel
 * opens where backups go NOW (the resolved folder, default included), so
 * that re-picking from wherever the panel last was is not how backups end
 * up in two places.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
choose_folder:(id)sender
{
    char *why     = NULL;
    char *current = task_backup_dir(self.app, &why);
    __weak TasksSettingsWindow *ws = self;

    shared_ui_pick_path(SharedPickFolder, @"Choose Backup Folder", @"Select", nil,
                        tasks_string(current), ^(NSString *path) {
        TasksSettingsWindow *s = ws;
        if (!s || s.is_shut_down || !path) return;   /* closed meanwhile, or cancelled */
        [s use_backup_folder:path];
    });
    free(current);
    free(why);
}

/*
 * use_backup_folder: — make a chosen folder the backup destination, if the
 * core will use it.  task_backup_set_dir is the judge: a folder it refuses
 * — one inside the live Tasks data directory above all, which this app
 * never touches — is not kept, and its reason is shown as given.
 *
 * Inputs:
 *   path — the chosen folder
 */
- (void)
use_backup_folder:(NSString *)path
{
    char *reason = NULL;

    if (task_backup_set_dir(self.app, path.fileSystemRepresentation, &reason)) {
        [self refresh_backup];
        [self fit_window];
    } else {
        shared_ui_notice(self.window, @"Backups cannot go to that folder",
                        tasks_string(reason), nil);
    }
    free(reason);
}

/*
 * back_up_now: — Back Up Now: one pass, reported under the buttons (and by
 * the core in the status bar).  Also the only way to exercise the feature
 * without waiting for the timer.
 *
 * A pass already in flight is said here, because the core starts no
 * second one and would call nothing back.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
back_up_now:(id)sender
{
    if (self.app->backup_running) {
        [self show_backup_status:@"A backup is already running"];
        return;
    }
    [self show_backup_status:@"Backing up…"];
    task_backup_start(self.app, on_backup_done, NULL);
}

/*
 * show_backup_status: — show a line under the backup buttons: a pass's
 * progress or its outcome.
 *
 * Inputs:
 *   text — the line
 */
- (void)
show_backup_status:(NSString *)text
{
    self.backup_status.stringValue = text;
    self.backup_status.hidden      = NO;
    [self fit_window];
}

/* ── Teardown ────────────────────────────────────────────────────────── */

/*
 * shut_down — the window is going away: commit a number still being typed
 * (ending the edit sends the field's action), and stop waiting for any
 * probe, whose answer would otherwise be shown in a closed window.
 * Idempotent.
 */
- (void)
shut_down
{
    if (self.is_shut_down) return;
    [self.window makeFirstResponder:nil];
    self.is_shut_down  = YES;
    self.awaited_probe = 0;
    task_app_unlisten(self.app, self.gcal_listener);
    self.gcal_listener = 0;
}

/*
 * windowWillClose: — the window is closing: shut down, then drop the
 * module's reference, which releases this controller.  That is the last
 * thing done here, because nothing else keeps `self` alive afterwards.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [self shut_down];
    settings_window = nil;
}

@end

/*
 * tasks_settings_window_open — show or raise the Settings window.
 * Inputs are documented in settings_window.h.
 */
void
tasks_settings_window_open(TaskApp *app)
{
    BOOL opening = settings_window == nil;

    if (opening) settings_window = [[TasksSettingsWindow alloc] init_with_app:app];
    [settings_window showWindow:nil];
    /* A new window gives its first field the focus, with its text
     * highlighted; nothing is being edited yet. */
    if (opening) [settings_window.window makeFirstResponder:nil];
}
