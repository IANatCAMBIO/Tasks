#import "editor_window.h"
#import "look.h"
#import "ui_util.h"

#include "recur.h"
#include "task_dates.h"
#include "task_edit.h"
#include "task_paths.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string.h>
#include <time.h>

/* ── Timing ─────────────────────────────────────────────────────────── */

/* A text edit is saved this long after the last keystroke. */

/* How long the window takes to grow or shrink round a fold. */
static const NSTimeInterval fold_duration = 0.2;

/* ── Layout ─────────────────────────────────────────────────────────── */

static const CGFloat status_popup_w = 118.0;    /* fits "In Progress", so the
                                                 * row does not move when the
                                                 * selection changes        */
static const CGFloat date_field_w   = 100.0;    /* "YYYY-MM-DD"             */
static const CGFloat time_field_w   = 56.0;     /* "HH:MM"                  */
static const CGFloat count_field_w  = 46.0;     /* three digits             */
static const CGFloat row_gap        = 8.0;      /* between a row's controls */
static const CGFloat foot_gap       = 12.0;     /* the form to the foot row */
static const CGFloat notes_inset    = 4.0;      /* text to the notes border */
static const CGFloat list_h         = 100.0;    /* a subtask or attachment
                                                 * list                     */
static const CGFloat list_row_h     = 20.0;
static const CGFloat list_icon      = 16.0;     /* an attachment's file icon */
static const CGFloat list_button_w  = 78.0;     /* the buttons beside a list */
static const CGFloat cell_pad       = 2.0;      /* row edge to its content   */
static const CGFloat cell_gap       = 4.0;      /* checkbox or icon to text  */

/* The form's width inside the window's margins; wrapped labels wrap here. */
static const CGFloat form_w = tasks_look_editor_w - 2 * tasks_look_form_margin;

/* How strongly the form's foot follows the window's.  Below the priority at
 * which a window keeps its size (so the form never resizes the window: the
 * folds do that, in refit_animated:) and above every hugging priority (so a
 * window dragged taller stretches the notes). */
static const NSLayoutPriority form_follows_window = NSLayoutPriorityWindowSizeStayPut - 1;

/* The notes' preferred height gives way first when the window is taller. */
static const NSLayoutPriority notes_height_preferred = NSLayoutPriorityDefaultLow - 1;

/* ── Names ──────────────────────────────────────────────────────────── */

static NSString * const subtask_cell_id    = @"subtask";
static NSString * const attachment_cell_id = @"attachment";

/* The checkbox inside a subtask cell, found again with viewWithTag:. */
static const NSInteger subtask_check_tag = 1;

/* The Advanced link's two faces: the arrow says what a click will do. */
static NSString * const advanced_to_show = @"Advanced ▾";
static NSString * const advanced_to_fold = @"Advanced ▴";

/* ── View factories ─────────────────────────────────────────────────── */

/*
 * form_label — a short non-editable label for a form row, at the same font
 * size as the list and entry fields.
 *
 * Inputs:
 *   title — the label text
 *
 * Output:
 *   new label.
 */
static NSTextField *
form_label(NSString *title)
{
    NSTextField *label = [NSTextField labelWithString:title];

    label.font = [NSFont systemFontOfSize:shared_look_ui_font];
    return label;
}

/*
 * entry_field — a one-line editing field of a fixed width: the stock
 * rounded bezel, with the system focus ring.
 *
 * Inputs:
 *   placeholder — shown while the field is empty
 *   width       — the field's width
 *   tip         — its tooltip
 *   delegate    — hears its edits (the editor)
 *
 * Output:
 *   new field.
 */
static NSTextField *
entry_field(NSString *placeholder, CGFloat width, NSString *tip, id<NSTextFieldDelegate> delegate)
{
    NSTextField *field = [NSTextField textFieldWithString:@""];

    field.font               = [NSFont systemFontOfSize:shared_look_ui_font];
    field.bezelStyle         = NSTextFieldRoundedBezel;
    field.placeholderString  = placeholder;
    field.toolTip            = tip;
    field.delegate           = delegate;
    field.usesSingleLineMode = YES;
    [field.widthAnchor constraintEqualToConstant:width].active = YES;
    return field;
}

/*
 * row_stack — controls laid out left to right, centred on one line.
 *
 * Inputs:
 *   views — the controls, in order; a spacer() among them takes the spare
 *           width
 *
 * Output:
 *   new horizontal stack.
 */
static NSStackView *
row_stack(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];

    stack.orientation  = NSUserInterfaceLayoutOrientationHorizontal;
    stack.distribution = NSStackViewDistributionFill;
    stack.spacing      = row_gap;
    return stack;
}

/*
 * column_stack — an empty top-to-bottom stack of full-width rows; fill it
 * with add_row.
 *
 * Inputs:
 *   spacing — the gap between rows
 *
 * Output:
 *   new vertical stack.
 */
static NSStackView *
column_stack(CGFloat spacing)
{
    NSStackView *stack = [[NSStackView alloc] initWithFrame:NSZeroRect];

    stack.orientation  = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment    = NSLayoutAttributeLeading;
    stack.distribution = NSStackViewDistributionFill;
    stack.spacing      = spacing;
    return stack;
}

/*
 * add_row — append a row to a column_stack, as wide as the stack.
 *
 * Inputs:
 *   stack — the column
 *   row   — the view to add
 */
static void
add_row(NSStackView *stack, NSView *row)
{
    [stack addArrangedSubview:row];
    [row.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active = YES;
}

/*
 * spacer — an empty view that takes a row's spare width, pushing what
 * follows it to the right edge.
 *
 * Output:
 *   new view.
 */
static NSView *
spacer(void)
{
    NSView *view = [[NSView alloc] initWithFrame:NSZeroRect];

    [view setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    return view;
}

/*
 * side_button — one of the small buttons beside a list.  They share a
 * width, so the two lists' button columns line up.
 *
 * Inputs:
 *   title  — the button's title
 *   target — the editor
 *   action — its action
 *
 * Output:
 *   new button.
 */
static NSButton *
side_button(NSString *title, id target, SEL action)
{
    NSButton *button = [NSButton buttonWithTitle:title target:target action:action];

    button.controlSize = NSControlSizeSmall;
    button.font        = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    [button.widthAnchor constraintEqualToConstant:list_button_w].active = YES;
    return button;
}

/*
 * calendar_button — the button beside a date field that opens the calendar.
 *
 * Inputs:
 *   target — the editor
 *   action — its action
 *   tip    — the tooltip, which is also what the button is called for
 *            accessibility (it has no title)
 *
 * Output:
 *   new borderless image button.
 */
static NSButton *
calendar_button(id target, SEL action, NSString *tip)
{
    NSImage  *image  = [NSImage imageWithSystemSymbolName:@"calendar" accessibilityDescription:tip];
    NSButton *button = [NSButton buttonWithImage:image target:target action:action];

    button.bordered = NO;
    button.toolTip  = tip;
    return button;
}

/*
 * popup_button — a pop-up menu of fixed choices.
 *
 * Inputs:
 *   titles — the choices, in index order
 *   target — the editor
 *   action — sent when the selection changes
 *
 * Output:
 *   new pop-up button, first choice selected.
 */
static NSPopUpButton *
popup_button(NSArray<NSString *> *titles, id target, SEL action)
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];

    popup.font   = [NSFont systemFontOfSize:shared_look_ui_font];
    popup.target = target;
    popup.action = action;
    [popup addItemsWithTitles:titles];
    return popup;
}

/*
 * count_stepper — the stepper beside a count field.  It holds the count's
 * last valid value: the field may show half-typed text, the stepper never
 * does.
 *
 * Inputs:
 *   min, max — the range
 *   target   — the editor
 *   action   — sent when the arrows change the value
 *
 * Output:
 *   new stepper.
 */
static NSStepper *
count_stepper(int min, int max, id target, SEL action)
{
    NSStepper *stepper = [[NSStepper alloc] initWithFrame:NSZeroRect];

    stepper.minValue   = min;
    stepper.maxValue   = max;
    stepper.increment  = 1;
    stepper.valueWraps = NO;
    stepper.target     = target;
    stepper.action     = action;
    return stepper;
}

/*
 * list_scroll — put a one-column list table in a bordered scroll view of
 * the lists' fixed height.
 *
 * Inputs:
 *   table    — the table, newly created
 *   delegate — its data source and delegate (the editor)
 *
 * Output:
 *   the scroll view holding it.
 */
static NSScrollView *
list_scroll(NSTableView *table, id<NSTableViewDataSource, NSTableViewDelegate> delegate)
{
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"item"];

    column.resizingMask = NSTableColumnAutoresizingMask;
    [table addTableColumn:column];
    table.headerView              = nil;
    table.style                   = NSTableViewStylePlain;
    table.rowHeight               = list_row_h;
    table.allowsEmptySelection    = YES;
    table.allowsMultipleSelection = NO;
    table.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;
    table.dataSource              = delegate;
    table.delegate                = delegate;

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.documentView        = table;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers  = YES;
    scroll.borderType          = NSBezelBorder;
    [scroll.heightAnchor constraintEqualToConstant:list_h].active = YES;
    return scroll;
}

/*
 * list_section — a heading over a list with its buttons at the right.
 *
 * Inputs:
 *   title   — the heading
 *   scroll  — the list (from list_scroll)
 *   buttons — the views of the button column, top to bottom
 *
 * Output:
 *   new vertical stack holding the section.
 */
static NSStackView *
list_section(NSString *title, NSScrollView *scroll, NSArray<NSView *> *buttons)
{
    NSStackView *column = [NSStackView stackViewWithViews:buttons];
    column.orientation  = NSUserInterfaceLayoutOrientationVertical;
    column.spacing      = tasks_look_form_spacing;

    NSStackView *line = row_stack(@[scroll, column]);
    line.alignment    = NSLayoutAttributeTop;

    NSStackView *section = column_stack(tasks_look_form_spacing);
    add_row(section, shared_ui_heading(title));
    add_row(section, line);
    return section;
}

/*
 * is_editing — whether a field is being edited (it has the field editor).
 *
 * Inputs:
 *   field — the field
 *
 * Output:
 *   YES while the user is typing in it.
 */
static BOOL
is_editing(NSTextField *field)
{
    return field.currentEditor != nil;
}

/*
 * show_text — put text in a field, only when it differs: rewriting a
 * field's text moves its cursor.
 *
 * Inputs:
 *   field        — the field
 *   text         — what it should show
 *   keep_editing — YES leaves a field that is being edited alone whatever
 *                  it holds (the user's typing wins until they leave it)
 */
static void
show_text(NSTextField *field, NSString *text, BOOL keep_editing)
{
    if (keep_editing && is_editing(field)) return;
    if (![field.stringValue isEqualToString:text]) field.stringValue = text;
}

/*
 * same_tasks — whether two task arrays hold the same tasks in the same
 * order (whatever their other fields say).
 *
 * Inputs:
 *   a, b — GPtrArrays of Task *; either may be NULL
 *
 * Output:
 *   YES when both exist, are equally long and agree id for id.
 */
static BOOL
same_tasks(const GPtrArray *a, const GPtrArray *b)
{
    if (a == NULL || b == NULL || a->len != b->len) return NO;

    for (guint i = 0; i < a->len; i++) {
        if (((const Task *)g_ptr_array_index(a, i))->id !=
            ((const Task *)g_ptr_array_index(b, i))->id) return NO;
    }
    return YES;
}

/*
 * same_attachments — whether two attachment arrays hold the same files in
 * the same order.
 *
 * Inputs:
 *   a, b — GPtrArrays of TaskAttachment *; either may be NULL
 *
 * Output:
 *   YES when both exist, are equally long and agree id for id and path for
 *   path.
 */
static BOOL
same_attachments(const GPtrArray *a, const GPtrArray *b)
{
    if (a == NULL || b == NULL || a->len != b->len) return NO;

    for (guint i = 0; i < a->len; i++) {
        const TaskAttachment *x = g_ptr_array_index(a, i);
        const TaskAttachment *y = g_ptr_array_index(b, i);
        if (x->id != y->id || g_strcmp0(x->path, y->path) != 0) return NO;
    }
    return YES;
}

/* ── The subtask list's two small classes ───────────────────────────── */

/*
 * TasksEditorTable — the subtask list.  A table normally makes a row's
 * text field wait for a second click on an already-selected row before it
 * edits; here a subtask's title is edited by clicking it, like any field.
 * (Selecting on mouse-down comes from SharedTable.)
 */
