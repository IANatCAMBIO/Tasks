#import "task_list.h"
#import "editor_window.h"
#import "look.h"
#import "row_text.h"
#import "ui_util.h"

#include "task_config.h"
#include "task_order.h"
#include "task_sort.h"

/*
 * HOW THE LIST IS BUILT
 *
 * A view-based NSTableView of six columns (column_specs below).  The rows
 * the window hands over (show_rows:) are never moved: what the table shows
 * is a DISPLAY ORDER over them (task_sort.h) — the saved manual order when
 * manual sorting is on, then the header sort — so everything kept per row
 * survives a sort, a drag and a checkbox tick.
 *
 * ROW HEIGHTS.  A row is as tall as its text, and laying text out is the
 * one expensive thing here: 0.1 to 0.3 ms a row, so measuring every row of
 * a 5,000-task view would stall each refresh for a second.  The table
 * therefore uses AppKit's automatic row heights: only the rows on screen
 * are measured, exactly, as their cells are filled (the Task cell carries
 * a height constraint), and tableView:heightOfRow: supplies the ESTIMATE
 * for every row that is not — the height a task had when it was last
 * measured, or a guess from its line count.  A refresh, a column resize
 * and a scroll each cost the visible rows only.
 *
 * WHAT CHANGES THE ORDER.  One method, arrange_selecting:, rebuilds the
 * display order and reloads the table, reselecting by task id (a reload
 * keeps selected row NUMBERS, which point at other tasks afterwards).
 * The checkbox is the exception: it updates its row in place, moving it
 * only if the sort says so, so that selection, scroll and hover survive.
 *
 * DRAG AND HEADER SORT COEXIST.  A row is dragged from anywhere in it but
 * the checkbox (when no search is active) and column headers always sort.
 * Dragging a row saves its new position as the manual order AND clears the
 * active header sort, so the position sticks.  Clicking a header re-sorts
 * from the saved manual order.  Each action cancels the other's residue.
 */

/* The columns, in table order.  Reordering is off and a hidden column
 * stays in the table, so these are also indexes into table.tableColumns. */
enum { col_done = 0, col_task, col_status, col_due, col_completed, col_count };

#define NO_SORT    (-1)                 /* column_spec_t.sort: not sortable    */
#define NO_COMMAND ((TasksCommand)0)    /* column_spec_t.command: not toggled  */

/*
 * column_spec_t — one column.
 *
 *   ident       — column identifier, and its cells' reuse identifier
 *   title       — header title
 *   sort        — the task_sort_column_t a header click sorts by, or NO_SORT
 *   visible_key — the setting holding its visibility, or NULL for a column
 *                 the user does not toggle
 *   command     — the command that toggles it, or NO_COMMAND
 */
typedef struct {
    NSString * __unsafe_unretained ident;
    NSString * __unsafe_unretained title;
    int          sort;
    const char  *visible_key;
    TasksCommand command;
} column_spec_t;

static const column_spec_t column_specs[col_count] = {
    [col_done]      = { @"done",      @"✓",         task_sort_done,
                        TASK_CFG_COL_DONE,      TasksCommandColumnDone },
    [col_task]      = { @"task",      @"Task",      task_sort_title,     NULL, NO_COMMAND },
    [col_status]    = { @"status",    @"Status",    task_sort_status,
                        TASK_CFG_COL_STATUS,    TasksCommandColumnStatus },
    [col_due]       = { @"due",       @"Due Date",  task_sort_due,
                        TASK_CFG_COL_DUE,       TasksCommandColumnDue },
    [col_completed] = { @"completed", @"Completed", task_sort_completed,
                        TASK_CFG_COL_COMPLETED, TasksCommandColumnCompleted },
};

/* What a manual-sort drag carries: one pasteboard item per dragged row,
 * holding its task id.  Private to the app, so nothing else accepts it. */
static NSString * const row_drag_type = @"tasks.task-row";

/* List-only metrics.  (Everything shared with another pane is in look.h.) */
static const CGFloat task_col_min_w = 120.0;    /* the Task column's floor      */
static const CGFloat data_col_min_w = 40.0;     /* Status, Due Date, Completed  */
static const CGFloat header_fit_pad = 10.0;     /* header title to column edges
                                                 * when fitting a column        */
/* The sort indicator, measured via sortIndicatorRectForBounds:, starts 17pt
 * from the right edge of the frame passed to super in drawInteriorWithFrame:.
 * That frame is already 4pt narrower on the left (tasks_look_header_inset), so
 * the title needs: col_w >= title_w + 4 (inset) + 17 (indicator) = title_w + 21.
 * header_fit_pad (10) already covers the no-indicator case; for sortable columns
 * we add the difference plus a 4pt margin.  4 + 17 - 10 + 4 = 15. */
static const CGFloat sort_indicator_w = 15.0;  /* extra width for sortable columns */
static const CGFloat label_inset    = 2.0;      /* an NSTextField draws its text
                                                 * this far inside its frame    */
static const size_t  fit_row_limit  = 2000;     /* rows measured to fit a column */

@interface TasksTaskList () <NSTableViewDataSource, NSTableViewDelegate, NSMenuDelegate>
@property (nonatomic, weak) TasksLibraryWindow *library;
@property (nonatomic, strong) NSTableView *table;
@property (nonatomic, strong) NSScrollView *scroll;
/* Set when the fitted columns may no longer fit their content (another
 * view, a column shown or hidden); the next show_rows: fits them.  Not on
 * every refresh: a fit measures text. */
@property (nonatomic, assign) BOOL fit_pending;

/* Declared here because the cell classes below call it before its
 * definition; documented where it is defined. */
- (void)cell_emphasis_changed:(NSTableCellView *)cell;
@end

/* ── The table and its cells ─────────────────────────────────────────── */

/*
 * TasksListTable — the list's table view.  A subclass for one reason: a
 * row drag may not begin on the checkbox, and AppKit asks the table
 * itself, not its delegate, where a drag may begin.
 */
@interface TasksListTable : NSTableView
@end

@implementation TasksListTable

/*
 * canDragRowsWithIndexes:atPoint: — AppKit asks whether a mouse-down may
 * start dragging rows.  Anywhere in the row but the checkbox, whose click
 * is a toggle, not a grab.
 *
 * Inputs:
 *   indexes — the rows that would be dragged (unused)
 *   point   — the mouse-down point, in the table
 *
 * Output:
 *   YES when the point is outside the done column.
 */
- (BOOL)
canDragRowsWithIndexes:(NSIndexSet *)indexes atPoint:(NSPoint)point
{
    return [self columnAtPoint:point] != col_done;
}

@end

/*
 * TasksHeaderCell — column header cell with a wider leading inset so the
 * title aligns with the row content below it.
 */
