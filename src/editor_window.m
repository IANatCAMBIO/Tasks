#import "editor_window.h"
#import "look.h"
#import "ui_util.h"

/*
 * STUB of the task editor (work package "editor": see
 * porting/INTERFACES.md).  It honours the frozen interface and shows the
 * window OWNERSHIP every editor follows — a registry keyed by task id that
 * holds each controller until its window closes — with the least content:
 * the task's title and status, read-only.  Nothing is edited, so there is
 * no save to flush and tasks_editor_open_new has no Cancel yet.  The
 * editor's owner replaces this file.
 */

@interface TasksEditorWindow : NSWindowController <NSWindowDelegate>
@property (nonatomic, assign) TaskApp *app;        /* borrowed; outlives every editor */
@property (nonatomic, assign) int64_t task_id;
@property (nonatomic, strong) NSTextField *summary;
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

@implementation TasksEditorWindow

/*
 * init_with_app:task_id: — build the editor window for a task.
 *
 * Inputs:
 *   app     — the application context
 *   task_id — the task to show
 *
 * Output:
 *   the controller, owned by the registry.
 */
- (instancetype)
init_with_app:(TaskApp *)app task_id:(int64_t)task_id
{
    NSWindow *window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, tasks_look_editor_w, 120)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:YES];

    self = [super initWithWindow:window];
    if (!self) return nil;

    _app     = app;
    _task_id = task_id;
    window.delegate = self;

    self.summary = [NSTextField wrappingLabelWithString:@""];
    self.summary.translatesAutoresizingMaskIntoConstraints = NO;
    [window.contentView addSubview:self.summary];
    [NSLayoutConstraint activateConstraints:@[
        [self.summary.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor
                                                   constant:tasks_look_form_margin],
        [self.summary.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor
                                                    constant:-tasks_look_form_margin],
        [self.summary.topAnchor constraintEqualToAnchor:window.contentView.topAnchor
                                               constant:tasks_look_form_margin],
    ]];
    [window center];
    return self;
}

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
    if (task == NULL) return NO;

    NSString *title = tasks_string(*task->title != '\0' ? task->title : "Untitled Task");
    self.window.title = title;
    self.summary.stringValue = [NSString stringWithFormat:@"%@\nStatus: %@\n\nTask editor (stub)",
                                title, tasks_string(task_status_label(task->status))];
    task_free(task);
    return YES;
}

/*
 * windowWillClose: — the window is closing: leave the registry, which
 * releases this controller.  That is the last thing done here, because
 * nothing else keeps `self` alive afterwards.
 *
 * Inputs:
 *   note — NSWindowWillCloseNotification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [editors() removeObjectForKey:@(self.task_id)];
}

@end

/*
 * tasks_editor_open — open or raise a task's editor.
 * Inputs are documented in editor_window.h.
 */
void
tasks_editor_open(TaskApp *app, int64_t task_id)
{
    TasksEditorWindow *editor = editors()[@(task_id)];

    if (!editor) {
        editor = [[TasksEditorWindow alloc] init_with_app:app task_id:task_id];
        if (![editor reload]) return;
        editors()[@(task_id)] = editor;
    }
    [editor showWindow:nil];
}

/*
 * tasks_editor_open_new — open the editor of a just-created task.  The stub
 * has no Cancel button, so this is tasks_editor_open.
 * Inputs are documented in editor_window.h.
 */
void
tasks_editor_open_new(TaskApp *app, int64_t task_id)
{
    tasks_editor_open(app, task_id);
}

/*
 * tasks_editor_close — close one task's editor, if open.
 * Inputs are documented in editor_window.h.
 */
void
tasks_editor_close(int64_t task_id)
{
    [editors()[@(task_id)] close];
}

/*
 * tasks_editor_refresh_all — reload every open editor; close those whose
 * task has gone.
 */
void
tasks_editor_refresh_all(void)
{
    /* A copy: closing an editor removes it from the registry. */
    for (TasksEditorWindow *editor in editors().allValues) {
        if (![editor reload]) [editor close];
    }
}

/*
 * tasks_editor_close_all — close every open editor.
 */
void
tasks_editor_close_all(void)
{
    for (TasksEditorWindow *editor in editors().allValues) [editor close];
}