@interface TasksEditorTable : SharedTable
@end

@implementation TasksEditorTable

/*
 * validateProposedFirstResponder:forEvent: — let a row's controls take the
 * very click that lands on them.
 *
 * Inputs:
 *   responder — the view under the click (unused)
 *   event     — the click (unused)
 *
 * Output:
 *   YES, always.
 */
- (BOOL)
validateProposedFirstResponder:(NSResponder *)responder forEvent:(NSEvent *)event
{
    return YES;
}

@end

/*
 * TasksSubtaskField — a subtask's title field.  When it takes the keyboard
 * (a click or Tab) it selects its row, so that Remove and the move buttons
 * act on the subtask being edited and not on a row selected earlier.
 */
@interface TasksSubtaskField : NSTextField
@end

@implementation TasksSubtaskField

/*
 * becomeFirstResponder — take the keyboard, and select the row this field
 * is in.
 *
 * Output:
 *   what NSTextField answers.
 */
- (BOOL)
becomeFirstResponder
{
    BOOL became = [super becomeFirstResponder];

    for (NSView *view = self.superview; became && view != nil; view = view.superview) {
        if (![view isKindOfClass:NSTableView.class]) continue;

        NSTableView *table = (NSTableView *)view;
        NSInteger    row   = [table rowForView:self];
        if (row >= 0) {
            [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
               byExtendingSelection:NO];
        }
        break;
    }
    return became;
}

@end

/* ── The editor ─────────────────────────────────────────────────────── */

@interface TasksEditorWindow : NSWindowController <NSWindowDelegate, NSTokenFieldDelegate,
                                                   NSTextViewDelegate, NSTableViewDataSource,
                                                   NSTableViewDelegate>

@property (nonatomic, assign) TaskApp *app;        /* borrowed; outlives every editor */
@property (nonatomic, assign) int64_t  task_id;
@property (nonatomic, assign) int64_t  parent_id;  /* 0 for a top-level task          */
@property (nonatomic, assign) BOOL     is_new;     /* opened by New Task: has Cancel  */

/* The always-visible fields. */
@property (nonatomic, strong) NSTextField   *title_field;
@property (nonatomic, strong) NSTextField   *source_label;     /* "From note: …" for an
                                                                 * action item from Notes */
@property (nonatomic, strong) NSPopUpButton *status_popup;      /* index is a TaskStatus */
@property (nonatomic, strong) NSTokenField  *tag_field;         /* one token per tag      */
@property (nonatomic, strong) NSTextField   *due_field;
@property (nonatomic, strong) NSDatePicker  *due_time_field;
@property (nonatomic, strong) NSButton      *priority_check;
@property (nonatomic, strong) NSTextField   *completed_label;
@property (nonatomic, strong) NSScrollView  *notes_scroll;
@property (nonatomic, strong) NSTextView    *notes_view;

/* The Advanced fold. */
@property (nonatomic, strong) NSButton    *advanced_button;
@property (nonatomic, strong) NSStackView *advanced_box;
@property (nonatomic, assign) BOOL         advanced_shown;      /* what the fold should be */

/* Subtasks (nil for a subtask's editor: one level of nesting only). */
@property (nonatomic, strong) NSTableView *subtask_table;
@property (nonatomic, strong) NSButton    *subtask_remove;
@property (nonatomic, strong) NSButton    *subtask_up;
@property (nonatomic, strong) NSButton    *subtask_down;
@property (nonatomic, assign) GPtrArray   *subtasks;            /* Task *; owned (C memory) */

/* Attachments. */
@property (nonatomic, strong) NSTableView *attachment_table;
@property (nonatomic, strong) NSButton    *attachment_remove;
@property (nonatomic, strong) NSButton    *attachment_open;
@property (nonatomic, assign) GPtrArray   *attachments;         /* TaskAttachment *; owned  */
@property (nonatomic, copy)   NSArray<NSImage *> *attachment_icons; /* one per attachment   */

/* Recurrence.  The body is the nested fold: shown while the switch is on. */
@property (nonatomic, strong) NSButton      *recur_check;
@property (nonatomic, strong) NSStackView   *recur_body;
@property (nonatomic, strong) NSTextField   *recur_start_field;
@property (nonatomic, strong) NSTextField   *recur_time_field;
@property (nonatomic, strong) NSTextField   *every_field;
@property (nonatomic, strong) NSStepper     *every_stepper;
@property (nonatomic, strong) NSPopUpButton *unit_popup;        /* index is a TaskRecurUnit */
@property (nonatomic, strong) NSTextField   *lead_field;
@property (nonatomic, strong) NSStepper     *lead_stepper;
@property (nonatomic, strong) NSPopUpButton *lead_unit_popup;
@property (nonatomic, strong) NSTextField   *recur_summary;
@property (nonatomic, assign) int64_t        recur_next;        /* the schedule's next
                                                                 * occurrence; saved with
                                                                 * the task                 */
@property (nonatomic, assign) int            seen_every;        /* the period the lead was  */
@property (nonatomic, assign) int            seen_unit;         /* last matched against     */

/* Saving. */
@property (nonatomic, assign) BOOL       save_pending;          /* a debounced save is due  */
@property (nonatomic, assign) NSUInteger save_id;               /* supersedes queued saves  */
@property (nonatomic, assign) Task      *loaded;                /* the row as the controls
                                                                 * show it: what a save is
                                                                 * diffed against; owned   */
@property (nonatomic, assign) BOOL       discarded;             /* Cancel: save nothing more */
@property (nonatomic, assign) BOOL       is_shut_down;

/* Fitting the window to the folds. */
@property (nonatomic, assign) BOOL                is_folding;   /* a resize is animating    */
@property (nonatomic, assign) BOOL                refit_wanted; /* state moved meanwhile    */
@property (nonatomic, strong) NSLayoutConstraint *notes_hold;   /* pins the notes' height
                                                                 * while the window moves   */
@end

/*
 * editors — the registry of open editors, keyed by task id.  It is what
 * owns each controller: an entry is added when a window opens and removed
 * when it closes, which releases the controller and its window.
 *
 * Output:
 *   the shared dictionary.
 */
static NSMutableDictionary<NSNumber *, TasksEditorWindow *> *
editors(void)
{
    static NSMutableDictionary<NSNumber *, TasksEditorWindow *> *registry;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ registry = [NSMutableDictionary dictionary]; });
    return registry;
}

/* Where the next editor window goes: each opens a step down and right of
 * the one before, so a second editor does not hide the first. */
static NSPoint next_cascade;

@implementation TasksEditorWindow

/* ── Construction ───────────────────────────────────────────────────── */

/*
 * init_with_app:task:is_new: — build the editor window for a task and load
 * the task into it.  The window is sized to its content and placed, but
 * not yet shown.
 *
 * Inputs:
 *   app    — the application context
 *   task   — the task as stored (borrowed)
 *   is_new — YES when New Task has just created it: the foot row then has
 *            Cancel, which deletes the task again
 *
 * Output:
 *   the controller; the caller puts it in the registry, which owns it.
 */
- (instancetype)
init_with_app:(TaskApp *)app task:(const Task *)task is_new:(BOOL)is_new
{
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, tasks_look_editor_w, list_h)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:YES];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _app       = app;
    _task_id   = task->id;
    _parent_id = task->parent_id;
    _is_new    = is_new;
    window.delegate = self;

    [self build_layout];
    [self show_task:task];

    /* A task that already has subtasks, attachments or a schedule opens
     * with them in view; a new one opens folded. */
    self.advanced_shown = !is_new && [self has_advanced_content];
    [self refresh_advanced_button];
    [self refit_animated:NO];

    [window center];
    next_cascade = [window cascadeTopLeftFromPoint:next_cascade];
    return self;
}

/*
 * dealloc — free the two row lists and the loaded row, which this object
 * alone owns.  Nothing else: teardown is shut_down's job (windowWillClose:).
 */
- (void)
dealloc
{
    task_ptr_array_free_tasks(_subtasks);
    task_ptr_array_free_attachments(_attachments);
    task_free(_loaded);
}

/*
 * build_layout — fill the window: the form above, clipped by its container,
 * and the foot row pinned to the window's bottom edge.
 *
 * The form is NOT tied firmly to the window's bottom.  The window's height
 * is set by refit_animated: alone; while it animates, the container clips
 * the form, so a fold's content is uncovered (or covered) by the moving
 * edge and the foot row rides on it.
 */
- (void)
build_layout
{
    NSView      *content = self.window.contentView;
    NSStackView *form    = column_stack(tasks_look_form_spacing);
    NSStackView *foot    = [self build_foot];
    NSView      *body    = [[NSView alloc] initWithFrame:NSZeroRect];

    [self build_fields:form];
    self.advanced_box = column_stack(tasks_look_section_gap);
    if (self.parent_id == 0) add_row(self.advanced_box, [self build_subtasks]);
    add_row(self.advanced_box, [self build_attachments]);
    add_row(self.advanced_box, [self build_recurrence]);
    self.advanced_box.hidden = YES;
    add_row(form, self.advanced_box);
    [form setCustomSpacing:tasks_look_section_gap afterView:self.notes_scroll];

    body.clipsToBounds = YES;
    body.translatesAutoresizingMaskIntoConstraints = NO;
    form.translatesAutoresizingMaskIntoConstraints = NO;
    foot.translatesAutoresizingMaskIntoConstraints = NO;
    [body addSubview:form];
    [content addSubview:body];
    [content addSubview:foot];

    NSLayoutConstraint *form_foot = [form.bottomAnchor constraintEqualToAnchor:body.bottomAnchor];
    form_foot.priority = form_follows_window;

    CGFloat margin = tasks_look_form_margin;
    [NSLayoutConstraint activateConstraints:@[
        [content.widthAnchor constraintEqualToConstant:tasks_look_editor_w],
        [body.topAnchor constraintEqualToAnchor:content.topAnchor constant:margin],
        [body.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:margin],
        [body.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-margin],
        [body.bottomAnchor constraintEqualToAnchor:foot.topAnchor constant:-foot_gap],
        [form.topAnchor constraintEqualToAnchor:body.topAnchor],
        [form.leadingAnchor constraintEqualToAnchor:body.leadingAnchor],
        [form.trailingAnchor constraintEqualToAnchor:body.trailingAnchor],
        form_foot,
        [foot.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:margin],
        [foot.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-margin],
        [foot.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-margin],
    ]];

    /* Tab walks the controls in reading order, folds included as they open. */
    self.window.autorecalculatesKeyViewLoop = YES;
}

/*
 * build_fields: — the always-visible part of the form: title, status and
 * due date, the two flags, the notes, and for a subtask the line saying
 * whose subtask it is.
 *
 * Inputs:
 *   form — the form's column, to append to
 */