@interface TasksHeaderCell : NSTableHeaderCell
@end

@implementation TasksHeaderCell

- (void)drawInteriorWithFrame:(NSRect)frame inView:(NSView *)view
{
    /* Shift the leading edge inward; right edge stays fixed so the sort
     * indicator position is unchanged. */
    frame.origin.x   += tasks_look_header_inset;
    frame.size.width -= tasks_look_header_inset;
    [super drawInteriorWithFrame:frame inView:view];
}

@end

/*
 * TasksListCell — a cell that tells the list when its row's emphasis
 * changes (selected or not, in a key window or not), so the list can redraw
 * what does not follow by itself: the row text's colour and the Due tint.
 */
@interface TasksListCell : NSTableCellView
@property (nonatomic, weak) TasksTaskList *list;
@end

@implementation TasksListCell

/*
 * setBackgroundStyle: — AppKit reports the row's emphasis.
 *
 * Inputs:
 *   style — emphasized for a selected row of a focused table in the key
 *           window, else normal
 */
- (void)
setBackgroundStyle:(NSBackgroundStyle)style
{
    [super setBackgroundStyle:style];
    [self.list cell_emphasis_changed:self];
}

@end

/*
 * TasksRowTextCell — the tall Task cell.  It draws the shared renderer's
 * string itself, with the same string-drawing options tasks_row_text_height
 * measures with, so the text always fits the height that was measured for
 * it (a text field lays text out by slightly different rules).  Its height
 * constraint is what gives the row its height.
 */
@interface TasksRowTextCell : TasksListCell
@property (nonatomic, copy) NSAttributedString *text;       /* from tasks_row_text */
@property (nonatomic, assign) CGFloat text_height;          /* of text, at the cell's width */
@property (nonatomic, strong) NSLayoutConstraint *height;   /* text_height + padding */
@end

@implementation TasksRowTextCell

/*
 * initWithFrame: — build the cell with its height constraint.
 *
 * Inputs:
 *   frame — the initial frame
 *
 * Output:
 *   the cell.
 */
- (instancetype)
initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;

    /* Just below required: while the table is moving a row between its
     * estimated and its real height the cell's frame briefly disagrees. */
    _height          = [self.heightAnchor constraintEqualToConstant:2.0 * tasks_look_row_pad_v];
    _height.priority = NSLayoutPriorityRequired - 1;
    _height.active   = YES;
    return self;
}

/*
 * isFlipped — the text is laid out from the top of the cell downwards.
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
 * setText: — show another string.
 *
 * Inputs:
 *   text — the string to draw
 */
- (void)
setText:(NSAttributedString *)text
{
    _text = [text copy];
    self.needsDisplay = YES;
}

/*
 * setText_height: — the height the text needs at this cell's width; the
 * row follows through the height constraint.
 *
 * Inputs:
 *   text_height — from tasks_row_text_height
 */
- (void)
setText_height:(CGFloat)text_height
{
    _text_height         = text_height;
    self.height.constant = text_height + 2.0 * tasks_look_row_pad_v;
}

/*
 * drawRect: — draw the text inside the row's padding.
 *
 * Inputs:
 *   dirty — the rectangle needing display (unused: the cell is drawn whole)
 */
- (void)
drawRect:(NSRect)dirty
{
    [self.text drawWithRect:NSInsetRect(self.bounds, tasks_look_row_pad_h, tasks_look_row_pad_v)
                    options:NSStringDrawingUsesLineFragmentOrigin |
                            NSStringDrawingUsesFontLeading];
}

/*
 * isAccessibilityElement — the cell has no text field for VoiceOver to
 * find, so it presents itself as the row's text.
 *
 * Output:
 *   YES.
 */
- (BOOL)
isAccessibilityElement
{
    return YES;
}

/*
 * accessibilityRole — what the cell is to assistive apps.
 *
 * Output:
 *   static text.
 */
- (NSAccessibilityRole)
accessibilityRole
{
    return NSAccessibilityStaticTextRole;
}

/*
 * accessibilityValue — what the cell says.
 *
 * Output:
 *   the row's text, without its styling.
 */
- (id)
accessibilityValue
{
    return self.text.string;
}

@end

/* ── Cell text and factories ─────────────────────────────────────────── */

/*
 * label_font — the font of the Status, Due Date and Completed cells.  One
 * function, so the cells and the column fit measure the same thing.
 *
 * Output:
 *   the font.
 */
static NSFont *
label_font(void)
{
    return [NSFont systemFontOfSize:tasks_look_row_font];
}

/*
 * label_text — what a Status, Due Date or Completed cell says.  One
 * function, shared by the cell and the column fit.
 *
 * Inputs:
 *   row    — the row
 *   column — col_status, col_due or col_completed
 *
 * Output:
 *   borrowed text; "" when the row has no value for the column.
 */
static const char *
label_text(const task_row_t *row, NSInteger column)
{
    switch (column) {
    case col_status:    return row->status_text;
    case col_due:       return row->due_text;
    case col_completed: return row->completed_text;
    default:            return "";
    }
}

/*
 * top_label — a one-line label placed in a cell at the height of the row
 * text's first line.
 *
 * Inputs:
 *   cell — the cell to add it to
 *   text — its initial text
 *
 * Output:
 *   the label, already a subview of the cell with its top pinned.
 */
static NSTextField *
top_label(NSTableCellView *cell, NSString *text)
{
    NSTextField *label = [NSTextField labelWithString:text];

    label.font          = label_font();
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    [cell addSubview:label];
    [label.topAnchor constraintEqualToAnchor:cell.topAnchor
                                    constant:tasks_look_row_pad_v].active = YES;
    return label;
}

/*
 * make_label_cell — a Status, Due Date or Completed cell.
 *
 * Inputs:
 *   list — the list the cell reports emphasis changes to
 *
 * Output:
 *   new cell whose textField is its label.
 */
static NSTableCellView *
make_label_cell(TasksTaskList *list)
{
    TasksListCell *cell  = [[TasksListCell alloc] initWithFrame:NSZeroRect];
    NSTextField   *label = top_label(cell, @"");

    /* A column dragged narrower than its text truncates it. */
    [label setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                    forOrientation:NSLayoutConstraintOrientationHorizontal];
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor
                                            constant:tasks_look_row_pad_h],
        [label.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor
                                             constant:-tasks_look_row_pad_h],
    ]];
    cell.list      = list;
    cell.textField = label;
    return cell;
}

/*
 * make_check_cell — the done checkbox.  The button is the cell's only
 * subview, which is how fill_cell:column:row: finds it.
 *
 * Inputs:
 *   list — the list, which receives done_clicked:
 *
 * Output:
 *   new cell.
 */
