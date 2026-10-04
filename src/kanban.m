#import "kanban.h"
#import "look.h"
#import "ui_util.h"

/*
 * STUB of the Kanban board (work package "kanban": see
 * porting/INTERFACES.md).  It honours the frozen interface with the least
 * that lets the rest of the app run: a text summary of the three lanes.
 * No cards, no selection (so task commands are disabled while it is up),
 * no drag, no saved order.  The board's owner replaces this file.
 */

@interface TasksKanban ()
@property (nonatomic, weak) TasksLibraryWindow *library;
@property (nonatomic, strong) NSTextField *summary;
@end

@implementation TasksKanban

/*
 * init_with_library: — build the pane.
 * Inputs/output are documented in kanban.h.
 */
- (instancetype)
init_with_library:(TasksLibraryWindow *)library
{
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;

    _library = library;

    self.summary = [NSTextField wrappingLabelWithString:@""];
    self.summary.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:self.summary];
    [NSLayoutConstraint activateConstraints:@[
        [self.summary.leadingAnchor constraintEqualToAnchor:self.leadingAnchor
                                                   constant:tasks_look_form_margin],
        [self.summary.trailingAnchor constraintEqualToAnchor:self.trailingAnchor
                                                    constant:-tasks_look_form_margin],
        [self.summary.topAnchor constraintEqualToAnchor:self.topAnchor
                                               constant:tasks_look_form_margin],
    ]];
    return self;
}

/* ── TasksTaskPane ───────────────────────────────────────────────────── */

/*
 * show_rows: — summarise the rows by lane.  The stub keeps nothing, so the
 * rows it was handed are freed before it returns.
 * The contract is documented in library_window.h.
 */
- (void)
show_rows:(task_row_list_t *)rows
{
    NSMutableString *text = [NSMutableString stringWithString:@"Kanban board (stub)\n"];

    for (int status = 0; status < TASK_STATUS_N_VALUES; status++) {
        NSMutableArray<NSString *> *titles = [NSMutableArray array];
        for (size_t i = 0; rows != NULL && i < rows->count; i++) {
            if ((int)rows->rows[i]->status == status) {
                [titles addObject:tasks_string(rows->rows[i]->title)];
            }
        }
        [text appendFormat:@"\n%@ — %lu\n%@\n",
            tasks_string(task_status_label((TaskStatus)status)), (unsigned long)titles.count,
            [titles componentsJoinedByString:@"\n"]];
    }
    self.summary.stringValue = rows != NULL ? text : @"";
    task_row_list_free(rows);
}

/*
 * selected_task_ids — the stub has no selection.
 *
 * Output:
 *   an empty array.
 */
- (NSArray<NSNumber *> *)
selected_task_ids
{
    return @[];
}

/*
 * view_changed — nothing belongs to the old view in the stub.
 */
- (void)
view_changed
{
}

/*
 * cancel_drag — the stub has no drag to cancel.
 */
- (void)
cancel_drag
{
}

/*
 * shut_down — nothing to stop or free: the stub keeps no rows.
 */
- (void)
shut_down
{
}

@end
