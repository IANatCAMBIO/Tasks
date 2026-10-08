#import "settings_window.h"
#import "look.h"
#import "ui_util.h"
#import "task_notify.h"

#include "backup.h"
#include "task_config.h"
#include "task_health.h"
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

/* The backup schedule's controls.  The interval is in minutes, 0 meaning
 * "only when Back Up Now is pressed"; a week is the longest offered. */
static const NSInteger backup_interval_max  = 10080;
static const NSInteger backup_interval_step = 15;
static const CGFloat   interval_field_w     = 56.0;  /* five digits  */
static const CGFloat   keep_field_w         = 44.0;  /* three digits */
static const CGFloat   stepper_gap          = 2.0;   /* a field to its stepper */

/* ── The Appearance options ─────────────────────────────────────────── */

/* One on/off setting: a checkbox that writes a bool key.  All of them
 * change what the task pane draws, and the panes re-read their settings on
 * every refresh, so one action serves the whole table. */
typedef struct {
    NSString * __unsafe_unretained title;
    const char                    *key;      /* a TASK_CFG_* bool key */
    NSString * __unsafe_unretained tooltip;  /* nil for none          */
} option_spec_t;

static const option_spec_t appearance_options[] = {
    { @"Show task titles in bold",          TASK_CFG_BOLD_TITLES,   nil },
    { @"Show drop shadows on Kanban cards", TASK_CFG_KANBAN_SHADOW,
      @"Lifts each card off its lane." },
    { @"Include all past-due tasks in the Due Today view", TASK_CFG_DUE_TODAY_OVERDUE, nil },
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

@interface TasksSettingsWindow : NSWindowController <NSWindowDelegate>
@property (nonatomic, assign) TaskApp *app;              /* borrowed; outlives the window */
@property (nonatomic, strong) NSScrollView *scroll;      /* the window's content          */
@property (nonatomic, strong) NSStackView *form;         /* every row, top to bottom      */

/* The Database plate: five facts about one file, refreshed together. */
@property (nonatomic, strong) NSTextField *health_light; /* the round indicator           */
@property (nonatomic, strong) NSTextField *health_label; /* the verdict and when          */
@property (nonatomic, strong) NSTextField *path_label;
@property (nonatomic, strong) NSTextField *data_label;   /* "42 tasks in 8 lists"         */
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
@property (nonatomic, strong) NSButton *choose_button;
@property (nonatomic, strong) NSButton *backup_button;
@property (nonatomic, strong) NSTextField *backup_status; /* the last Back Up Now's outcome;
                                                           * hidden until there is one    */

/* Notifications. */
@property (nonatomic, strong) NSButton *notif_tasks_btn;   /* radio: Tasks popup  */
@property (nonatomic, strong) NSButton *notif_native_btn;  /* radio: macOS Notif. */

@property (nonatomic, assign) BOOL is_shut_down;

/* Declared here because the C callbacks below call them before their
 * definitions; documented where they are defined. */
- (void)show_probe:(const task_health_probe_t *)probe;
- (void)show_backup_status:(NSString *)text;
@end

/* The one Settings window while it is open; nil otherwise.  This reference
 * is what owns the controller. */
static TasksSettingsWindow *settings_window;

/* Probe ids count up and are never reused, so an answer can be matched to
 * the window, and the request, that asked for it. */
static NSUInteger last_probe_id;

/* ── Core callbacks ─────────────────────────────────────────────────── */

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
 *   message — its one-line outcome (borrowed); NULL from a pass that found
 *             nothing new to copy
 *   data    — unused
 */
static void
on_backup_done(TaskApp *app, gboolean ok, const gchar *message, gpointer data)
{
    NSString *text = message != NULL ? tasks_string(message)
                   : ok              ? @"Nothing has changed since the last backup"
                                     : @"Backup failed";

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
        @"against this database, then re-count its tasks and lists and re-read its size and "
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
    self.interval_stepper = number_stepper(0, backup_interval_max, backup_interval_step,
                                           self, @selector(interval_changed:));
    self.keep_field       = number_field(keep_field_w, self, @selector(keep_changed:));
    self.keep_stepper     = number_stepper(TASK_BACKUP_KEEP_MIN, TASK_BACKUP_KEEP_MAX, 1,
                                           self, @selector(keep_changed:));

    self.interval_field.toolTip =
        @"Minutes between backups. 0 backs up only when you press Back Up Now. A pass whose "
        @"database has not changed since the last backup writes nothing.";
    self.keep_field.toolTip =
        @"How many backup files to retain. The oldest are removed once a new backup has been "
        @"verified, never before.";

    [self show_number:task_config_get_int(TASK_CFG_BACKUP_INTERVAL_MIN)
                field:self.interval_field
              stepper:self.interval_stepper];
    [self show_number:task_config_get_int(TASK_CFG_BACKUP_KEEP)
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
 * the database), the two buttons that answer that line, and the outcome of
 * the last Back Up Now.
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
        @"Writes a verified copy of the database into a folder of your choice on a timer, "
        @"keeping only the most recent few. Worth pointing at a disk independent of wherever "
        @"the database itself lives, so one mishap cannot take both.";

    self.folder_label  = wrapping_label(dim, text_w);
    self.fate_label    = wrapping_label(dim, text_w);
    self.backup_status = wrapping_label(dim, text_w);
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
             self.folder_label, self.fate_label,
             button_row(@[self.choose_button, self.backup_button]), self.backup_status];
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

/*
 * build_form — lay the four sections out in one vertical stack, in the
 * style of Files' Preferences, inside a scroll view that only comes into
 * play when the form is taller than tasks_look_settings_max_h.
 */
- (void)
build_form
{
    NSArray<NSArray<NSView *> *> *sections =
        @[[self appearance_rows], [self notification_rows],
          [self database_rows], [self backup_rows]];
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
 * refresh (the Kanban shadow included), so the one event is all it takes.
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
    gint n_lists = 0;

    self.path_label.stringValue = tasks_string(db->path);

    task_db_totals(db, &n_tasks, &n_lists);
    self.data_label.stringValue =
        [NSString stringWithFormat:@"%d task%s in %d list%s",
            n_tasks, n_tasks == 1 ? "" : "s", n_lists, n_lists == 1 ? "" : "s"];

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
    copy_to_pasteboard(tasks_string(self.app->db->path));
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
 * leaves usable, and where the copies land.
 *
 * The folder line names the RESOLVED folder in every case, from the same
 * call the backup pass uses, so the line cannot promise a folder the
 * copies do not go to.  A folder that is, or is inside, the database's own
 * gets the warning: those copies are real backups but cannot survive
 * losing that folder — and it is the default, so saying nothing would let
 * the arrangement most in need of changing look like the one nobody need
 * think about.
 */
- (void)
refresh_backup
{
    BOOL on = task_config_get_bool(TASK_CFG_BACKUP_ENABLED);

    self.backup_box.state         = on ? NSControlStateValueOn : NSControlStateValueOff;
    self.interval_field.enabled   = on;
    self.interval_stepper.enabled = on;
    self.keep_field.enabled       = on;
    self.keep_stepper.enabled     = on;
    self.choose_button.enabled    = on;
    self.backup_button.enabled    = on;

    gchar    *dir    = task_backup_dir();
    NSString *db_dir = [tasks_string(self.app->db->path) stringByDeletingLastPathComponent];
    self.folder_label.stringValue =
        [NSString stringWithFormat:@"Backing up to %@", tasks_string(dir)];
    self.fate_label.hidden = !task_path_shares_fate(dir, db_dir.fileSystemRepresentation);
    g_free(dir);
}

/*
 * toggle_backup: — the master switch: write it and re-arm the timer.  No
 * folder is asked for: the backup has a default destination, so switching
 * this on always does something.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
toggle_backup:(NSButton *)sender
{
    task_config_set_bool(TASK_CFG_BACKUP_ENABLED, sender.state == NSControlStateValueOn);
    task_backup_auto_start(self.app, self.app->db->path);
    [self refresh_backup];
}

/*
 * interval_changed: — the interval's field or stepper changed: write the
 * cadence and re-arm the timer with it.
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

    /* Leaving the field without changing it also lands here, and must not
     * restart the wait for the next pass. */
    if (minutes == task_config_get_int(TASK_CFG_BACKUP_INTERVAL_MIN)) return;
    task_config_set_int(TASK_CFG_BACKUP_INTERVAL_MIN, (gint)minutes);
    task_backup_auto_start(self.app, self.app->db->path);
}

/*
 * keep_changed: — the retention field or stepper changed: write the bound.
 * The timer is NOT re-armed: the cadence has not moved, re-arming would
 * restart the wait for the next pass, and the bound is read afresh at the
 * start of every pass.
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

    task_config_set_int(TASK_CFG_BACKUP_KEEP, (gint)keep);
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
    gchar *current = task_backup_dir();
    __weak TasksSettingsWindow *ws = self;

    tasks_ui_pick_path(self.window, TasksPickFolder, @"Choose Backup Folder", @"Select", nil,
                       tasks_string(current), ^(NSString *path) {
        TasksSettingsWindow *s = ws;
        if (!s || s.is_shut_down || !path) return;   /* closed meanwhile, or cancelled */
        [s use_backup_folder:path];
    });
    g_free(current);
}

/*
 * use_backup_folder: — make a chosen folder the backup destination, if the
 * core will use it.  task_backup_ready is the judge: a folder it refuses —
 * one inside the live Tasks data directory above all, which this build
 * never touches — is not kept, and its reason is shown as given.
 *
 * Inputs:
 *   path — the chosen folder
 */
- (void)
use_backup_folder:(NSString *)path
{
    gchar *previous = task_config_get(TASK_CFG_BACKUP_DIR);
    gchar *reason   = NULL;

    task_config_set(TASK_CFG_BACKUP_DIR, path.fileSystemRepresentation);
    if (task_backup_ready(&reason)) {
        task_backup_auto_start(self.app, self.app->db->path);
        [self refresh_backup];
        [self fit_window];
    } else {
        task_config_set(TASK_CFG_BACKUP_DIR, previous);
        tasks_ui_notice(self.window, @"Backups cannot go to that folder",
                        tasks_string(reason), nil);
    }
    g_free(reason);
    g_free(previous);
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
    task_backup_start(self.app, self.app->db->path, on_backup_done, NULL);
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