static NSTableCellView *
make_check_cell(TasksTaskList *list)
{
    NSTableCellView *cell  = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    NSButton        *check = [NSButton checkboxWithTitle:@""
                                                  target:list
                                                  action:@selector(done_clicked:)];

    check.translatesAutoresizingMaskIntoConstraints = NO;
    [cell addSubview:check];
    [NSLayoutConstraint activateConstraints:@[
        [check.centerXAnchor constraintEqualToAnchor:cell.centerXAnchor],
        [check.topAnchor constraintEqualToAnchor:cell.topAnchor
                                        constant:tasks_look_row_pad_v],
    ]];
    return cell;
}

/*
 * estimated_text_height — a guess at a row's text height from its line
 * count alone, for a row that has never been laid out: every line taken as
 * one unwrapped line of plain text.  Emoji and wrapped lines make the real
 * height a little more; the row is measured exactly when it is shown.
 *
 * Inputs:
 *   line_count — the row's lines, title included
 *
 * Output:
 *   the estimated height in points.
 */
static CGFloat
estimated_text_height(size_t line_count)
{
    static CGFloat         title_h;
    static CGFloat         small_h;
    static dispatch_once_t once;

    dispatch_once(&once, ^{
        NSLayoutManager *layout = [[NSLayoutManager alloc] init];
        title_h = [layout defaultLineHeightForFont:[NSFont systemFontOfSize:tasks_look_row_font]];
        small_h = [layout defaultLineHeightForFont:
                      [NSFont systemFontOfSize:tasks_look_row_small_font]] +
                  tasks_look_row_line_gap;
    });
    return ceil(title_h + (line_count > 1 ? line_count - 1 : 0) * small_h);
}

/*
 * row_background — a row's background: the stripe tint on every second
 * row, the table's own background on the others.  (A selected row draws
 * its selection over it.)
 *
 * Inputs:
 *   table — the table
 *   row   — the row index
 *
 * Output:
 *   the colour.
 */
static NSColor *
row_background(NSTableView *table, NSInteger row)
{
    return row % 2 == 1 ? tasks_color(TasksColorRowStripe) : table.backgroundColor;
}

/*
 * spec_for_command — the column a visibility command toggles.
 *
 * Inputs:
 *   command — a command (never NO_COMMAND, which is not one)
 *
 * Output:
 *   the column's spec, or NULL when the command toggles no column.
 */
static const column_spec_t *
spec_for_command(TasksCommand command)
{
    for (int i = 0; i < col_count; i++) {
        if (column_specs[i].command == command) return &column_specs[i];
    }
    return NULL;
}

@implementation TasksTaskList {
    task_row_list_t *_rows;         /* owned, in the order the window gave
                                     * them; NULL while the pane is hidden  */
    int             *_order;        /* owned display order: table row i
                                     * shows _rows->rows[_order[i]]         */
    CGFloat         *_heights;      /* owned, per _rows index: the text
                                     * height last measured for that task;
                                     * 0 = never measured                   */
    task_sort_key_t  _keys[TASK_SORT_MAX_KEYS];     /* the header sort      */
    size_t           _n_keys;       /* 0 = unsorted                         */
    BOOL             _clearing_sort_from_drag;      /* suppress arrange in
                                                     * sortDescriptorsDidChange:
                                                     * when a drag clears the
                                                     * sort descriptor        */
}

/* ── Construction ────────────────────────────────────────────────────── */

/*
 * init_with_library: — build the pane.
 * Inputs/output are documented in task_list.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library     = library;
    _fit_pending = YES;
    [self build_table];
    [self build_menus];
    /* The columns must know what they sort by before the table is given
     * a sort to show. */
    [self apply_settings];
    [self restore_sort];
    return self;
}

/*
 * dealloc — a pane released without shut_down still frees what it owns.
 */
- (void)
dealloc
{
    task_row_list_free(_rows);
    free(_order);
    free(_heights);
}

/*
 * build_table — the table, its columns and the scroll view round it.
 */
- (void)
build_table
{
    self.table = [[TasksListTable alloc] initWithFrame:NSZeroRect];
    self.table.style                    = NSTableViewStylePlain;
    self.table.rowSizeStyle             = NSTableViewRowSizeStyleCustom;
    self.table.usesAutomaticRowHeights  = YES;
    /* No spacing: the cells pad themselves (look.h), so a cell is exactly
     * as wide as its column and a row exactly as tall as its Task cell. */
    self.table.intercellSpacing         = NSZeroSize;
    self.table.allowsMultipleSelection  = YES;
    self.table.allowsColumnReordering   = NO;
    /* Only the Task column carries the autoresizing mask, so it alone
     * takes up a change of the pane's width. */
    self.table.columnAutoresizingStyle  = NSTableViewUniformColumnAutoresizingStyle;
    self.table.dataSource               = self;
    self.table.delegate                 = self;
    self.table.target                   = self;
    self.table.doubleAction             = @selector(row_double_clicked:);

    for (int i = 0; i < col_count; i++) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:column_specs[i].ident];

        column.title = column_specs[i].title;
        if (i == col_done) {
            column.width        = tasks_look_check_col_w;
            column.minWidth     = tasks_look_check_col_w;
            column.maxWidth     = tasks_look_check_col_w;
            column.resizingMask = NSTableColumnNoResizing;
            column.headerCell.alignment = NSTextAlignmentCenter;
        } else if (i == col_task) {
            column.headerCell   = [[TasksHeaderCell alloc] initTextCell:column.title];
            column.minWidth     = task_col_min_w;
            column.resizingMask = NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask;
        } else {
            column.headerCell   = [[TasksHeaderCell alloc] initTextCell:column.title];
            column.minWidth     = data_col_min_w;
            column.resizingMask = NSTableColumnUserResizingMask;
        }
        [self.table addTableColumn:column];
    }

    /* Rows are dragged within this table only. */
    [self.table registerForDraggedTypes:@[row_drag_type]];
    [self.table setDraggingSourceOperationMask:NSDragOperationMove forLocal:YES];
    [self.table setDraggingSourceOperationMask:NSDragOperationNone forLocal:NO];

    self.scroll = [[NSScrollView alloc] initWithFrame:self.bounds];
    self.scroll.autoresizingMask      = NSViewWidthSizable | NSViewHeightSizable;
    self.scroll.hasVerticalScroller   = YES;
    self.scroll.hasHorizontalScroller = YES;
    self.scroll.autohidesScrollers    = YES;
    self.scroll.documentView          = self.table;
    [self addSubview:self.scroll];
}

/*
 * build_menus — the two context menus: the rows' (filled each time it
 * opens, menuNeedsUpdate:) and the header's, which shows and hides columns.
 */