- (void)
build_fields:(NSStackView *)form
{
    self.title_field = [NSTextField textFieldWithString:@""];
    self.title_field.font               = [NSFont systemFontOfSize:shared_look_ui_font];
    self.title_field.bezelStyle         = NSTextFieldRoundedBezel;
    self.title_field.placeholderString  = @"Task title";
    self.title_field.delegate           = self;
    self.title_field.usesSingleLineMode = YES;
    add_row(form, self.title_field);

    /* Shown for a Notes action item only (show_task fills it). */
    self.source_label = shared_ui_note(@"", form_w);
    self.source_label.toolTip =
        @"This task is an action item written in Notes. Its title, completion and due date "
        @"stay in step with the line in the note.";
    self.source_label.hidden = YES;
    add_row(form, self.source_label);

    /* Status at the left, the due date and time at the right. */
    NSMutableArray<NSString *> *statuses = [NSMutableArray array];
    for (int status = 0; status < TASK_STATUS_N_VALUES; status++) {
        [statuses addObject:tasks_string(task_status_label((TaskStatus)status))];
    }
    self.status_popup = popup_button(statuses, self, @selector(status_changed:));
    [self.status_popup.widthAnchor constraintEqualToConstant:status_popup_w].active = YES;

    self.due_field = entry_field(@"YYYY-MM-DD", date_field_w,
        @"The due date, as YYYY-MM-DD or M/D/YY.  Leave it empty for no due date.", self);
    {
        NSDatePicker *tp = [[NSDatePicker alloc] initWithFrame:NSZeroRect];
        tp.datePickerStyle    = NSDatePickerStyleTextFieldAndStepper;
        tp.datePickerElements = NSDatePickerElementFlagHourMinute;
        tp.font               = [NSFont systemFontOfSize:shared_look_ui_font];
        tp.toolTip            = @"The time of day this task is due.";
        tp.target             = self;
        tp.action             = @selector(due_time_changed:);
        self.due_time_field   = tp;
    }
    NSView *status_row = row_stack(@[
        form_label(@"Status:"), self.status_popup, spacer(),
        form_label(@"Due:"), self.due_field,
        calendar_button(self, @selector(pick_due_date:), @"Pick a due date"),
        form_label(@"at"), self.due_time_field,
    ]);
    add_row(form, status_row);

    /* The tags, as tokens: typing offers the existing tags (completions),
     * Return or a comma makes the typed text a tag, new or not. */
    self.tag_field = [[NSTokenField alloc] initWithFrame:NSZeroRect];
    self.tag_field.font              = [NSFont systemFontOfSize:shared_look_ui_font];
    self.tag_field.bezelStyle        = NSTextFieldRoundedBezel;
    self.tag_field.placeholderString = @"Add a tag";
    self.tag_field.toolTip           =
        @"Type to find a tag; pick one from the list, or press Return to make a new one. "
        @"A task without a tag is listed under All Tasks alone. Each tag has its own row in the sidebar.";
    self.tag_field.delegate          = self;
    self.tag_field.completionDelay   = 0;
    self.tag_field.cell.wraps        = NO;
    self.tag_field.cell.scrollable   = YES;
    [self.tag_field setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1
                               forOrientation:NSLayoutConstraintOrientationHorizontal];

    /* The tags at the left, taking the spare width; the flag right-aligned
     * under the time picker. */
    self.priority_check = [NSButton checkboxWithTitle:@"High Priority" target:self
                                               action:@selector(flag_changed:)];
    self.priority_check.font = [NSFont systemFontOfSize:shared_look_ui_font];
    NSView *flags_row = row_stack(@[form_label(@"Tags:"), self.tag_field,
                                    self.priority_check]);
    [form setCustomSpacing:tasks_look_form_spacing * 1.2 afterView:status_row];
    add_row(form, flags_row);

    /* When the task was completed, on a line of its own under the flags;
     * hidden for a task never completed (show_completed:). */
    self.completed_label = [NSTextField labelWithString:@""];
    self.completed_label.font      = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    self.completed_label.textColor = NSColor.secondaryLabelColor;
    self.completed_label.hidden    = YES;
    add_row(form, self.completed_label);

    [form setCustomSpacing:tasks_look_form_spacing * 2.5 afterView:flags_row];
    [form setCustomSpacing:tasks_look_form_spacing * 2.5 afterView:self.completed_label];
    add_row(form, shared_ui_heading(@"Notes"));
    add_row(form, [self build_notes]);

    if (self.parent_id != 0) {
        Task *parent = task_db_task_get(self.app->db, self.parent_id);
        NSTextField *note = [NSTextField wrappingLabelWithString:[NSString stringWithFormat:
            @"This is a subtask of “%@” — subtasks cannot have their own subtasks.",
            parent != NULL ? tasks_title_string(parent, @"Untitled Task") : @"?"]];
        note.selectable              = NO;
        note.preferredMaxLayoutWidth = form_w;
        add_row(form, note);
        task_free(parent);
    }
}

/*
 * build_notes — the notes text: plain text, word-wrapped, with undo, in a
 * bordered scroll view that prefers exactly the configured number of lines
 * and grows when the window is dragged taller.
 *
 * Output:
 *   the scroll view (also kept in notes_scroll; the text view in notes_view).
 */
- (NSScrollView *)
build_notes
{
    self.notes_scroll = [NSTextView scrollableTextView];
    self.notes_view   = self.notes_scroll.documentView;

    self.notes_scroll.borderType         = NSBezelBorder;
    self.notes_view.richText             = NO;
    self.notes_view.importsGraphics      = NO;
    self.notes_view.allowsUndo           = YES;
    self.notes_view.font                 = [NSFont systemFontOfSize:shared_look_ui_font];
    self.notes_view.textContainerInset   = NSMakeSize(notes_inset, notes_inset);
    self.notes_view.delegate             = self;

    CGFloat line   = [self.notes_view.layoutManager defaultLineHeightForFont:self.notes_view.font];
    CGFloat height = ceil(line * tasks_look_editor_notes_lines) + 2 * notes_inset;
    NSSize  frame  = [NSScrollView frameSizeForContentSize:NSMakeSize(form_w, height)
                                   horizontalScrollerClass:Nil
                                     verticalScrollerClass:Nil
                                                borderType:self.notes_scroll.borderType
                                               controlSize:NSControlSizeRegular
                                             scrollerStyle:self.notes_scroll.scrollerStyle];

    NSLayoutConstraint *preferred =
        [self.notes_scroll.heightAnchor constraintEqualToConstant:frame.height];
    preferred.priority = notes_height_preferred;
    preferred.active   = YES;
    [self.notes_scroll.heightAnchor constraintGreaterThanOrEqualToConstant:frame.height].active = YES;
    return self.notes_scroll;
}

/*
 * build_subtasks — the Subtasks section: the list of [checkbox][title]
 * rows, and Add, Remove and the two move buttons beside it.
 *
 * Output:
 *   the section view.
 */
- (NSView *)
build_subtasks
{
    self.subtask_table = [[TasksEditorTable alloc] initWithFrame:NSZeroRect];
    NSScrollView *scroll = list_scroll(self.subtask_table, self);

    self.subtask_remove = side_button(@"Remove", self, @selector(remove_subtask:));
    self.subtask_up     = [NSButton buttonWithTitle:@"▲" target:self
                                             action:@selector(move_subtask:)];
    self.subtask_down   = [NSButton buttonWithTitle:@"▼" target:self
                                             action:@selector(move_subtask:)];
    self.subtask_up.toolTip   = @"Move the selected subtask up";
    self.subtask_down.toolTip = @"Move the selected subtask down";
    for (NSButton *button in @[self.subtask_up, self.subtask_down]) {
        button.controlSize = NSControlSizeSmall;
        button.font        = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    }

    /* The pair shares one button's width. */
    NSStackView *moves = row_stack(@[self.subtask_up, self.subtask_down]);
    moves.distribution = NSStackViewDistributionFillEqually;
    moves.spacing      = cell_gap;
    [moves.widthAnchor constraintEqualToConstant:list_button_w].active = YES;

    return list_section(@"Subtasks", scroll, @[
        side_button(@"Add", self, @selector(add_subtask:)), self.subtask_remove, moves,
    ]);
}

/*
 * build_attachments — the Attachments section: the list of file names, and
 * Add…, Remove and Open beside it.  A double-click opens a file.
 *
 * Output:
 *   the section view.
 */
- (NSView *)
build_attachments
{
    self.attachment_table = [[SharedTable alloc] initWithFrame:NSZeroRect];
    NSScrollView *scroll = list_scroll(self.attachment_table, self);

    self.attachment_table.target       = self;
    self.attachment_table.doubleAction = @selector(open_attachment:);

    self.attachment_remove = side_button(@"Remove", self, @selector(remove_attachment:));
    self.attachment_open   = side_button(@"Open", self, @selector(open_attachment:));
    return list_section(@"Attachments", scroll, @[
        side_button(@"Add…", self, @selector(add_attachment:)),
        self.attachment_remove, self.attachment_open,
    ]);
}

/*
 * build_recurrence — the Recurrence section: what it does, the switch, and
 * the body the switch shows (three rows whose leading labels line up, and
 * the summary).  The body is hidden, never greyed, while the switch is off.
 *
 * Output:
 *   the section view.
 */
- (NSView *)
build_recurrence
{
    self.recur_check = [NSButton checkboxWithTitle:@"Repeat this task" target:self
                                            action:@selector(recur_control_changed:)];
    self.recur_check.font    = [NSFont systemFontOfSize:shared_look_ui_font];
    self.recur_check.toolTip =
        @"Give this task a repeating schedule.  Switching it off leaves the task exactly "
        @"where it is and stops it coming back.";

    /* Row 1: Starting <date> at <time>. */
    self.recur_start_field = entry_field(@"YYYY-MM-DD", date_field_w,
        @"The day this schedule is anchored on — the \"Monday\" of \"every Monday at "
        @"9:00 AM\".  A start still in the future is the FIRST repeat, not a week after it.  "
        @"Leave it empty to anchor on the task's own due date.", self);
    self.recur_time_field = entry_field(@"HH:MM", time_field_w,
        @"The time of day the schedule is anchored at (24-hour).  A repeat measured in days, "
        @"weeks, months or years lands on this time every time; one measured in minutes or "
        @"hours starts from it and steps on from there.", self);
    NSStackView *starting = row_stack(@[
        self.recur_start_field,
        calendar_button(self, @selector(pick_start_date:), @"Pick the start date"),
        form_label(@"at"), self.recur_time_field,
    ]);

    /* Row 2: Repeat every <N> <unit>. */
    NSMutableArray<NSString *> *units = [NSMutableArray array];
    for (int unit = 0; unit < TASK_RECUR_N_UNITS; unit++) {
        [units addObject:tasks_string(task_recur_unit_label((TaskRecurUnit)unit))];
    }
    self.every_field = entry_field(@"", count_field_w,
        @"How often this task comes back.  A set time before each repeat, a COMPLETED task "
        @"is put back to New and its due date moves to that repeat.", self);
    self.every_stepper = count_stepper(TASK_EDIT_EVERY_MIN, TASK_EDIT_EVERY_MAX, self,
                                       @selector(stepper_changed:));
    self.unit_popup = popup_button(units, self, @selector(recur_control_changed:));
    NSStackView *every = row_stack(@[self.every_field, self.every_stepper, self.unit_popup]);

    /* Row 3: Reset to New <N> <unit> beforehand.  The lead's units are the
     * first TASK_EDIT_LEAD_N_UNITS repeat units. */
    self.lead_field = entry_field(@"", count_field_w,
        @"How far ahead of each repeat a completed task is reopened.  It is shortened "
        @"automatically when it would not fit inside the repeat itself.", self);
    self.lead_stepper = count_stepper(TASK_EDIT_LEAD_MIN, TASK_EDIT_LEAD_MAX, self,
                                      @selector(stepper_changed:));
    self.lead_unit_popup = popup_button(
        [units subarrayWithRange:NSMakeRange(0, TASK_EDIT_LEAD_N_UNITS)], self,
        @selector(recur_control_changed:));
    NSStackView *lead = row_stack(@[self.lead_field, self.lead_stepper, self.lead_unit_popup,
                                    form_label(@"beforehand")]);

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[form_label(@"Starting"), starting],
        @[form_label(@"Repeat every"), every],
        @[form_label(@"Reset to New"), lead],
    ]];
    grid.rowSpacing    = tasks_look_form_spacing;
    grid.columnSpacing = row_gap;
    grid.rowAlignment  = NSGridRowAlignmentNone;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementLeading;
    /* The lead is a different question about the same schedule. */
    [grid rowAtIndex:2].topPadding = tasks_look_form_spacing;

    self.recur_summary = shared_ui_note(@"", form_w);
    self.recur_body = column_stack(tasks_look_form_spacing);
    add_row(self.recur_body, grid);
    add_row(self.recur_body, self.recur_summary);
    self.recur_body.hidden = YES;

    NSStackView *section = column_stack(tasks_look_form_spacing);
    add_row(section, shared_ui_heading(@"Recurrence"));
    add_row(section, shared_ui_note(
        @"Recurrence will change the Due Date of your task to the next specified iteration "
        @"date and time.  Additionally, completed tasks will be set to New X (defaults to a "
        @"week, or less on a shorter repeat) before that Due Date to give you lead time.", form_w));
    add_row(section, self.recur_check);
    add_row(section, self.recur_body);
    return section;
}

/*
 * build_foot — the row at the window's bottom edge: the Advanced link at
 * the left; Save at the right, with Cancel before it for a new task.  Save
 * is the default button; Escape is Cancel.
 *
 * Output:
 *   the row.
 */
