#import "sidebar.h"
#import "library_window.h"
#import "look.h"
#import "ui_util.h"

/*
 * STUB of the sidebar (work package "sidebar": see porting/INTERFACES.md).
 * It honours the frozen interface with the least that lets the rest of the
 * app run: a flat list of the visible views and the lists, whose selection
 * drives the task pane.  No groups, no outline, no dialogs, no context
 * menus, no drag-reorder, no width fitting — and so every sidebar command
 * reports itself disabled.  The sidebar's owner replaces this file.
 */

static NSString * const column_ident = @"row";

@interface TasksSidebar () <NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, weak) TasksLibraryWindow *library;
@property (nonatomic, strong) NSTableView *table;
/* The rows on screen: what each says, and the selection it stands for
 * (a task_sel_t boxed in an NSValue).  Parallel arrays. */
@property (nonatomic, strong) NSMutableArray<NSString *> *labels;
@property (nonatomic, strong) NSMutableArray<NSValue *> *selections;
/* YES while reload is selecting a row itself, so that the selection
 * handler does not report it back to the window. */
@property (nonatomic, assign) BOOL reloading;
@end

@implementation TasksSidebar

/*
 * init_with_library: — build the pane.
 * Inputs/output are documented in sidebar.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library    = library;
    _labels     = [NSMutableArray array];
    _selections = [NSMutableArray array];

    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:column_ident];
    column.resizingMask   = NSTableColumnAutoresizingMask;

    self.table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    self.table.headerView                  = nil;
    self.table.style                       = NSTableViewStylePlain;
    self.table.allowsEmptySelection        = NO;
    self.table.columnAutoresizingStyle     = NSTableViewFirstColumnOnlyAutoresizingStyle;
    self.table.backgroundColor             = tasks_color(TasksColorSidebarBackground);
    self.table.dataSource                  = self;
    self.table.delegate                    = self;
    [self.table addTableColumn:column];

    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:self.bounds];
    scroll.autoresizingMask    = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.documentView        = self.table;
    [self addSubview:scroll];
    return self;
}

/*
 * add_row:kind:id: — append one row to the model.
 *
 * Inputs:
 *   label — what the row says
 *   kind  — the kind of selection it stands for
 *   ident — the view index or list id
 */
- (void)
add_row:(NSString *)label kind:(task_sel_kind_t)kind id:(int64_t)ident
{
    task_sel_t selection = { kind, ident };

    [self.labels addObject:label];
    [self.selections addObject:[NSValue valueWithBytes:&selection objCType:@encode(task_sel_t)]];
}

/*
 * selection_at: — the selection a row stands for.
 *
 * Inputs:
 *   row — a valid row index
 *
 * Output:
 *   the selection.
 */
- (task_sel_t)
selection_at:(NSInteger)row
{
    task_sel_t selection;

    [self.selections[(NSUInteger)row] getValue:&selection size:sizeof selection];
    return selection;
}

/*
 * reload — rebuild the rows and re-select the window's selection.
 * The contract is documented in sidebar.h.
 */
- (void)
reload
{
    TasksLibraryWindow *library = self.library;
    if (!library) return;

    [self.labels removeAllObjects];
    [self.selections removeAllObjects];

    for (guint i = 0; i < task_view_count(); i++) {
        const TaskView *view = task_view_nth(i);
        if (view->visible != NULL && !view->visible(library.app, view->user_data)) continue;
        [self add_row:tasks_string(view->label) kind:task_sel_view id:i];
    }
    GPtrArray *lists = task_db_lists(library.app->db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        const TaskList *list = g_ptr_array_index(lists, i);
        NSString *label = *list->emoji != '\0'
            ? [NSString stringWithFormat:@"%@  %@", tasks_string(list->emoji), tasks_string(list->name)]
            : tasks_string(list->name);
        [self add_row:label kind:task_sel_list id:list->id];
    }
    task_ptr_array_free_lists(lists);

    /* The window's selection if it still has a row, else the first row. */
    task_sel_t wanted = library.selection;
    NSInteger  found  = -1;
    for (NSInteger row = 0; row < (NSInteger)self.selections.count; row++) {
        task_sel_t candidate = [self selection_at:row];
        if (candidate.kind == wanted.kind && candidate.id == wanted.id) found = row;
    }

    self.reloading = YES;
    [self.table reloadData];
    if (self.selections.count > 0) {
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)MAX(found, 0)]
                byExtendingSelection:NO];
    }
    self.reloading = NO;
    if (found < 0 && self.selections.count > 0) [library select:[self selection_at:0]];
}

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
    return (NSInteger)self.labels.count;
}

/*
 * tableView:viewForTableColumn:row: — the label for a row.
 *
 * Inputs:
 *   table  — the table
 *   column — the single column (unused)
 *   row    — the row index
 *
 * Output:
 *   a reused or new label.
 */
- (NSView *)
tableView:(NSTableView *)table
viewForTableColumn:(NSTableColumn *)column
              row:(NSInteger)row
{
    NSTextField *label = [table makeViewWithIdentifier:column_ident owner:self];

    if (!label) {
        label               = [NSTextField labelWithString:@""];
        label.identifier    = column_ident;
        label.font          = [NSFont systemFontOfSize:tasks_look_sidebar_font];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
    }
    label.stringValue = self.labels[(NSUInteger)row];
    return label;
}

/*
 * tableViewSelectionDidChange: — the user chose a row: tell the window.
 *
 * Inputs:
 *   note — NSTableViewSelectionDidChangeNotification (unused)
 */
- (void)
tableViewSelectionDidChange:(NSNotification *)note
{
    NSInteger row = self.table.selectedRow;

    if (self.reloading || row < 0) return;
    [self.library select:[self selection_at:row]];
}

/*
 * perform_command:param: — the stub carries out no sidebar command; they
 * are all disabled by update_state:for_command:.
 * Inputs are documented in sidebar.h.
 */
- (void)
perform_command:(TasksCommand)command param:(NSNumber *)param
{
}

/*
 * update_state:for_command: — every sidebar command is unavailable in the
 * stub.
 * Inputs are documented in sidebar.h.
 */
- (void)
update_state:(TasksCommandState *)state for_command:(TasksCommand)command
{
    state.enabled = NO;
}

/*
 * shut_down — nothing to stop: the stub has no observers or timers.
 */
- (void)
shut_down
{
}

@end