- (void)
build_menus
{
    self.table.menu          = [[NSMenu alloc] initWithTitle:@""];
    self.table.menu.delegate = self;

    /* The items are commands like any other: the window validates them,
     * which is where their check marks come from (update_state:). */
    NSMenu *columns = [[NSMenu alloc] initWithTitle:@""];
    for (int i = 0; i < col_count; i++) {
        if (column_specs[i].command == NO_COMMAND) continue;
        [columns addItem:tasks_command_menu_item(column_specs[i].command, nil, self.library)];
    }
    self.table.headerView.menu = columns;
}

/*
 * restore_sort — put the header sort saved by the last run back on the
 * table.  The table then reports it like a header click
 * (tableView:sortDescriptorsDidChange:), which is the one place the sort
 * is taken up.
 */
- (void)
restore_sort
{
    task_sort_key_t keys[TASK_SORT_MAX_KEYS];
    gchar  *saved  = task_config_get(TASK_CFG_LIST_SORT);
    size_t  n_keys = task_sort_keys_parse(saved, keys);
    NSMutableArray<NSSortDescriptor *> *descriptors = [NSMutableArray array];

    for (size_t i = 0; i < n_keys; i++) {
        [descriptors addObject:
            [NSSortDescriptor sortDescriptorWithKey:tasks_string(task_sort_column_name(keys[i].column))
                                          ascending:keys[i].ascending]];
    }
    g_free(saved);
    self.table.sortDescriptors = descriptors;
}

/* ── Rows and their order ────────────────────────────────────────────── */

/*
 * row_count — how many rows are shown.
 *
 * Output:
 *   the count; 0 while the pane is hidden.
 */
- (NSInteger)
row_count
{
    return _rows != NULL ? (NSInteger)_rows->count : 0;
}

/*
 * row_at: — the task shown at a table row.
 *
 * Inputs:
 *   row — a table row, 0 .. row_count - 1
 *
 * Output:
 *   the row description (borrowed).
 */
- (const task_row_t *)
row_at:(NSInteger)row
{
    return _rows->rows[_order[row]];
}

/*
 * rebuild_order — work the display order out again from the rows: the
 * saved manual order (if any), then the header sort applied on top.
 * Reading the saved order is always allowed, search or no search
 * (task_order.h).
 */
- (void)
rebuild_order
{
    free(_order);
    _order = NULL;
    if (_rows == NULL) return;

    TasksLibraryWindow *library = self.library;
    int n = (int)_rows->count;

    int64_t *ids = malloc((size_t)(n > 0 ? n : 1) * sizeof *ids);
    if (ids == NULL) abort();
    for (int i = 0; i < n; i++) ids[i] = _rows->rows[i]->id;
    _order = task_order_load(TASK_CFG_FAMILY_MANUAL, library.selection, ids, n);
    free(ids);

    if (_order == NULL) {
        _order = malloc((size_t)(n > 0 ? n : 1) * sizeof *_order);
        if (_order == NULL) abort();
        for (int i = 0; i < n; i++) _order[i] = i;
    }
    task_sort_apply(_rows->rows, _order, n, _keys, _n_keys);
}

/*
 * selected_id_set — the selected tasks' ids, for finding them again after
 * the rows have moved.
 *
 * Output:
 *   the ids; empty when nothing is selected.
 */
- (NSSet<NSNumber *> *)
selected_id_set
{
    return [NSSet setWithArray:[self selected_task_ids]];
}

/*
 * arrange_selecting: — rebuild the display order, reload the table and
 * select the given tasks where they now are.  The one path by which the
 * table's rows change wholesale.
 *
 * Inputs:
 *   selected — ids of the tasks to select; those no longer shown are
 *              dropped
 */
- (void)
arrange_selecting:(NSSet<NSNumber *> *)selected
{
    [self rebuild_order];
    [self.table reloadData];

    NSMutableIndexSet *indexes = [NSMutableIndexSet indexSet];
    if (selected.count > 0) {
        for (NSInteger row = 0; row < [self row_count]; row++) {
            if ([selected containsObject:@([self row_at:row]->id)]) {
                [indexes addIndex:(NSUInteger)row];
            }
        }
    }
    [self.table selectRowIndexes:indexes byExtendingSelection:NO];
}

/*
 * measured_heights — the text heights measured so far, by task id, so that
 * the next rows of the same tasks start from them.
 *
 * Output:
 *   task id -> text height, for every row that has been measured.
 */
- (NSDictionary<NSNumber *, NSNumber *> *)
measured_heights
{
    NSMutableDictionary<NSNumber *, NSNumber *> *heights = [NSMutableDictionary dictionary];

    for (NSInteger i = 0; i < [self row_count]; i++) {
        if (_heights[i] > 0.0) heights[@(_rows->rows[i]->id)] = @(_heights[i]);
    }
    return heights;
}

/* ── TasksTaskPane ───────────────────────────────────────────────────── */

/*
 * show_rows: — replace the rows, keeping the selection by task id and the
 * scroll position.
 * The contract is documented in library_window.h.
 */
- (void)
show_rows:(task_row_list_t *)rows
{
    /* First, while the table and the rows still agree: this may clear the
     * header sort, which re-arranges what is on screen. */
    [self apply_settings];

    NSSet<NSNumber *> *selected = [self selected_id_set];
    NSDictionary<NSNumber *, NSNumber *> *measured = [self measured_heights];

    task_row_list_free(_rows);
    free(_heights);
    _rows    = rows;
    _heights = NULL;
    if (rows != NULL) {
        _heights = calloc(rows->count > 0 ? rows->count : 1, sizeof *_heights);
        if (_heights == NULL) abort();
        /* A task keeps the height it had: an estimate that is exact unless
         * its text changed, which keeps the scroll position steady. */
        for (size_t i = 0; i < rows->count; i++) {
            _heights[i] = measured[@(rows->rows[i]->id)].doubleValue;
        }
    }
    [self arrange_selecting:selected];
    [self fit_columns_if_pending];
}

/*
 * selected_task_ids — the selected rows' task ids, top to bottom.
 * Output is documented in library_window.h.
 */
- (NSArray<NSNumber *> *)
selected_task_ids
{
    NSMutableArray<NSNumber *> *ids = [NSMutableArray array];
    NSInteger count = [self row_count];

    [self.table.selectedRowIndexes enumerateIndexesUsingBlock:^(NSUInteger row, BOOL *stop) {
        if ((NSInteger)row < count) [ids addObject:@([self row_at:(NSInteger)row]->id)];
    }];
    return ids;
}

/*
 * view_changed — another sidebar row was selected: the old view's selection
 * and scroll position do not carry over, and the columns are fitted to the
 * new view's content.
 */
- (void)
view_changed
{
    [self.table deselectAll:nil];
    [self.table scrollPoint:NSZeroPoint];
    self.fit_pending = YES;
}