- (NSStackView *)
build_foot
{
    self.advanced_button = [NSButton buttonWithTitle:advanced_to_show target:self
                                              action:@selector(advanced_action:)];
    self.advanced_button.font     = [NSFont systemFontOfSize:shared_look_ui_font];
    self.advanced_button.bordered = NO;
    self.advanced_button.toolTip  =
        @"Show or hide the Recurrence, Subtasks and Attachments sections";

    NSButton *save = [NSButton buttonWithTitle:@"Save" target:self action:@selector(save_action:)];
    save.font          = [NSFont systemFontOfSize:shared_look_ui_font];
    save.keyEquivalent = @"\r";

    NSMutableArray<NSView *> *views = [NSMutableArray arrayWithObjects:
        self.advanced_button, spacer(), nil];
    if (self.is_new) {
        NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:self
                                              action:@selector(cancel_action:)];
        cancel.font          = [NSFont systemFontOfSize:shared_look_ui_font];
        cancel.keyEquivalent = @"\e";
        [views addObject:cancel];
    }
    [views addObject:save];
    return row_stack(views);
}

/*
 * present — show the window (or bring it forward).  A new task opens with
 * its title selected, ready to be typed over; an existing one opens with
 * nothing focused, so its title is not highlighted for no reason.
 *
 * Inputs:
 *   first — YES the first time, when the focus is set
 */
- (void)
present:(BOOL)first
{
    [self showWindow:nil];
    if (first) [self.window makeFirstResponder:self.is_new ? self.title_field : nil];
}

/* ── Loading ────────────────────────────────────────────────────────── */

/*
 * reload — show the task as it now stands in the database.
 *
 * Output:
 *   NO when the task no longer exists (the caller closes the editor).
 */
- (BOOL)
reload
{
    Task *task = task_db_task_get(self.app->db, self.task_id);
    BOOL  live = task != NULL && !task->deleted;

    if (live) [self show_task:task];
    task_free(task);
    return live;
}

/*
 * show_task: — put a stored task into the controls, and remember it as the
 * loaded row every save is diffed against.  Text is rewritten only where
 * it differs, so a cursor does not jump; a date, time or count field that
 * is being edited is left alone — and so is its column in the loaded row,
 * which keeps the value the field was showing.  A save then diffs that
 * field against what it showed, so it writes only what the user typed and
 * never puts the old value back over an outside change to the column.
 *
 * Inputs:
 *   task — the task as stored (borrowed)
 */
- (void)
show_task:(const Task *)task
{
    char        clock[TASK_EDIT_TIME_LEN];
    Task       *loaded = task_dup(task);
    const Task *before = self.loaded;

    if (before != NULL) {
        if (is_editing(self.due_field))         loaded->due            = before->due;
        if (is_editing(self.recur_start_field)) loaded->recur_start    = before->recur_start;
        if (is_editing(self.recur_time_field))  loaded->recur_time     = before->recur_time;
        if (is_editing(self.every_field))       loaded->recur_interval = before->recur_interval;
        if (is_editing(self.lead_field))        loaded->recur_lead     = before->recur_lead;
    }
    task_free(self.loaded);
    self.loaded = loaded;

    show_text(self.title_field, tasks_string(task->title), NO);
    [self show_source];
    [self show_status:task->status];
    [self show_tags];
    self.priority_check.state = task->priority ? NSControlStateValueOn : NSControlStateValueOff;
    [self show_date:task->due in:self.due_field];
    {
        int due_min = task->due_time >= 0 && task->due_time <= 23 * 60 + 59
            ? task->due_time : SHARED_DB_TASK_DUE_TIME_DEFAULT;
        NSDateComponents *comps = [[NSDateComponents alloc] init];
        comps.hour   = due_min / 60;
        comps.minute = due_min % 60;
        self.due_time_field.dateValue =
            [[NSCalendar currentCalendar] dateFromComponents:comps] ?: NSDate.date;
    }
    [self show_completed:task];

    NSString *notes = tasks_string(task->notes);
    if (![self.notes_view.string isEqualToString:notes]) {
        self.notes_view.string = notes;
        /* The undo stack describes text that is no longer there. */
        [self.notes_view.undoManager removeAllActions];
    }

    BOOL repeats = task->recur_interval > 0;
    self.recur_check.state = repeats ? NSControlStateValueOn : NSControlStateValueOff;
    [self show_count:repeats ? task->recur_interval : TASK_EDIT_EVERY_MIN
                  in:self.every_field stepper:self.every_stepper];
    [self.unit_popup selectItemAtIndex:repeats ? (NSInteger)task->recur_unit : TASK_RECUR_DAY];
    [self show_date:task->recur_start in:self.recur_start_field];
    task_edit_time_format(task->recur_time, SHARED_DB_TASK_RECUR_TIME_DEFAULT, clock);
    show_text(self.recur_time_field, @(clock), YES);
    [self show_lead:task->recur_lead];
    self.seen_every = repeats ? task->recur_interval : 0;
    self.seen_unit  = repeats ? (int)task->recur_unit : (int)TASK_RECUR_DAY;
    self.recur_next = task->recur_next;

    [self reload_subtasks];
    [self reload_attachments];
    [self refresh_title];
    [self refresh_recurrence];
}

/*
 * show_source — the line naming the note a Notes action item was written
 * in, hidden for any other task (task_db_task_note).  Read at every load
 * and by tasks_editor_refresh_sources, so a note renamed in Notes shows its
 * new title at the next poll.
 */
- (void)
show_source
{
    gchar    *note   = task_db_task_note(self.app->db, self.task_id);
    NSString *text   = note == NULL ? @"" :
        [NSString stringWithFormat:@"From note: \u201C%@\u201D",
            note[0] != '\0' ? tasks_string(note) : @"Untitled"];
    BOOL      hidden = note == NULL;
    g_free(note);

    if ([self.source_label.stringValue isEqualToString:text] &&
        self.source_label.hidden == hidden) return;
    self.source_label.stringValue = text;
    self.source_label.hidden      = hidden;
    if (self.window.visible) [self refit_animated:NO];
}

/* ── Tags ───────────────────────────────────────────────────────────── */

/*
 * stored_tag_names — the names of the tags the task carries, as stored.
 *
 * Output:
 *   the names, alphabetical.
 */
- (NSArray<NSString *> *)
stored_tag_names
{
    GPtrArray                  *tags  = task_db_task_tags(self.app->db, self.task_id);
    NSMutableArray<NSString *> *names = [NSMutableArray arrayWithCapacity:tags->len];

    for (guint i = 0; i < tags->len; i++) {
        [names addObject:tasks_string(((const TaskTag *)g_ptr_array_index(tags, i))->name)];
    }
    task_ptr_array_free_tags(tags);
    return names;
}

/*
 * has_name — whether a name is among some tag names: tag names compare
 * case-insensitively, as the database compares them.
 *
 * Inputs:
 *   names — the names
 *   name  — the name looked for
 *
 * Output:
 *   YES when it is there.
 */
static BOOL
has_name(NSArray<NSString *> *names, NSString *name)
{
    for (NSString *have in names) {
        if ([have caseInsensitiveCompare:name] == NSOrderedSame) return YES;
    }
    return NO;
}

/*
 * show_tags — the task's tags as the tag field's tokens.  A field being
 * edited is left alone (its tokens are written as they are made, and the
 * field is shown again when the edit ends).
 */
- (void)
show_tags
{
    if (is_editing(self.tag_field)) return;

    NSArray<NSString *> *names = [self stored_tag_names];
    if (![self.tag_field.objectValue isEqual:names]) self.tag_field.objectValue = names;
}

/*
 * drop_removed_tags — take off the task every tag that is no longer a
 * token in the field (the user deleted it).  Text still being typed never
 * names a stored tag that is not also a token, so it removes nothing.
 */
- (void)
drop_removed_tags
{
    NSMutableArray<NSString *> *tokens = [NSMutableArray array];
    for (id token in self.tag_field.objectValue) {
        if ([token isKindOfClass:NSString.class]) [tokens addObject:token];
    }

    GPtrArray *tags    = task_db_task_tags(self.app->db, self.task_id);
    BOOL       changed = NO;
    for (guint i = 0; i < tags->len; i++) {
        const TaskTag *tag = g_ptr_array_index(tags, i);
        if (has_name(tokens, tasks_string(tag->name))) continue;

        if (task_db_task_tag_remove(self.app->db, self.task_id, tag->id) > 0) {
            changed = YES;
        } else {
            gchar *what = g_strdup_printf("Removing the tag \xe2\x80\x9c%s\xe2\x80\x9d", tag->name);
            task_app_write_failed(self.app, what);
            g_free(what);
        }
    }
    task_ptr_array_free_tags(tags);
    if (changed) task_app_notify_tasks(self.app);
}

/*
 * tokenField:completionsForSubstring:indexOfToken:indexOfSelectedItem: —
 * the existing tags offered while a tag is typed: those whose name starts
 * with the text, then those containing it elsewhere, leaving out the tags
 * the task already carries.  Nothing is preselected, so Return makes the
 * text as typed a tag — a new one when no tag has that name — and an
 * offered tag is chosen with the arrow keys or a click.
 *
 * Inputs:
 *   field       — the tag field (unused)
 *   substring   — the text typed for this token
 *   token_index — its place among the tokens (unused)
 *   selected    — out: the preselected completion, -1 for none
 *
 * Output:
 *   the names to offer.
 */
- (NSArray *)
tokenField:(NSTokenField *)field
completionsForSubstring:(NSString *)substring
indexOfToken:(NSInteger)token_index
indexOfSelectedItem:(NSInteger *)selected
{
    NSArray<NSString *>        *have    = [self stored_tag_names];
    NSMutableArray<NSString *> *starts  = [NSMutableArray array];
    NSMutableArray<NSString *> *inside  = [NSMutableArray array];
    GPtrArray                  *tags    = task_db_tags(self.app->db, TASK_TAGS_ALL);

    for (guint i = 0; i < tags->len; i++) {
        NSString *name  = tasks_string(((const TaskTag *)g_ptr_array_index(tags, i))->name);
        NSRange   found = [name rangeOfString:substring options:NSCaseInsensitiveSearch];
        if (found.location == NSNotFound || has_name(have, name)) continue;
        [found.location == 0 ? starts : inside addObject:name];
    }
    task_ptr_array_free_tags(tags);
    NSArray<NSString *> *names = [starts arrayByAddingObjectsFromArray:inside];
    if (names.count > (NSUInteger)shared_look_tag_suggestions_max)
        names = [names subarrayWithRange:NSMakeRange(0, (NSUInteger)shared_look_tag_suggestions_max)];
    if (selected != NULL) *selected = -1;
    return names;
}

/*
 * tokenField:shouldAddObjects:atIndex: — tokens are being made from typed
 * or chosen text: give the task each tag at once (the existing tag of that
 * name, in any case, or a new one) and show it as stored.  A name the task
 * already carries, an empty one and one that could not be saved are not
 * made into tokens.
 *
 * Inputs:
 *   field  — the tag field (unused)
 *   tokens — the new tokens' texts
 *   index  — where they go (unused)
 *
 * Output:
 *   the tokens to insert: the stored names of the tags added.
 */