/*
 * cancel_drag — nothing to abandon.  A row drag is AppKit's own drag
 * session and the list keeps no state for it: the dragged ids ride on the
 * pasteboard and are matched against the rows on screen only when dropped,
 * and a drop on a hidden or emptied pane is refused.
 */
- (void)
cancel_drag
{
}

/*
 * shut_down — free the rows; the table is emptied so nothing draws them.
 */
- (void)
shut_down
{
    [self show_rows:NULL];
}

/* ── Settings the list draws by ──────────────────────────────────────── */

/*
 * apply_settings — make the columns match the settings: which are visible.
 * Column headers always sort; whether rows may be dragged is decided per
 * drag (pasteboardWriterForRow:), not by any column.
 */
- (void)
apply_settings
{
    for (int i = 0; i < col_count; i++) {
        const column_spec_t *spec   = &column_specs[i];
        NSTableColumn       *column = self.table.tableColumns[(NSUInteger)i];
        BOOL hidden = spec->visible_key != NULL && !task_config_get_bool(spec->visible_key);

        if (column.hidden != hidden) {
            column.hidden    = hidden;
            self.fit_pending = YES;
        }
        column.sortDescriptorPrototype = spec->sort == NO_SORT
            ? nil
            : [NSSortDescriptor
                  sortDescriptorWithKey:tasks_string(
                                            task_sort_column_name((task_sort_column_t)spec->sort))
                              ascending:YES];
    }
}

/*
 * text_width — the width the row text wraps in: the Task column less the
 * padding either side.
 *
 * Output:
 *   the width in points.
 */
- (CGFloat)
text_width
{
    return self.table.tableColumns[col_task].width - 2.0 * tasks_look_row_pad_h;
}

/*
 * fitted_width: — the width a Status, Due Date or Completed column needs
 * to show its header and its widest cell.  Measured text is rounded up: a
 * label a fraction of a point too narrow truncates.
 *
 * Inputs:
 *   index — col_status, col_due or col_completed
 *
 * Output:
 *   the width in points.
 */
- (CGFloat)
fitted_width:(int)index
{
    NSTableColumn *column = self.table.tableColumns[(NSUInteger)index];
    NSDictionary<NSAttributedStringKey, id> *attrs = @{ NSFontAttributeName: label_font() };
    CGFloat widest = 0.0;

    if (index == col_status) {
        /* Three possible labels: no need to look at the rows. */
        for (int status = 0; status < TASK_STATUS_N_VALUES; status++) {
            NSString *text = tasks_string(task_status_label((TaskStatus)status));
            widest = MAX(widest, [text sizeWithAttributes:attrs].width);
        }
    } else {
        size_t limit = MIN(_rows->count, fit_row_limit);
        for (size_t i = 0; i < limit; i++) {
            const char *text = label_text(_rows->rows[i], index);
            if (text[0] == '\0') continue;
            widest = MAX(widest, [tasks_string(text) sizeWithAttributes:attrs].width);
        }
    }

    CGFloat cell_w   = ceil(widest) + 2.0 * (label_inset + tasks_look_row_pad_h);
    CGFloat title_w  = ceil([column.title sizeWithAttributes:
                                @{ NSFontAttributeName: column.headerCell.font }].width);
    /* Use the column spec's sort field rather than sortDescriptorPrototype:
     * the prototype may not be set yet when the first fit runs at startup. */
    BOOL can_sort = column_specs[index].sort != NO_SORT
                    && ![self.library manual_sort_live];
    CGFloat indicator = can_sort ? sort_indicator_w : 0.0;
    CGFloat header_w  = title_w + header_fit_pad + indicator;
    return MAX(column.minWidth, MAX(cell_w, header_w));
}

/*
 * fit_columns_if_pending — size the Status, Due Date and Completed columns
 * to their content and give the Task column exactly what is left.
 *
 * Two modes:
 *   full fit (fit_pending YES) — every data column is reset to its fitted
 *       width, even if that shrinks it, and the Task column is recalculated.
 *   widen-only (fit_pending NO) — each data column is widened when new
 *       content is wider than the current width; the Task column shrinks to
 *       compensate only when a column widened.
 *
 * Widen-only runs on every show_rows: so that a task whose due time is
 * changed after the initial fit does not stay truncated.
 */
- (void)
fit_columns_if_pending
{
    if (_rows == NULL) return;
    if (self.scroll.contentSize.width <= 0.0) {
        __weak TasksTaskList *ws = self;
        dispatch_async(dispatch_get_main_queue(), ^{ [ws fit_columns_if_pending]; });
        return;
    }

    BOOL full_fit = self.fit_pending;
    self.fit_pending = NO;

    CGFloat others  = 0.0;
    BOOL    changed = NO;
    for (int i = 0; i < col_count; i++) {
        NSTableColumn *column = self.table.tableColumns[(NSUInteger)i];
        if (i == col_task || column.hidden) continue;
        if (i >= col_status) {
            CGFloat w = [self fitted_width:i];
            if (full_fit || w > column.width) {
                column.width = w;
                changed = YES;
            }
        }
        others += column.width;
    }

    if (!changed) return;
    NSTableColumn *task = self.table.tableColumns[col_task];
    task.width = MAX(task.minWidth, self.scroll.contentSize.width - others);
}

/* ── Table: rows and cells ───────────────────────────────────────────── */

/*
 * numberOfRowsInTableView: — how many rows.
 *
 * Inputs:
 *   table — the table (unused)
 *
 * Output:
 *   the row count.
 */
- (NSInteger)
numberOfRowsInTableView:(NSTableView *)table
{
    return [self row_count];
}

/*
 * tableView:heightOfRow: — the ESTIMATED height of a row that is not on
 * screen (rows on screen take their height from their Task cell): what the
 * task measured last time, else a guess from its line count.
 *
 * Inputs:
 *   table — the table (unused)
 *   row   — the row index
 *
 * Output:
 *   the height in points.
 */
- (CGFloat)
tableView:(NSTableView *)table heightOfRow:(NSInteger)row
{
    CGFloat text_height = _heights[_order[row]];

    if (text_height <= 0.0) text_height = estimated_text_height([self row_at:row]->line_count);
    return text_height + 2.0 * tasks_look_row_pad_v;
}

/*
 * fill_cell:column:row: — make a cell show its row.  Used when a cell
 * comes on screen and to update a row in place.
 *
 * Inputs:
 *   cell   — a cell made for that column
 *   column — the column index
 *   row    — the row index
 */
- (void)
fill_cell:(NSTableCellView *)cell column:(NSInteger)column row:(NSInteger)row
{
    const task_row_t *task = [self row_at:row];
    BOOL emphasized = cell.backgroundStyle == NSBackgroundStyleEmphasized;

    switch (column) {
    case col_done:
        ((NSButton *)cell.subviews.firstObject).state =
            task->status == TASK_STATUS_DONE ? NSControlStateValueOn : NSControlStateValueOff;
        break;
    case col_task: {
        TasksRowTextCell *text_cell = (TasksRowTextCell *)cell;
        text_cell.text        = tasks_row_text(task, emphasized, TasksRowLayoutWrap);
        text_cell.text_height = tasks_row_text_height(text_cell.text, [self text_width]);
        _heights[_order[row]] = text_cell.text_height;
        break;
    }
    case col_status:
    case col_due:
    case col_completed:
        cell.textField.stringValue = tasks_string(label_text(task, column));
        if (column == col_due) {
            /* The tint gives way to the selection's text colour: red on
             * the accent colour cannot be read. */
            cell.textField.textColor = emphasized ? NSColor.alternateSelectedControlTextColor
                                                  : tasks_due_color(task->due_urgency);
        }
        break;
    default:
        break;                      /* the handle looks the same on every row */
    }
}

/*
 * cell_emphasis_changed: — a cell's row became, or stopped being, a
 * selected row of the focused table: redraw the two things that follow the
 * emphasis only when told to, the row text's colour and the Due tint.
 *
 * Inputs:
 *   cell — a TasksListCell; one not in the table (still being made) is
 *          ignored
 */
- (void)
cell_emphasis_changed:(NSTableCellView *)cell
{
    NSInteger row    = [self.table rowForView:cell];
    NSInteger column = [self.table columnForView:cell];

    if (row < 0 || row >= [self row_count] || column < 0) return;

    if (column == col_task) {
        /* Emphasis changes the colour only, never the height. */
        ((TasksRowTextCell *)cell).text = tasks_row_text(
            [self row_at:row], cell.backgroundStyle == NSBackgroundStyleEmphasized,
            TasksRowLayoutWrap);
    } else {
        [self fill_cell:cell column:column row:row];
    }
}

/*
 * tableView:viewForTableColumn:row: — a cell for a row, reused when one is
 * free.
 *
 * Inputs:
 *   table  — the table
 *   column — the cell's column
 *   row    — the row index
 *
 * Output:
 *   the cell view.
 */
- (NSView *)
tableView:(NSTableView *)table
viewForTableColumn:(NSTableColumn *)column
              row:(NSInteger)row
{
    NSInteger index = (NSInteger)[table.tableColumns indexOfObjectIdenticalTo:column];
    NSTableCellView *cell = [table makeViewWithIdentifier:column.identifier owner:nil];

    if (!cell) {
        if (index == col_done) {
            cell = make_check_cell(self);
        } else if (index == col_task) {
            TasksRowTextCell *text_cell = [[TasksRowTextCell alloc] initWithFrame:NSZeroRect];
            text_cell.list = self;
            cell = text_cell;
        } else {
            cell = make_label_cell(self);
        }
        cell.identifier = column.identifier;
    }
    [self fill_cell:cell column:index row:row];
    return cell;
}

/*
 * note_heights_changed: — have the table take up new row heights, at once:
 * a height that follows the text must not animate behind it.
 *
 * Inputs:
 *   rows — the rows whose height changed
 */
- (void)
note_heights_changed:(NSIndexSet *)rows
{
    [NSAnimationContext beginGrouping];
    NSAnimationContext.currentContext.duration = 0.0;
    [self.table noteHeightOfRowsWithIndexesChanged:rows];
    [NSAnimationContext endGrouping];
}

/*
 * tableViewColumnDidResize: — the Task column's width changed (the window,
 * the sidebar or another column was resized): the text wraps differently,
 * so the rows on screen are measured again.  Rows off screen are measured
 * when they are shown.
 *
 * Inputs:
 *   note — NSTableViewColumnDidResizeNotification; its user info names the
 *          column
 */
- (void)
tableViewColumnDidResize:(NSNotification *)note
{
    if (note.userInfo[@"NSTableColumn"] != self.table.tableColumns[col_task]) return;

    NSMutableIndexSet *rows = [NSMutableIndexSet indexSet];
    NSInteger count = [self row_count];
    CGFloat   width = [self text_width];

    [self.table enumerateAvailableRowViewsUsingBlock:^(NSTableRowView *row_view, NSInteger row) {
        TasksRowTextCell *cell = [row_view viewAtColumn:col_task];
        if (cell == nil || row >= count) return;

        cell.text_height = tasks_row_text_height(cell.text, width);
        self->_heights[self->_order[row]] = cell.text_height;
        [rows addIndex:(NSUInteger)row];
    }];
    [self note_heights_changed:rows];
}

/*
 * stripe_rows — give every row on screen its background again, after a row
 * moved and the rows between changed from odd to even.
 */
- (void)
stripe_rows
{
    NSTableView *table = self.table;

    [table enumerateAvailableRowViewsUsingBlock:^(NSTableRowView *row_view, NSInteger row) {
        row_view.backgroundColor = row_background(table, row);
    }];
}

/*
 * tableView:didAddRowView:forRow: — a row came on screen: stripe it.  Row
 * views are reused, so the background is set either way.
 *
 * Inputs:
 *   table    — the table
 *   row_view — the row's view
 *   row      — the row index
 */
- (void)
tableView:(NSTableView *)table didAddRowView:(NSTableRowView *)row_view forRow:(NSInteger)row
{
    row_view.backgroundColor = row_background(table, row);
}

/* ── Header sorting ──────────────────────────────────────────────────── */

/*
 * tableView:sortDescriptorsDidChange: — the header sort changed: a header
 * was clicked, or the list set the table's sort descriptors itself
 * (restore_sort, apply_settings).  THE place the sort is taken up: spell
 * the descriptors the way task_sort.h saves a sort, remember it for the
 * next run, and re-arrange the rows.
 *
 * Inputs:
 *   table — the table
 *   old   — the previous descriptors (unused)
 */
- (void)
tableView:(NSTableView *)table sortDescriptorsDidChange:(NSArray<NSSortDescriptor *> *)old
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];

    for (NSSortDescriptor *descriptor in table.sortDescriptors) {
        [names addObject:descriptor.ascending ? descriptor.key
                                              : [@"-" stringByAppendingString:descriptor.key]];
    }
    NSString *text = [names componentsJoinedByString:@","];

    _n_keys = task_sort_keys_parse(text.UTF8String, _keys);
    task_config_set(TASK_CFG_LIST_SORT, _n_keys > 0 ? text.UTF8String : NULL);
    if (!_clearing_sort_from_drag) [self arrange_selecting:[self selected_id_set]];
}

/* ── The done checkbox ───────────────────────────────────────────────── */

/*
 * refresh_everything_later — ask the window for a full refresh on the next
 * run-loop pass.  Not now: the refresh rebuilds the table, and the caller
 * is inside the action of a button in one of its cells.
 */