- (NSArray *)
tokenField:(NSTokenField *)field shouldAddObjects:(NSArray *)tokens atIndex:(NSUInteger)index
{
    NSArray<NSString *>        *have  = [self stored_tag_names];
    NSMutableArray<NSString *> *added = [NSMutableArray array];

    for (id token in tokens) {
        if (![token isKindOfClass:NSString.class]) continue;
        NSString *name = [token stringByTrimmingCharactersInSet:
                             NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (name.length == 0 || has_name(have, name) || has_name(added, name)) continue;

        int64_t  tag_id = task_db_task_tag_add(self.app->db, self.task_id, name.UTF8String);
        TaskTag *tag    = tag_id != 0 ? task_db_tag_get(self.app->db, tag_id) : NULL;
        if (tag == NULL) {
            gchar *what = g_strdup_printf("The tag \xe2\x80\x9c%s\xe2\x80\x9d", name.UTF8String);
            task_app_write_failed(self.app, what);
            g_free(what);
            continue;
        }
        [added addObject:tasks_string(tag->name)];
        task_tag_free(tag);
    }
    if (added.count > 0) task_app_notify_tasks(self.app);
    return added;
}

/*
 * show_status: — select a status in the pop-up.  Setting a pop-up's
 * selection sends no action, so this saves nothing.
 *
 * Inputs:
 *   status — the stored status; out of range shows New
 */
- (void)
show_status:(TaskStatus)status
{
    BOOL known = status >= 0 && status < TASK_STATUS_N_VALUES;

    [self.status_popup selectItemAtIndex:known ? (NSInteger)status : TASK_STATUS_NEW];
}

/*
 * show_date:in: — show a stored date in a date field as YYYY-MM-DD, unless
 * the field is being edited.
 *
 * Inputs:
 *   date  — local-midnight timestamp, 0 for none (shown empty)
 *   field — the due or the start field
 */
- (void)
show_date:(int64_t)date in:(NSTextField *)field
{
    gchar *iso = task_due_format_iso(date);

    show_text(field, tasks_string(iso), YES);
    g_free(iso);
}

/*
 * show_count:in:stepper: — show a count in its field and its stepper,
 * unless the field is being edited: then neither changes, since a save
 * reads the stepper and diffs it against the loaded value show_task: kept.
 *
 * Inputs:
 *   count   — the value
 *   field   — the count's field
 *   stepper — the count's stepper; the value is clamped to its range (a
 *             hand-edited database can hold anything)
 */
- (void)
show_count:(int)count in:(NSTextField *)field stepper:(NSStepper *)stepper
{
    if (is_editing(field)) return;
    stepper.integerValue = MAX((int)stepper.minValue, MIN((int)stepper.maxValue, count));
    show_text(field, [NSString stringWithFormat:@"%ld", (long)stepper.integerValue], YES);
}

/*
 * show_lead: — show a lead as a count and the largest unit that divides it;
 * nothing changes while the count is being edited (the unit too, since a
 * save multiplies the two).
 *
 * Inputs:
 *   minutes — the lead in minutes
 */
- (void)
show_lead:(int)minutes
{
    int count = 0, unit = 0;

    if (is_editing(self.lead_field)) return;
    task_edit_lead_split(minutes, &count, &unit);
    [self show_count:count in:self.lead_field stepper:self.lead_stepper];
    [self.lead_unit_popup selectItemAtIndex:unit];
}

/*
 * show_completed: — the read-only line saying when the task was last
 * completed: "Completed <date>" while it is Done, "Last completed <date>"
 * once it has been reopened; hidden when it never was.  The window is
 * refitted when the line appears or goes.
 *
 * Inputs:
 *   task — the task as stored (borrowed)
 */
- (void)
show_completed:(const Task *)task
{
    gchar    *when   = task_due_format(task->completed_at);
    NSString *text   = *when == '\0' ? @"" :
        [NSString stringWithFormat:@"%@ %@",
            task->status == TASK_STATUS_DONE ? @"Completed" : @"Last completed",
            tasks_string(when)];
    BOOL      hidden = text.length == 0;
    g_free(when);

    if ([self.completed_label.stringValue isEqualToString:text] &&
        self.completed_label.hidden == hidden) return;
    self.completed_label.stringValue = text;
    self.completed_label.hidden      = hidden;
    if (self.window.visible) [self refit_animated:NO];
}

/*
 * refresh_title — the window's title: the task's title as typed, or
 * "Untitled Task" while it is empty.
 */
- (void)
refresh_title
{
    NSString *title = self.title_field.stringValue;

    self.window.title = title.length > 0 ? title : @"Untitled Task";
}

/* ── Saving ─────────────────────────────────────────────────────────── */

/*
 * form — the date and schedule controls as they stand, for task_edit.h.
 *
 * Output:
 *   the form.  Its strings belong to autoreleased NSStrings: use it within
 *   the method that asked for it.
 */
- (task_edit_form_t)
form
{
    NSCalendar       *cal  = [NSCalendar currentCalendar];
    NSDateComponents *tc   = [cal components:NSCalendarUnitHour | NSCalendarUnitMinute
                                   fromDate:self.due_time_field.dateValue];
    NSString         *time = [NSString stringWithFormat:@"%02ld:%02ld",
                              (long)tc.hour, (long)tc.minute];
    return (task_edit_form_t){
        .due_text         = self.due_field.stringValue.UTF8String,
        .due_time_text    = time.UTF8String,
        .repeat           = self.recur_check.state == NSControlStateValueOn,
        .every            = (int)self.every_stepper.integerValue,
        .unit             = (int)self.unit_popup.indexOfSelectedItem,
        .recur_start_text = self.recur_start_field.stringValue.UTF8String,
        .recur_time_text  = self.recur_time_field.stringValue.UTF8String,
        .lead_count       = (int)self.lead_stepper.integerValue,
        .lead_unit        = (int)self.lead_unit_popup.indexOfSelectedItem,
    };
}

/*
 * save_now — write the fields the user has changed, now, and tell the app
 * its tasks changed.  Any queued debounced save is superseded.
 *
 * The controls are read over a copy of the LOADED row and only the columns
 * that differ from it are written (task_db_task_update), so a field the
 * user did not touch is never written back — a change another app made to
 * it meanwhile survives, and arrives at the next refresh.  Half-typed dates
 * and times keep the loaded value (task_edit_apply), so they write nothing
 * either.  Afterwards the loaded row is what the controls show.
 *
 * When the write fails (another writer held the file past the busy
 * timeout) nothing is lost: the edit stays in the controls, save_pending
 * stays set and the loaded row unchanged, so the next save — the next
 * debounce, leaving a field, closing the window — tries again; the status
 * bar says it was not saved.
 *
 * Does nothing once the editor is closed or its new task was cancelled.
 *
 * Output:
 *   NO when the write failed (the edit is still pending); YES otherwise,
 *   including when there was nothing to write.
 */
- (BOOL)
save_now
{
    self.save_pending = NO;
    self.save_id++;
    if (self.is_shut_down || self.discarded || self.loaded == NULL) return YES;

    /* A copy of the loaded row with the controls' values over it.  The two
     * strings are lent to it for the update, not given. */
    Task             edited = *self.loaded;
    task_edit_form_t form   = [self form];
    NSInteger        status = self.status_popup.indexOfSelectedItem;

    edited.title    = (char *)self.title_field.stringValue.UTF8String;
    edited.notes    = (char *)self.notes_view.string.UTF8String;
    edited.status   = status >= 0 && status < TASK_STATUS_N_VALUES ? (TaskStatus)status
                                                                   : TASK_STATUS_NEW;
    edited.priority = self.priority_check.state == NSControlStateValueOn;
    task_edit_apply(&form, &edited);
    edited.recur_next = self.recur_next;

    gint written = task_db_task_update(self.app->db, self.loaded, &edited);
    if (written < 0) {
        self.save_pending = YES;
        task_app_status(self.app, "Changes to \xe2\x80\x9c%s\xe2\x80\x9d were not saved "
                        "yet: " TASK_APP_WRITE_FAILED_REASON "; they are kept and saved again "
                        "with the next change or when the editor closes", edited.title);
        return NO;
    }
    if (written == 0) return YES;

    /* A schedule that now fires sooner has to wake the recurrence pass. */
    task_recur_wake_by(self.app, edited.recur_next > 0
        ? edited.recur_next - task_recur_lead_seconds(&edited) : 0);

    BOOL  status_moved = edited.status != self.loaded->status;
    Task *saved        = task_dup(&edited);
    task_free(self.loaded);
    self.loaded = saved;

    /* Moving into or out of Done changes the completion line; the stamp is
     * the database's, so read it back. */
    if (status_moved) {
        Task *fresh = task_db_task_get(self.app->db, self.task_id);
        if (fresh != NULL) {
            self.loaded->completed_at = fresh->completed_at;
            [self show_completed:fresh];
        }
        task_free(fresh);
    }
    task_app_notify_tasks(self.app);
    return YES;
}

/*
 * queue_save — save once the typing pauses: (re)start the debounce.  Each
 * call supersedes the one before, so a burst of keystrokes is one save.
 */
- (void)
queue_save
{
    NSUInteger save_id = ++self.save_id;
    self.save_pending  = YES;

    __weak TasksEditorWindow *ws = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(shared_look_autosave_delay * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        TasksEditorWindow *s = ws;
        if (!s || s.save_id != save_id || !s.save_pending) return;   /* gone, or superseded */
        [s save_now];
    });
}

/*
 * status_changed: — the status pop-up: saved at once.
 *
 * Inputs:
 *   sender — the pop-up (unused)
 */
- (void)
status_changed:(id)sender
{
    [self save_now];
}

/*
 * flag_changed: — Favorite or High Priority: saved at once.
 *
 * Inputs:
 *   sender — the checkbox (unused)
 */
- (void)
flag_changed:(id)sender
{
    [self save_now];
}

/*
 * resync_status — show the status the database now holds, and take it as
 * the loaded one.  Called when something other than the pop-up moved it
 * (completing a subtask promotes a New parent to In Progress), so the pop-up
 * agrees with the row and the next save does not see a change to write.
 */
- (void)
resync_status
{
    Task *task = task_db_task_get(self.app->db, self.task_id);

    if (task != NULL && self.loaded != NULL) {
        [self show_status:task->status];
        self.loaded->status       = task->status;
        self.loaded->completed_at = task->completed_at;
    }
    task_free(task);
}

/* ── Recurrence ─────────────────────────────────────────────────────── */

/*
 * recur_changed — any schedule control changed: let an untouched lead
 * follow the new period, recompute the next occurrence, redraw the summary
 * and the fold, and queue a save.
 */
- (void)
recur_changed
{
    task_edit_form_t form = [self form];
    int seen_every = self.seen_every, seen_unit = self.seen_unit, lead = 0;

    if (task_edit_lead_follow(&seen_every, &seen_unit, &form, &lead)) {
        [self show_lead:lead];
        form = [self form];
    }
    self.seen_every = seen_every;
    self.seen_unit  = seen_unit;

    self.recur_next = task_edit_reseed(&form, (int64_t)time(NULL));
    [self refresh_recurrence];
    [self queue_save];
}

/*
 * refresh_recurrence — the Recurrence block's single applier: write the
 * summary from the controls, then fit the window, which shows or hides the
 * body according to the switch.  The fit comes last because the summary is
 * a wrapped label whose height depends on its text.
 */
- (void)
refresh_recurrence
{
    task_edit_form_t form     = [self form];
    Task             schedule = task_edit_schedule(&form, self.recur_next);
    gchar           *summary  = task_recur_describe(&schedule, (int64_t)time(NULL));
    NSString        *text     = tasks_string(summary);
    g_free(summary);

    /* The fit (two layout passes of the whole form) only when it would
     * change something: the summary's text, whose wrapped height it sets,
     * or a fold that is not yet as its switch says. */
    BOOL body_now = self.recur_check.state == NSControlStateValueOn;
    BOOL changed  = ![self.recur_summary.stringValue isEqualToString:text] ||
                    self.recur_body.hidden == body_now ||
                    self.advanced_box.hidden == self.advanced_shown;

    self.recur_summary.stringValue = text;
    if (changed) [self refit_animated:YES];
}

/*
 * recur_control_changed: — the Repeat switch or one of the unit pop-ups.
 *
 * Inputs:
 *   sender — the control (unused)
 */
- (void)
recur_control_changed:(id)sender
{
    [self recur_changed];
}

/*
 * stepper_changed: — a count's arrows: show the new value in its field.
 *
 * Inputs:
 *   sender — the "every" or the lead stepper
 */
- (void)
stepper_changed:(NSStepper *)sender
{
    NSTextField *field = sender == self.every_stepper ? self.every_field : self.lead_field;

    field.integerValue = sender.integerValue;
    [self recur_changed];
}

/*
 * count_typed: — a count field's text changed: move its stepper to the
 * number typed, or leave it where it is while the text is not one.
 *
 * Inputs:
 *   field — the "every" or the lead field
 */
- (void)
count_typed:(NSTextField *)field
{
    BOOL       every   = field == self.every_field;
    NSStepper *stepper = every ? self.every_stepper : self.lead_stepper;

    stepper.integerValue = task_edit_count_parse(field.stringValue.UTF8String,
                                                 (int)stepper.minValue, (int)stepper.maxValue,
                                                 (int)stepper.integerValue);
    [self recur_changed];
}

/* ── The folds and the window's height ──────────────────────────────── */

/*
 * has_advanced_content — whether the task already has a schedule, subtasks
 * or attachments.  Read from the loaded controls: call after show_task:.
 *
 * Output:
 *   YES when the Advanced block has something to show.
 */
- (BOOL)
has_advanced_content
{
    return self.recur_check.state == NSControlStateValueOn ||
           (self.subtasks != NULL && self.subtasks->len > 0) ||
           (self.attachments != NULL && self.attachments->len > 0);
}

/*
 * refresh_advanced_button — title the Advanced link for what a click will
 * do, in the link colour.
 */
- (void)
refresh_advanced_button
{
    self.advanced_button.attributedTitle = [[NSAttributedString alloc]
        initWithString:self.advanced_shown ? advanced_to_fold : advanced_to_show
            attributes:@{
                NSForegroundColorAttributeName: shared_ui_color(shared_look_link),
                NSUnderlineStyleAttributeName:  @(NSUnderlineStyleSingle),
                NSFontAttributeName:            [NSFont systemFontOfSize:shared_look_ui_font],
            }];
}

/*
 * advanced_action: — the Advanced link: open or close the fold.
 *
 * Inputs:
 *   sender — the link (unused)
 */
- (void)
advanced_action:(id)sender
{
    self.advanced_shown = !self.advanced_shown;
    [self refresh_advanced_button];
    [self refit_animated:YES];
}

/*
 * set_content_height:animated:then: — give the window's content a new
 * height, keeping the window's TOP edge where it is (AppKit's origin is the
 * bottom-left corner, so the origin moves by the difference) — unless that
 * would push its bottom edge below the screen's work area, as it would for
 * a Quick Task editor in the lower-right corner opening its Advanced fold:
 * the window then moves up just far enough, never above the work area.
 *
 * Inputs:
 *   height   — the new content height
 *   animated — YES to slide there over fold_duration
 *   then     — called when the window has its new height: at once when not
 *              animated, else when the animation ends; not nil
 */
- (void)
set_content_height:(CGFloat)height animated:(BOOL)animated then:(void (^)(void))then
{
    NSWindow *window  = self.window;
    NSRect    content = [window contentRectForFrameRect:window.frame];

    content.origin.y   += NSHeight(content) - height;
    content.size.height = height;
    NSRect frame = [window frameRectForContentRect:content];

    NSScreen *screen = window.screen;
    if (screen != nil && NSMinY(frame) < NSMinY(screen.visibleFrame)) {
        /* Bottom on the work area's bottom; a window taller than the
         * work area keeps its title bar at the top instead. */
        NSRect area    = screen.visibleFrame;
        frame.origin.y = MIN(NSMinY(area), NSMaxY(area) - NSHeight(frame));
    }

    if (!animated) {
        [window setFrame:frame display:window.visible];
        then();
        return;
    }
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
        context.duration = fold_duration;
        [window.animator setFrame:frame display:YES];
    } completionHandler:then];
}

/*
 * refit_animated: — the single applier of the two folds: show or hide the
 * Advanced block (advanced_shown) and the recurrence body (the Repeat
 * switch), and give the window the height its content now needs, its top
 * edge fixed.  Height the user added by dragging the window taller, which
 * the notes take up, is kept.
 *
 * Growing: the content appears at once and the window's edge uncovers it.
 * Shrinking: the edge covers the content first, and it is hidden when the
 * window has arrived.  A change that arrives while the window is still
 * moving is applied when it stops.
 *
 * Inputs:
 *   animated — YES to animate when the window is on screen
 */
- (void)
refit_animated:(BOOL)animated
{
    if (self.is_folding) {
        self.refit_wanted = YES;
        return;
    }

    NSWindow *window   = self.window;
    NSView   *content  = window.contentView;
    BOOL      adv_was  = !self.advanced_box.hidden;
    BOOL      body_was = !self.recur_body.hidden;
    BOOL      adv_now  = self.advanced_shown;
    BOOL      body_now = self.recur_check.state == NSControlStateValueOn;

    /* Before the first fit the window has no meaningful height of its own. */
    [content layoutSubtreeIfNeeded];
    CGFloat before = content.fittingSize.height;
    CGFloat extra  = window.visible ? MAX(0.0, NSHeight(content.frame) - before) : 0.0;

    self.advanced_box.hidden = !adv_now;
    self.recur_body.hidden   = !body_now;
    CGFloat after  = content.fittingSize.height;
    CGFloat height = after + extra;

    __weak TasksEditorWindow *ws = self;
    void (^arrived)(void) = ^{
        TasksEditorWindow *s = ws;
        if (!s || s.is_shut_down) return;

        s.advanced_box.hidden = !adv_now;
        s.recur_body.hidden   = !body_now;
        s.notes_hold.active   = NO;
        s.notes_hold          = nil;
        s.is_folding          = NO;
        /* The window may be dragged taller, never shorter than its content
         * or to another width. */
        s.window.contentMinSize = NSMakeSize(tasks_look_editor_w, after);
        s.window.contentMaxSize = NSMakeSize(tasks_look_editor_w, CGFLOAT_MAX);
        if (s.refit_wanted) {
            s.refit_wanted = NO;
            [s refit_animated:YES];
        }
    };

    if (!animated || !window.visible) {
        [self set_content_height:height animated:NO then:arrived];
        return;
    }
    if (height == NSHeight(content.frame)) {
        arrived();
        return;
    }

    /* While the window moves, the notes keep the height they have (they
     * would otherwise give their extra height up to the opening fold and
     * take it back), and a closing fold stays visible to be covered. */
    self.is_folding = YES;
    self.notes_hold = [self.notes_scroll.heightAnchor
        constraintEqualToConstant:NSHeight(self.notes_scroll.frame)];
    self.notes_hold.active = YES;
    if (after < before) {
        self.advanced_box.hidden = !adv_was;
        self.recur_body.hidden   = !body_was;
        self.window.contentMinSize = NSMakeSize(tasks_look_editor_w, after);
    }
    [self set_content_height:height animated:YES then:arrived];
}

/* ── Subtasks ───────────────────────────────────────────────────────── */

/*
 * is_subtask_field: — whether a view is a title field of the subtask list.
 *
 * Inputs:
 *   view — a control
 *
 * Output:
 *   YES when it sits in a row of the subtask table.
 */
- (BOOL)
is_subtask_field:(NSView *)view
{
    return self.subtask_table != nil && [self.subtask_table rowForView:view] >= 0;
}

/*
 * selected_subtask — the subtask the list's buttons act on.
 *
 * Output:
 *   the selected row's task (borrowed from the list), or NULL when no row
 *   is selected.
 */
- (const Task *)
selected_subtask
{
    NSInteger row = self.subtask_table.selectedRow;

    if (self.subtasks == NULL || row < 0 || (guint)row >= self.subtasks->len) return NULL;
    return g_ptr_array_index(self.subtasks, (guint)row);
}

/*
 * reload_subtasks — read the subtasks from the database and show them, the
 * selection staying on the same subtask.  When the same subtasks are still
 * there in the same order the rows are updated in place, because rebuilding
 * the table would destroy a title field that is being edited.
 */
- (void)
reload_subtasks
{
    if (self.subtask_table == nil) return;

    const Task *selected    = [self selected_subtask];
    int64_t     selected_id = selected != NULL ? selected->id : 0;
    GPtrArray  *fresh       = task_db_subtasks(self.app->db, self.task_id);
    BOOL        in_place    = same_tasks(self.subtasks, fresh);

    task_ptr_array_free_tasks(self.subtasks);
    self.subtasks = fresh;

    if (in_place) {
        for (guint row = 0; row < fresh->len; row++) {
            NSTableCellView *cell = [self.subtask_table viewAtColumn:0 row:(NSInteger)row
                                                     makeIfNecessary:NO];
            if (cell != nil) [self fill_subtask_cell:cell row:(NSInteger)row];
        }
    } else {
        [self.subtask_table reloadData];
        [self select_subtask:selected_id];
    }
    [self refresh_list_buttons];
}

/*
 * select_subtask: — select the row showing a subtask and scroll to it.
 *
 * Inputs:
 *   subtask_id — the subtask; 0 or one that is not listed selects nothing
 *
 * Output:
 *   its row, or -1.
 */
- (NSInteger)
select_subtask:(int64_t)subtask_id
{
    for (guint row = 0; self.subtasks != NULL && row < self.subtasks->len; row++) {
        if (((const Task *)g_ptr_array_index(self.subtasks, row))->id != subtask_id) continue;

        [self.subtask_table selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                        byExtendingSelection:NO];
        [self.subtask_table scrollRowToVisible:(NSInteger)row];
        return (NSInteger)row;
    }
    [self.subtask_table deselectAll:nil];
    return -1;
}

/*
 * fill_subtask_cell:row: — show a subtask in its row: the tick, and the
 * title unless that field is being edited.
 *
 * Inputs:
 *   cell — the row's cell view
 *   row  — the row
 */
- (void)
fill_subtask_cell:(NSTableCellView *)cell row:(NSInteger)row
{
    const Task *subtask = g_ptr_array_index(self.subtasks, (guint)row);
    NSButton   *check   = [cell viewWithTag:subtask_check_tag];

    check.state = subtask->status == TASK_STATUS_DONE ? NSControlStateValueOn
                                                      : NSControlStateValueOff;
    show_text(cell.textField, tasks_string(subtask->title), YES);
}

/*
 * subtask_cell — a new, empty row view for the subtask list: a checkbox and
 * a borderless title field that is edited in place.
 *
 * Output:
 *   the cell view.
 */
- (NSTableCellView *)
subtask_cell
{
    NSTableCellView *cell  = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    NSButton        *check = [NSButton checkboxWithTitle:@"" target:self
                                                  action:@selector(subtask_ticked:)];
    NSTextField     *field = [[TasksSubtaskField alloc] initWithFrame:NSZeroRect];

    check.tag         = subtask_check_tag;
    check.controlSize = NSControlSizeSmall;

    field.bordered           = NO;
    field.drawsBackground    = NO;
    field.editable           = YES;
    field.usesSingleLineMode = YES;
    field.lineBreakMode      = NSLineBreakByTruncatingTail;
    field.font               = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    field.delegate           = self;

    cell.identifier = subtask_cell_id;
    cell.textField  = field;
    check.translatesAutoresizingMaskIntoConstraints = NO;
    field.translatesAutoresizingMaskIntoConstraints = NO;
    [cell addSubview:check];
    [cell addSubview:field];
    [NSLayoutConstraint activateConstraints:@[
        [check.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:cell_pad],
        [check.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        [field.leadingAnchor constraintEqualToAnchor:check.trailingAnchor constant:cell_gap],
        [field.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-cell_pad],
        [field.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
    ]];
    return cell;
}

/*
 * subtask_ticked: — a subtask's checkbox: Done when ticked, In Progress when
 * cleared (work on it resumed).  Completing a subtask may promote the
 * parent from New to In Progress, so the status pop-up is resynced.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
subtask_ticked:(NSButton *)sender
{
    NSInteger row = [self.subtask_table rowForView:sender];
    if (row < 0 || (guint)row >= self.subtasks->len) return;

    BOOL    done       = sender.state == NSControlStateValueOn;
    int64_t subtask_id = ((const Task *)g_ptr_array_index(self.subtasks, (guint)row))->id;

    if (!task_db_task_set_status(self.app->db, subtask_id,
                                 done ? TASK_STATUS_DONE : TASK_STATUS_IN_PROGRESS)) {
        [self reload_subtasks];          /* the box as stored */
        task_app_write_failed(self.app, "The subtask's status");
        return;
    }
    [self reload_subtasks];
    if (done) [self resync_status];
    task_app_notify_tasks(self.app);
}

/*
 * save_subtask_title: — a subtask's title field finished editing: write the
 * title if it changed.
 *
 * Inputs:
 *   field — the title field
 */
- (void)
save_subtask_title:(NSTextField *)field
{
    NSInteger row = [self.subtask_table rowForView:field];
    if (row < 0 || (guint)row >= self.subtasks->len) return;

    int64_t subtask_id = ((const Task *)g_ptr_array_index(self.subtasks, (guint)row))->id;
    Task   *stored     = task_db_task_get(self.app->db, subtask_id);
    if (stored == NULL) return;

    /* The title is lent to the copy for the update (see save_now); only the
     * title column is written, and only when it changed. */
    Task edited  = *stored;
    edited.title = (char *)field.stringValue.UTF8String;
    gint written = task_db_task_update(self.app->db, stored, &edited);
    if (written > 0) {
        [self reload_subtasks];
        task_app_notify_tasks(self.app);
    } else if (written < 0) {
        task_app_write_failed(self.app, "The subtask's title");
    }
    task_free(stored);
}