- (void)
refresh_everything_later
{
    __weak TasksLibraryWindow *weak_library = self.library;

    dispatch_async(dispatch_get_main_queue(), ^{
        [weak_library full_refresh];
    });
}

/*
 * done_clicked: — a row's checkbox was clicked: ticking completes the
 * task, unticking puts it back In Progress (task_status_apply_done).
 *
 * The row is then updated IN PLACE — described again and its cells
 * refilled, moved only if the header sort now puts it elsewhere — so the
 * selection, the scroll position and every other row stay as they are.
 * Only a task that must VANISH (completed while completed tasks are
 * hidden) takes a full refresh.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
done_clicked:(NSButton *)sender
{
    TasksLibraryWindow *library = self.library;
    NSInteger row = [self.table rowForView:sender];

    if (row < 0 || row >= [self row_count] || !library) return;

    int               base    = _order[row];
    const task_row_t *old     = _rows->rows[base];
    int64_t           task_id = old->id;
    BOOL              done    = old->status != TASK_STATUS_DONE;

    task_db_task_set_status(library.app->db, task_id, task_status_apply_done(old->status, done));
    if (done) {
        task_app_status(library.app, "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 Completed", old->title);
    }

    /* Fetched again for the completion date the database just stamped. */
    Task *task = task_db_task_get(library.app->db, task_id);
    if (task == NULL || (done && !task_config_get_bool(TASK_CFG_SHOW_COMPLETED))) {
        task_free(task);
        [self refresh_everything_later];
        return;
    }

    TaskRowCtx ctx;
    task_row_ctx_init(library.app, &ctx, task_sel_is_virtual(library.selection));
    task_row_free(_rows->rows[base]);
    _rows->rows[base] = task_row_describe(task, &ctx);
    task_row_ctx_clear(&ctx);
    task_free(task);

    /* Where the sort puts the row now: Done, Status and Completed are all
     * sort keys.  Every other row keeps its place relative to the rest. */
    [self rebuild_order];
    NSInteger moved_to = row;
    for (NSInteger i = 0; i < [self row_count]; i++) {
        if (_order[i] == base) moved_to = i;
    }
    if (moved_to != row) {
        [self.table moveRowAtIndex:row toIndex:moved_to];
        [self stripe_rows];
    }

    for (NSInteger column = 0; column < col_count; column++) {
        NSTableCellView *cell = [self.table viewAtColumn:column row:moved_to makeIfNecessary:NO];
        if (cell != nil) [self fill_cell:cell column:column row:moved_to];
    }
    [self note_heights_changed:[NSIndexSet indexSetWithIndex:(NSUInteger)moved_to]];

    /* A completed subtask may have changed what its parent's editor shows. */
    tasks_editor_refresh_all();
}

/* ── Opening the editor ──────────────────────────────────────────────── */

/*
 * row_double_clicked: — open the editor for the row that was double-clicked.
 *
 * Inputs:
 *   sender — the table (unused)
 */
- (void)
row_double_clicked:(id)sender
{
    NSInteger row = self.table.clickedRow;
    TasksLibraryWindow *library = self.library;

    if (row < 0 || row >= [self row_count] || !library) return;
    tasks_editor_open(library.app, [self row_at:row]->id);
}

/*
 * keyDown: — Return (or Enter) opens the selected task's editor, through
 * the same command as Info… so there is one implementation.  The table
 * passes up the keys it has no use for; everything else goes on up.
 *
 * Inputs:
 *   event — the key event
 */
- (void)
keyDown:(NSEvent *)event
{
    NSEventModifierFlags mods = event.modifierFlags & (NSEventModifierFlagCommand |
                                                       NSEventModifierFlagControl |
                                                       NSEventModifierFlagOption |
                                                       NSEventModifierFlagShift);
    unichar key = event.charactersIgnoringModifiers.length > 0
        ? [event.charactersIgnoringModifiers characterAtIndex:0] : 0;

    if ((key == NSCarriageReturnCharacter || key == NSEnterCharacter) && mods == 0 &&
        self.table.numberOfSelectedRows == 1) {
        [self.library perform_command:TasksCommandTaskInfo param:nil];
        return;
    }
    [super keyDown:event];
}

/*
 * acceptsFirstResponder — the window gives the keyboard to "the task pane"
 * (after Escape in the search field); the pane takes it and passes it on.
 *
 * Output:
 *   YES.
 */
- (BOOL)
acceptsFirstResponder
{
    return YES;
}

/*
 * becomeFirstResponder — hand the keyboard on to the table, which is what
 * the arrow keys and the selection belong to.  On the next run-loop pass:
 * the window is still in the middle of making THIS view first responder.
 *
 * Output:
 *   YES.
 */
- (BOOL)
becomeFirstResponder
{
    __weak TasksTaskList *weak_self = self;

    dispatch_async(dispatch_get_main_queue(), ^{
        TasksTaskList *list = weak_self;
        if (list.window.firstResponder == list) [list.window makeFirstResponder:list.table];
    });
    return YES;
}

/* ── Context menu ────────────────────────────────────────────────────── */

/*
 * menuNeedsUpdate: — the rows' context menu is about to open.  AppKit does
 * not change the selection on a right-click, so do it here: a click inside
 * the selection keeps it, a click outside collapses it to the clicked row.
 * Then fill the menu with the window's shared task menu.
 *
 * Inputs:
 *   menu — the table's context menu
 */
- (void)
menuNeedsUpdate:(NSMenu *)menu
{
    NSInteger row = self.table.clickedRow;

    [menu removeAllItems];
    if (row < 0) return;
    if (![self.table isRowSelected:row]) {
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
                byExtendingSelection:NO];
    }
    NSMenu *source = [self.library task_context_menu];
    for (NSMenuItem *item in [source.itemArray copy]) {
        [source removeItem:item];
        [menu addItem:item];
    }
}

/* ── Manual-sort drag ────────────────────────────────────────────────── */

/*
 * tableView:draggingSession:willBeginAtPoint:forRowIndexes: — replace
 * NSTableView's default row-snapshot drag image with a small generic pill
 * at 10% of the table width and 65% opacity.
 *
 * Inputs:
 *   table   — the table
 *   session — the new drag session whose items are replaced
 *   point   — starting point in screen coordinates (unused)
 *   indexes — dragged row indices (unused; image is generic)
 */