/*
 * add_subtask: — Add: create a subtask at the end of the list and start
 * editing its title, selected so that typing replaces it.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
add_subtask:(id)sender
{
    Task *task = task_db_task_get(self.app->db, self.task_id);
    if (task == NULL) return;

    /* End an edit in progress first: the new row rebuilds the table. */
    [self.window makeFirstResponder:nil];
    int64_t subtask_id = task_db_task_create(self.app->db, self.task_id, "New subtask", NULL);
    task_free(task);
    if (subtask_id == 0) {
        task_app_status(self.app, "Could not create the subtask");
        return;
    }

    [self reload_subtasks];
    NSInteger row = [self select_subtask:subtask_id];
    if (row >= 0) [self.subtask_table editColumn:0 row:row withEvent:nil select:YES];
    task_app_notify_tasks(self.app);
}

/*
 * remove_subtask: — Remove: delete the selected subtask.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
remove_subtask:(id)sender
{
    const Task *subtask = [self selected_subtask];
    if (subtask == NULL) return;

    int64_t subtask_id = subtask->id;
    [self.window makeFirstResponder:nil];
    task_db_task_delete(self.app->db, subtask_id);
    [self reload_subtasks];
    task_app_notify_tasks(self.app);
}

/*
 * move_subtask: — the up and down buttons: move the selected subtask one
 * place and keep it selected.
 *
 * Inputs:
 *   sender — subtask_up or subtask_down
 */
- (void)
move_subtask:(id)sender
{
    const Task *subtask = [self selected_subtask];
    if (subtask == NULL) return;

    int64_t subtask_id = subtask->id;
    [self.window makeFirstResponder:nil];
    task_db_subtask_move(self.app->db, subtask_id, sender == self.subtask_up ? -1 : 1);
    [self reload_subtasks];
    [self select_subtask:subtask_id];
    [self refresh_list_buttons];
    task_app_notify_tasks(self.app);
}

/* ── Attachments ────────────────────────────────────────────────────── */

/*
 * attachment_path: — the file an attachment row names.
 *
 * Inputs:
 *   row — a row of the attachment list
 *
 * Output:
 *   its path.
 */
- (NSString *)
attachment_path:(NSInteger)row
{
    return tasks_string(((const TaskAttachment *)g_ptr_array_index(self.attachments,
                                                                   (guint)row))->path);
}

/*
 * reload_attachments — read the attachments from the database and show
 * them, the selection staying on the same attachment.  Each file's icon is
 * looked up once here, not per cell.  A path inside the fenced live data
 * directory is not examined: it gets the generic document icon.
 */
- (void)
reload_attachments
{
    NSInteger selected    = self.attachment_table.selectedRow;
    int64_t   selected_id = 0;
    if (self.attachments != NULL && selected >= 0 && (guint)selected < self.attachments->len) {
        selected_id = ((const TaskAttachment *)g_ptr_array_index(self.attachments,
                                                                 (guint)selected))->id;
    }

    /* The same attachments as shown: nothing to redo.  A reload comes with
     * every field blur and every refresh of every editor, and the icon
     * lookup below touches each file — on a network volume, slowly. */
    GPtrArray *fresh = task_db_attachments(self.app->db, self.task_id);
    if (same_attachments(self.attachments, fresh)) {
        task_ptr_array_free_attachments(fresh);
        return;
    }
    task_ptr_array_free_attachments(self.attachments);
    self.attachments = fresh;

    NSMutableArray<NSImage *> *icons = [NSMutableArray array];
    for (guint row = 0; row < self.attachments->len; row++) {
        NSString *path = [self attachment_path:(NSInteger)row];
        [icons addObject:task_path_is_live_data(path.fileSystemRepresentation)
            ? [NSWorkspace.sharedWorkspace iconForContentType:UTTypeData]
            : [NSWorkspace.sharedWorkspace iconForFile:path]];
    }
    self.attachment_icons = icons;

    [self.attachment_table reloadData];
    [self.attachment_table deselectAll:nil];
    for (guint row = 0; row < self.attachments->len; row++) {
        if (((const TaskAttachment *)g_ptr_array_index(self.attachments, row))->id != selected_id) {
            continue;
        }
        [self.attachment_table selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                           byExtendingSelection:NO];
    }
    [self refresh_list_buttons];
}

/*
 * attachment_cell — a new, empty row view for the attachment list: the
 * file's icon and its name, truncated in the middle.
 *
 * Output:
 *   the cell view.
 */
- (NSTableCellView *)
attachment_cell
{
    NSTableCellView *cell  = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    NSImageView     *icon  = shared_ui_image_view(nil, list_icon, list_icon);
    NSTextField     *label = [NSTextField labelWithString:@""];

    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    label.font          = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];

    cell.identifier = attachment_cell_id;
    cell.imageView  = icon;
    cell.textField  = label;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [cell addSubview:icon];
    [cell addSubview:label];
    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:cell_pad],
        [icon.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        [label.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:cell_gap],
        [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-cell_pad],
        [label.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
    ]];
    return cell;
}

/*
 * add_attachment: — Add…: choose a file and record it against the task.
 * Only the path is stored; the file is not copied.  A file inside the
 * fenced live data directory is refused.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
add_attachment:(id)sender
{
    __weak TasksEditorWindow *ws = self;
    shared_ui_pick_path(SharedPickOpenFile, @"Choose a file to attach to this task.",
                        @"Attach", nil, nil, ^(NSString *path) {
        TasksEditorWindow *s = ws;
        if (!s || s.is_shut_down || path == nil) return;

        if (task_path_is_live_data(path.fileSystemRepresentation)) {
            shared_ui_notice(s.window, @"That file cannot be attached",
                            @"It is in the data folder of the other Tasks app, which this "
                            @"app does not touch.", nil);
            return;
        }
        task_db_attachment_add(s.app->db, s.task_id, path.fileSystemRepresentation);
        [s reload_attachments];
        task_app_notify_tasks(s.app);
    });
}

/*
 * remove_attachment: — Remove: forget the selected attachment.  The file
 * itself is not touched.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
remove_attachment:(id)sender
{
    NSInteger row = self.attachment_table.selectedRow;
    if (row < 0) return;

    task_db_attachment_remove(self.app->db,
        ((const TaskAttachment *)g_ptr_array_index(self.attachments, (guint)row))->id);
    [self reload_attachments];
    task_app_notify_tasks(self.app);
}

/*
 * open_attachment: — Open, and a double-click on a row: open the file in
 * its default application.  The status bar says so when it cannot be
 * opened (moved, deleted, or inside the fenced live data directory).
 *
 * Inputs:
 *   sender — the Open button, or the table for a double-click
 */
- (void)
open_attachment:(id)sender
{
    NSInteger row = sender == self.attachment_table ? self.attachment_table.clickedRow
                                                    : self.attachment_table.selectedRow;
    if (row < 0) return;

    NSString *path = [self attachment_path:row];
    if (task_path_is_live_data(path.fileSystemRepresentation) ||
        ![NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:path]]) {
        task_app_status(self.app, "Could not open %s", path.lastPathComponent.UTF8String);
    }
}

/* ── The two lists' table plumbing ──────────────────────────────────── */

/*
 * refresh_list_buttons — enable the buttons that act on a selected row:
 * Remove for both lists, Open, and each move button unless the subtask is
 * already at that end.
 */
- (void)
refresh_list_buttons
{
    NSInteger subtask    = self.subtask_table.selectedRow;
    NSInteger last       = self.subtasks != NULL ? (NSInteger)self.subtasks->len - 1 : -1;
    BOOL      attachment = self.attachment_table.selectedRow >= 0;

    self.subtask_remove.enabled    = subtask >= 0;
    self.subtask_up.enabled        = subtask > 0;
    self.subtask_down.enabled      = subtask >= 0 && subtask < last;
    self.attachment_remove.enabled = attachment;
    self.attachment_open.enabled   = attachment;
}

/*
 * numberOfRowsInTableView: — how many subtasks or attachments there are.
 *
 * Inputs:
 *   table — the subtask or the attachment table
 *
 * Output:
 *   the row count.
 */
- (NSInteger)
numberOfRowsInTableView:(NSTableView *)table
{
    const GPtrArray *rows = table == self.subtask_table ? self.subtasks : self.attachments;

    return rows != NULL ? (NSInteger)rows->len : 0;
}

/*
 * tableView:viewForTableColumn:row: — the view of one row of either list,
 * reused where the table has one to spare.
 *
 * Inputs:
 *   table  — the subtask or the attachment table
 *   column — its only column (unused)
 *   row    — the row
 *
 * Output:
 *   the filled cell view.
 */
- (NSView *)
tableView:(NSTableView *)table
viewForTableColumn:(NSTableColumn *)column
              row:(NSInteger)row
{
    if (table == self.subtask_table) {
        NSTableCellView *cell = [table makeViewWithIdentifier:subtask_cell_id owner:nil]
            ?: [self subtask_cell];
        [self fill_subtask_cell:cell row:row];
        return cell;
    }

    NSTableCellView *cell = [table makeViewWithIdentifier:attachment_cell_id owner:nil]
        ?: [self attachment_cell];
    NSString *path = [self attachment_path:row];
    cell.imageView.image       = self.attachment_icons[(NSUInteger)row];
    cell.textField.stringValue = path.lastPathComponent;
    cell.toolTip               = path;
    return cell;
}

/*
 * tableViewSelectionDidChange: — a list's selection moved: its buttons
 * follow.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
tableViewSelectionDidChange:(NSNotification *)note
{
    [self refresh_list_buttons];
}

/* ── The date picker ────────────────────────────────────────────────── */

/*
 * pick_date_for: — choose a date field's date from a calendar in a tool
 * panel, as Notes asks for an action item's due date: Set writes the
 * picked day, Clear removes the date.  The calendar opens on the date the
 * field holds, or on today.  Typing in the field works as well; this is
 * the other way in.
 *
 * Inputs:
 *   field — the due or the start field; Set and Clear write into it and
 *           save
 */
- (void)
pick_date_for:(NSTextField *)field
{
    int64_t       shown  = task_due_parse(field.stringValue.UTF8String);
    NSDatePicker *picker = [[NSDatePicker alloc] initWithFrame:NSZeroRect];

    picker.datePickerStyle    = NSDatePickerStyleClockAndCalendar;
    picker.datePickerElements = NSDatePickerElementFlagYearMonthDay;
    picker.dateValue          = shown != 0 ? [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)shown]
                                           : NSDate.date;
    NSSize size = picker.fittingSize;
    picker.translatesAutoresizingMaskIntoConstraints = NO;
    [picker.widthAnchor  constraintEqualToConstant:size.width].active  = YES;
    [picker.heightAnchor constraintEqualToConstant:size.height].active = YES;

    NSString *what = field == self.due_field ? @"Due Date" : @"Start Date";
    __weak TasksEditorWindow *ws = self;
    SharedToolPanel *panel = [SharedToolPanel
        panel_with_title:what hero_title:what subtitle:self.window.title symbol:@"calendar"
                 content:picker buttons:@[@"Set", @"Clear", @"Cancel"] destructive:-1
                 handler:^NSString *(NSInteger index) {
        TasksEditorWindow *s = ws;
        if (!s || s.is_shut_down || index == 2) return nil;

        /* The fields hold Gregorian dates whatever calendar the system
         * shows the picker in. */
        NSCalendar *gregorian =
            [[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
        NSDateComponents *day = [gregorian components:NSCalendarUnitYear | NSCalendarUnitMonth |
                                                      NSCalendarUnitDay
                                             fromDate:picker.dateValue];
        [field abortEditing];
        field.stringValue = index == 1 ? @"" :
            [NSString stringWithFormat:@"%04ld-%02ld-%02ld",
                (long)day.year, (long)day.month, (long)day.day];
        if (field == s.due_field) {
            [s save_now];
        } else {
            [s recur_changed];
        }
        return nil;
    }];
    [panel show_over:self.window];
}

/*
 * due_time_changed: — the due time picker changed; queue a save.
 *
 * Inputs:
 *   sender — the picker (unused)
 */
- (void)
due_time_changed:(id)sender
{
    [self queue_save];
}

/*
 * pick_due_date: — the calendar button beside the due date.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
pick_due_date:(id)sender
{
    [self pick_date_for:self.due_field];
}

/*
 * pick_start_date: — the calendar button beside the schedule's start date.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
pick_start_date:(id)sender
{
    [self pick_date_for:self.recur_start_field];
}

/* ── Text editing and keys ──────────────────────────────────────────── */

/*
 * controlTextDidChange: — a field's text changed under the user's hands.
 * The title, due date and due time queue a debounced save; a schedule
 * field goes through recur_changed.  A subtask's title waits for the edit
 * to end.  In the tag field a deleted token takes its tag off the task at
 * once (a new one is added as it is made: tokenField:shouldAddObjects:).
 *
 * Inputs:
 *   note — the notification; its object is the field
 */
- (void)
controlTextDidChange:(NSNotification *)note
{
    NSTextField *field = note.object;

    if ([self is_subtask_field:field]) return;
    if (field == self.tag_field) {
        [self drop_removed_tags];
        return;
    }

    if (field == self.title_field) {
        [self refresh_title];
        [self queue_save];
    } else if (field == self.due_field) {
        [self queue_save];
    } else if (field == self.every_field || field == self.lead_field) {
        [self count_typed:field];
    } else {
        [self recur_changed];
    }
}

/*
 * controlTextDidEndEditing: — the user left a field (Return, Tab or a click
 * elsewhere).  A subtask's title is saved.  The tag field's deleted tokens
 * take their tags off the task.  For the form's fields a pending save is
 * flushed, and the form is then shown again from the database, so
 * that a date typed as 10/4/26 reads 2026-10-04 and text that was not
 * understood gives way to the value that is stored.
 *
 * Inputs:
 *   note — the notification; its object is the field
 */
- (void)
controlTextDidEndEditing:(NSNotification *)note
{
    NSTextField *field = note.object;

    if (self.is_shut_down || self.discarded) return;
    if ([self is_subtask_field:field]) {
        [self save_subtask_title:field];
        return;
    }
    if (field == self.tag_field) [self drop_removed_tags];
    if (self.save_pending) [self save_now];

    /* On the next pass: the field still counts as being edited here, and a
     * field being edited is not rewritten. */
    __weak TasksEditorWindow *ws = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        TasksEditorWindow *s = ws;
        if (!s || s.is_shut_down || s.save_pending) return;
        if (![s reload]) [s close];
    });
}