- (void)
tableView:(NSTableView *)table
draggingSession:(NSDraggingSession *)session
willBeginAtPoint:(NSPoint)point
forRowIndexes:(NSIndexSet *)indexes
{
    NSInteger first = (NSInteger)indexes.firstIndex;
    if (first == NSNotFound) return;

    NSTableRowView *row_view = [table rowViewAtRow:first makeIfNecessary:NO];
    if (row_view == nil) return;

    NSRect bounds = row_view.bounds;
    NSBitmapImageRep *rep = [row_view bitmapImageRepForCachingDisplayInRect:bounds];
    if (rep == nil) return;
    [row_view cacheDisplayInRect:bounds toBitmapImageRep:rep];

    NSImage *ghost = [NSImage imageWithSize:bounds.size
                                    flipped:NO
                             drawingHandler:^BOOL(NSRect r) {
        [rep drawInRect:r fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver
               fraction:tasks_look_card_ghost_alpha
         respectFlipped:YES hints:nil];
        [NSColor.separatorColor set];
        NSFrameRect(r);
        return YES;
    }];

    [session enumerateDraggingItemsWithOptions:0
                                       forView:table
                                       classes:@[NSPasteboardItem.class]
                                 searchOptions:@{}
                                    usingBlock:^(NSDraggingItem *item, NSInteger idx, BOOL *stop) {
        item.imageComponentsProvider = ^NSArray<NSDraggingImageComponent *> *{
            NSDraggingImageComponent *comp = [NSDraggingImageComponent
                draggingImageComponentWithKey:NSDraggingImageComponentIconKey];
            comp.contents = ghost;
            comp.frame    = NSMakeRect(0, 0, bounds.size.width, bounds.size.height);
            return @[comp];
        };
        *stop = YES;
    }];
}

/*
 * tableView:pasteboardWriterForRow: — what a dragged row puts on the
 * pasteboard: its task id.  Rows are draggable when no search is active
 * (a filtered order cannot be saved safely; task_order.h).
 *
 * Inputs:
 *   table — the table (unused)
 *   row   — the row being dragged
 *
 * Output:
 *   the pasteboard item, or nil to leave the row out of the drag.
 */
- (id<NSPasteboardWriting>)
tableView:(NSTableView *)table pasteboardWriterForRow:(NSInteger)row
{
    if (![self.library manual_sort_live] || row >= [self row_count]) return nil;

    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    [item setString:@([self row_at:row]->id).stringValue forType:row_drag_type];
    return item;
}

/*
 * tableView:validateDrop:proposedRow:proposedDropOperation: — where dragged
 * rows may land.  Ignores NSTableView's proposed position and recomputes it
 * from the cursor: top 50% of a row → insert above, bottom 50% → insert
 * below.
 *
 * Inputs:
 *   table     — the table
 *   info      — the drag; draggingLocation is in window coordinates
 *   row       — NSTableView's proposal (ignored)
 *   operation — NSTableView's proposal (ignored)
 *
 * Output:
 *   move, or none to refuse the drop.
 */
- (NSDragOperation)
tableView:(NSTableView *)table
validateDrop:(id<NSDraggingInfo>)info
 proposedRow:(NSInteger)row
proposedDropOperation:(NSTableViewDropOperation)operation
{
    if (info.draggingSource != table || ![self.library manual_sort_live]) {
        return NSDragOperationNone;
    }

    NSPoint   pt      = [table convertPoint:info.draggingLocation fromView:nil];
    NSInteger hit     = [table rowAtPoint:pt];
    NSInteger target;

    if (hit < 0) {
        target = [self row_count];
    } else {
        NSRect row_rect = [table rectOfRow:hit];
        target = (pt.y < NSMidY(row_rect)) ? hit : hit + 1;
    }

    [table setDropRow:target dropOperation:NSTableViewDropAbove];
    return NSDragOperationMove;
}

/*
 * tableView:acceptDrop:row:dropOperation: — dragged rows were dropped in a
 * gap: move them there as a block, in their own order, and save the new
 * order for this view.  The dragged tasks are found by id among the rows
 * on screen NOW, so a refresh during the drag cannot move the wrong rows.
 *
 * Inputs:
 *   table     — the table
 *   info      — the drag; its pasteboard holds the dragged task ids
 *   row       — the gap, as "before this row" (the row count = the end)
 *   operation — always above (see validateDrop)
 *
 * Output:
 *   YES when the rows were moved.
 */
- (BOOL)
tableView:(NSTableView *)table
acceptDrop:(id<NSDraggingInfo>)info
       row:(NSInteger)row
dropOperation:(NSTableViewDropOperation)operation
{
    TasksLibraryWindow *library = self.library;
    int n = (int)[self row_count];

    if (![library manual_sort_live] || row < 0 || row > n) return NO;

    NSMutableSet<NSNumber *> *dragged = [NSMutableSet set];
    for (NSPasteboardItem *item in info.draggingPasteboard.pasteboardItems) {
        [dragged addObject:@([item stringForType:row_drag_type].longLongValue)];
    }

    int     *from   = malloc((size_t)(n > 0 ? n : 1) * sizeof *from);
    int64_t *ids    = malloc((size_t)(n > 0 ? n : 1) * sizeof *ids);
    int      n_from = 0;
    if (from == NULL || ids == NULL) abort();

    for (int i = 0; i < n; i++) {
        if ([dragged containsObject:@([self row_at:i]->id)]) from[n_from++] = i;
    }
    BOOL moved = n_from > 0;
    if (moved) {
        NSSet<NSNumber *> *selected = [self selected_id_set];

        task_sort_move(_order, n, from, n_from, (int)row);
        for (int i = 0; i < n; i++) ids[i] = [self row_at:i]->id;
        task_order_save(TASK_CFG_FAMILY_MANUAL, library.selection, ids, n);
        /* Clear the header sort so the drag position sticks.  Update sort
         * state directly and suppress the arrange that sortDescriptorsDidChange:
         * would trigger, so we arrange exactly once with the correct selection. */
        _n_keys = 0;
        task_config_set(TASK_CFG_LIST_SORT, NULL);
        _clearing_sort_from_drag = YES;
        self.table.sortDescriptors = @[];
        _clearing_sort_from_drag = NO;
        /* The saved order is now what rebuild_order reads back. */
        [self arrange_selecting:selected];
    }
    free(from);
    free(ids);
    return moved;
}

/* ── Commands ────────────────────────────────────────────────────────── */

/*
 * perform_command:param: — show or hide a column and remember the choice.
 * Inputs are documented in task_list.h.
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
    const column_spec_t *spec = spec_for_command(command);
    if (spec == NULL) return;

    task_config_set_bool(spec->visible_key, !task_config_get_bool(spec->visible_key));
    [self apply_settings];
    [self fit_columns_if_pending];
}

/*
 * update_state:for_command: — a column command is checked while its column
 * is visible.
 * Inputs are documented in task_list.h.
 */
- (void)
update_state:(TasksCommandState *)state for_command:(TasksCommand)command
{
    const column_spec_t *spec = spec_for_command(command);

    if (spec != NULL) state.checked = task_config_get_bool(spec->visible_key);
}

@end