/*
 * control:textView:doCommandBySelector: — Return and Escape in a field.
 *
 * Return in the title saves and closes the editor.  Return in the tag
 * field is the token field's own: it makes the typed text a tag and stays
 * in the field for the next one.  Return anywhere else ends the edit
 * (which saves it) and lets go of the field, instead of AppKit selecting
 * all its text again.  Escape in a subtask's title
 * abandons that edit; anywhere else it is Escape for the window (dismiss).
 *
 * Inputs:
 *   control   — the field being edited
 *   text_view — its field editor (unused)
 *   command   — the command the key means
 *
 * Output:
 *   YES when the command was handled here.
 */
- (BOOL)
control:(NSControl *)control
textView:(NSTextView *)text_view
doCommandBySelector:(SEL)command
{
    BOOL subtask = [self is_subtask_field:control];

    if (command == @selector(insertNewline:) && control == self.tag_field) return NO;
    if (command == @selector(insertNewline:)) {
        if (control == self.title_field) {
            [self save_action:nil];
        } else {
            [self.window makeFirstResponder:subtask ? self.subtask_table : nil];
        }
        return YES;
    }
    if (command == @selector(cancelOperation:)) {
        if (subtask) {
            [control abortEditing];
            [self.window makeFirstResponder:self.subtask_table];
        } else {
            [self dismiss];
        }
        return YES;
    }
    return NO;
}

/*
 * textDidChange: — the notes changed: queue a debounced save.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
textDidChange:(NSNotification *)note
{
    [self queue_save];
}

/*
 * textView:doCommandBySelector: — Tab, Shift-Tab and Escape in the notes.
 * Tab moves to the next control, as in every other field of the form
 * (Option-Tab still types a tab); Escape is Escape for the window, not the
 * text view's word completion.
 *
 * Inputs:
 *   text_view — the notes (unused)
 *   command   — the command the key means
 *
 * Output:
 *   YES when the command was handled here.
 */
- (BOOL)
textView:(NSTextView *)text_view doCommandBySelector:(SEL)command
{
    if (command == @selector(insertTab:)) {
        [self.window selectNextKeyView:nil];
    } else if (command == @selector(insertBacktab:)) {
        [self.window selectPreviousKeyView:nil];
    } else if (command == @selector(cancelOperation:)) {
        [self dismiss];
    } else {
        return NO;
    }
    return YES;
}

/*
 * cancelOperation: — Escape with no field being edited.
 *
 * Inputs:
 *   sender — the responder that passed it on (unused)
 */
- (void)
cancelOperation:(id)sender
{
    [self dismiss];
}

/*
 * dismiss — what Escape does: Cancel for a new task (the task is
 * discarded, as the button says), and for an existing one simply close the
 * editor, keeping every edit.  The editor may be released by the time this
 * returns.
 */
- (void)
dismiss
{
    if (self.is_new) {
        [self cancel_action:nil];
    } else {
        [self close];
    }
}

/* ── Closing ────────────────────────────────────────────────────────── */

/*
 * save_action: — Save, and Return in the title: flush the save and close —
 * unless the save failed, when the window stays open with the edit in it
 * and says why (unsaved_notice).  The editor may be released by the time
 * this returns.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
save_action:(id)sender
{
    if (![self save_now]) {
        [self unsaved_notice];
        return;
    }
    [self close];
}

/*
 * unsaved_notice — tell the user, on this window, that their changes could
 * not be saved and are still here.
 */
- (void)
unsaved_notice
{
    shared_ui_notice(self.window, @"Your changes could not be saved",
                    @"The changes are still in this window. Close it, or press Save, to try "
                    @"again in a moment. (The cause: " TASK_APP_WRITE_FAILED_REASON @".)", nil);
}

/*
 * windowShouldClose: — the close button or ⌘W: end the edit in progress and
 * save it first.  When that save fails the window stays open with the
 * edit in it and asks (as Notes' editor does): Close Without Saving loses
 * the edit, Cancel (the default) keeps the window.
 *
 * Inputs:
 *   sender — the window (unused)
 *
 * Output:
 *   NO while the edit could not be saved.
 */
- (BOOL)
windowShouldClose:(NSWindow *)sender
{
    if (self.discarded || self.is_shut_down) return YES;
    [self.window makeFirstResponder:nil];
    if (self.save_pending && ![self save_now]) {
        __weak TasksEditorWindow *ws = self;
        shared_ui_confirm(self.window,
                          [NSString stringWithFormat:@"Could not save \u201C%@\u201D", self.window.title],
                          @"Your changes are still in the window. Closing it now loses them. "
                          @"(The cause: " TASK_APP_WRITE_FAILED_REASON @".)",
                          @"Close Without Saving", YES, ^(BOOL confirmed) {
            TasksEditorWindow *s = ws;
            if (!s || !confirmed) return;
            s.save_pending = NO;
            s.discarded    = YES;
            [s close];
        });
        return NO;
    }
    return YES;
}

/*
 * cancel_action: — Cancel, in the editor of a task New Task has just
 * created: discard the task.  The order matters: first drop any pending
 * save and stop further ones, then close the window, and only THEN delete
 * the task, so that no save can land on the deleted row.  Closing releases
 * this controller, so what the delete needs is copied out first.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
cancel_action:(id)sender
{
    TaskApp *app     = self.app;
    int64_t  task_id = self.task_id;

    self.save_pending = NO;
    self.discarded    = YES;
    [self close];

    task_db_task_delete(app->db, task_id);
    task_app_notify_changed(app);
    task_app_status(app, "Discarded the new task");
}

/*
 * shut_down — tear the editor down as its window closes: end any edit in
 * progress and flush the pending save (unless the task is being
 * discarded; a save that still fails is reported, since a close the app
 * makes cannot be refused), end a sheet that is still up, and let go of
 * the tables.
 * Idempotent; nothing is saved after it.
 */
- (void)
shut_down
{
    if (self.is_shut_down) return;

    if (!self.discarded) {
        /* Ending the edit saves a subtask title that is still being typed
         * and flushes a pending save through controlTextDidEndEditing:; the
         * notes have no "end", so their pending save is flushed here. */
        [self.window makeFirstResponder:nil];
        if (self.save_pending && ![self save_now]) {
            /* Closed by the app (the library window went, the database
             * changed), so the window cannot stay: say what was lost,
             * once the close is over. */
            NSString *title = self.title_field.stringValue;
            dispatch_async(dispatch_get_main_queue(), ^{
                shared_ui_notice(nil, [NSString stringWithFormat:
                                      @"Changes to \u201C%@\u201D could not be saved", title],
                                @"The cause: " TASK_APP_WRITE_FAILED_REASON @".", nil);
            });
        }
    }
    self.is_shut_down = YES;
    self.save_pending = NO;
    self.save_id++;

    if (self.window.attachedSheet != nil) {
        [self.window endSheet:self.window.attachedSheet returnCode:NSModalResponseCancel];
    }
    /* The tables must not ask for rows once the row lists are freed. */
    self.subtask_table.dataSource    = nil;
    self.subtask_table.delegate      = nil;
    self.attachment_table.dataSource = nil;
    self.attachment_table.delegate   = nil;
}

/*
 * windowWillClose: — the window is closing: tear down, then leave the
 * registry, which releases this controller.  That is the last thing done
 * here, because nothing else keeps `self` alive afterwards.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [self shut_down];
    [editors() removeObjectForKey:@(self.task_id)];
}

@end

/* ── Public entry points ────────────────────────────────────────────── */

/*
 * open_editor — open a task's editor, or bring its open one forward.
 *
 * Inputs:
 *   app     — the application context
 *   task_id — the task; one that no longer exists opens nothing
 *   is_new  — YES for a task New Task has just created (adds Cancel); it
 *             has no effect on an editor that is already open
 *   corner  — YES to open a new window in the lower-right corner of the
 *             screen the user is working on rather than centred
 */
static void
open_editor(TaskApp *app, int64_t task_id, BOOL is_new, BOOL corner)
{
    TasksEditorWindow *editor = editors()[@(task_id)];
    if (editor) {
        [editor present:NO];
        return;
    }

    Task *task = task_db_task_get(app->db, task_id);
    if (task != NULL && !task->deleted) {
        editor = [[TasksEditorWindow alloc] init_with_app:app task:task is_new:is_new];
        editors()[@(task_id)] = editor;
        if (corner) shared_ui_place_lower_right(editor.window, NSScreen.mainScreen);
        [editor present:YES];
    }
    task_free(task);
}

/*
 * tasks_editor_open — open or raise a task's editor.
 * Inputs are documented in editor_window.h.
 */
void
tasks_editor_open(TaskApp *app, int64_t task_id)
{
    open_editor(app, task_id, NO, NO);
}

/*
 * tasks_editor_open_new — open the editor of a just-created task, with
 * Cancel, centred or in the corner.
 * Inputs are documented in editor_window.h.
 */
void
tasks_editor_open_new(TaskApp *app, int64_t task_id, bool in_corner)
{
    open_editor(app, task_id, YES, in_corner);
}

/*
 * tasks_editor_close — close one task's editor, if open, flushing its save.
 * Inputs are documented in editor_window.h.
 */
void
tasks_editor_close(int64_t task_id)
{
    [editors()[@(task_id)] close];
}

/*
 * tasks_editor_refresh_all — reload every open editor; close those whose
 * task has gone.  An editor with an unsaved edit saves it first: the save
 * writes only the fields the user changed, so the reload then shows those
 * as typed and every other field as the database now has it.  An editor
 * whose save fails is not reloaded, so its edit is not lost.
 */
void
tasks_editor_refresh_all(void)
{
    /* A copy: closing an editor removes it from the registry. */
    for (TasksEditorWindow *editor in editors().allValues) {
        /* An edit that could not be saved stays on screen as typed. */
        if (editor.save_pending && ![editor save_now]) continue;
        if (![editor reload]) [editor close];
    }
}

/*
 * tasks_editor_refresh_sources — every open editor's "From note" line.
 */
void
tasks_editor_refresh_sources(void)
{
    for (TasksEditorWindow *editor in editors().allValues) [editor show_source];
}

/*
 * tasks_editor_close_all — close every open editor, each flushing its save.
 */
void
tasks_editor_close_all(void)
{
    for (TasksEditorWindow *editor in editors().allValues) [editor close];
}
