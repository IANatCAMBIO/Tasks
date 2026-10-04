#import "commands.h"
#import "look.h"
#import "ui_util.h"

/* The table.  Shortcuts follow Mac conventions.  A toggling command's
 * title, icon and tooltip here are its FIRST face: what it offers while
 * completed tasks are shown, sorting is automatic, the sidebar is visible
 * and the list is up.  The library window supplies only the other face
 * (update_shell_state:for_command:). */
static const tasks_command_spec_t command_specs[] = {
    { TasksCommandAbout,           TasksCommandOwnerApp,      @"tasks.about",
      @"About " @TASKS_APP_NAME,   @"",  0, nil, nil },
    { TasksCommandSettings,        TasksCommandOwnerApp,      @"tasks.settings",
      @"Settings…",                @",", 0, nil, nil },

    { TasksCommandNewTask,         TasksCommandOwnerShell,    @"tasks.new-task",
      @"New Task",                 @"n", 0, @"plus", @"Create a task in the selected list" },
    { TasksCommandDeleteTask,      TasksCommandOwnerShell,    @"tasks.delete-task",
      @"Delete Task",              @"\x08", 0, @"trash", @"Delete the selected task" },
    { TasksCommandTaskInfo,        TasksCommandOwnerShell,    @"tasks.task-info",
      @"Info…",                    @"i", 0, nil, nil },
    { TasksCommandClearCompleted,  TasksCommandOwnerShell,    @"tasks.clear-completed",
      @"Clear Completed Tasks",    @"",  0, nil, nil },
    { TasksCommandOpenDatabase,    TasksCommandOwnerShell,    @"tasks.open-database",
      @"Open Database File…",      @"o", 0, nil, nil },
    { TasksCommandFind,            TasksCommandOwnerShell,    @"tasks.find",
      @"Find…",                    @"f", 0, nil, nil },
    { TasksCommandToggleCompleted, TasksCommandOwnerShell,    @"tasks.toggle-completed",
      @"Hide Completed",           @"",  0, @"eye.slash", @"Hide completed tasks" },
    { TasksCommandToggleSort,      TasksCommandOwnerShell,    @"tasks.toggle-sort",
      @"Manual Sorting",           @"",  0, @"line.3.horizontal", @"Switch to manual drag sorting" },
    { TasksCommandToggleSidebar,   TasksCommandOwnerShell,    @"tasks.toggle-sidebar",
      @"Hide Sidebar",             @"s", NSEventModifierFlagControl,
      @"sidebar.left",             @"Hide the lists pane" },
    { TasksCommandToggleCompact,   TasksCommandOwnerShell,    @"tasks.toggle-compact",
      @"Compact Controls",         @"",  0, nil, nil },
    { TasksCommandTogglePane,      TasksCommandOwnerShell,    @"tasks.toggle-pane",
      @"Kanban View",              @"k", NSEventModifierFlagShift,
      @"rectangle.split.3x1",      @"Show the tasks as a Kanban board" },
    { TasksCommandMarkDone,        TasksCommandOwnerShell,    @"tasks.mark-done",
      @"Mark Complete",            @"",  0, nil, nil },
    { TasksCommandSetPinned,       TasksCommandOwnerShell,    @"tasks.set-pinned",
      @"Add to Favorites",         @"",  0, nil, nil },
    { TasksCommandSetPriority,     TasksCommandOwnerShell,    @"tasks.set-priority",
      @"Set High Priority",        @"",  0, nil, nil },
    { TasksCommandMoveToList,      TasksCommandOwnerShell,    @"tasks.move-to-list",
      @"Move to List",             @"",  0, nil, nil },

    { TasksCommandNewList,         TasksCommandOwnerSidebar,  @"tasks.new-list",
      @"New List…",                @"n", NSEventModifierFlagShift, nil, nil },
    { TasksCommandNewGroup,        TasksCommandOwnerSidebar,  @"tasks.new-group",
      @"New Group…",               @"",  0, nil, nil },
    { TasksCommandEditList,        TasksCommandOwnerSidebar,  @"tasks.edit-list",
      @"Edit List…",               @"",  0, nil, nil },
    { TasksCommandDeleteList,      TasksCommandOwnerSidebar,  @"tasks.delete-list",
      @"Delete List",              @"",  0, nil, nil },
    { TasksCommandMoveToGroup,     TasksCommandOwnerSidebar,  @"tasks.move-to-group",
      @"Move to Group",            @"",  0, nil, nil },
    { TasksCommandRemoveFromGroup, TasksCommandOwnerSidebar,  @"tasks.remove-from-group",
      @"Remove from Group",        @"",  0, nil, nil },
    { TasksCommandRenameGroup,     TasksCommandOwnerSidebar,  @"tasks.rename-group",
      @"Rename Group…",            @"",  0, nil, nil },
    { TasksCommandDeleteGroup,     TasksCommandOwnerSidebar,  @"tasks.delete-group",
      @"Remove Group",             @"",  0, nil, nil },

    { TasksCommandColumnDone,      TasksCommandOwnerTaskList, @"tasks.column-done",
      @"Done",                     @"",  0, nil, nil },
    { TasksCommandColumnStatus,    TasksCommandOwnerTaskList, @"tasks.column-status",
      @"Status",                   @"",  0, nil, nil },
    { TasksCommandColumnDue,       TasksCommandOwnerTaskList, @"tasks.column-due",
      @"Due Date",                 @"",  0, nil, nil },
    { TasksCommandColumnCompleted, TasksCommandOwnerTaskList, @"tasks.column-completed",
      @"Completion Date",          @"",  0, nil, nil },
};
#define COMMAND_COUNT (sizeof(command_specs) / sizeof(command_specs[0]))

@implementation TasksCommandState
@end

/*
 * tasks_command_spec — a command's row.
 * Inputs/output are documented in commands.h.
 */
const tasks_command_spec_t *
tasks_command_spec(TasksCommand command)
{
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (command_specs[i].command == command) return &command_specs[i];
    }
    return NULL;
}

/*
 * tasks_command_with_ident — the row a toolbar identifier names.
 * Inputs/output are documented in commands.h.
 */
const tasks_command_spec_t *
tasks_command_with_ident(NSString *ident)
{
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if ([command_specs[i].ident isEqualToString:ident]) return &command_specs[i];
    }
    return NULL;
}

/*
 * tasks_command_default_state — the table's state of a command.
 * Inputs/output are documented in commands.h.
 */
TasksCommandState *
tasks_command_default_state(TasksCommand command)
{
    const tasks_command_spec_t *spec = tasks_command_spec(command);
    TasksCommandState *state = [[TasksCommandState alloc] init];

    state.enabled = spec != NULL;
    if (spec) {
        state.title   = spec->title;
        state.icon    = spec->icon;
        state.tooltip = spec->tooltip;
    }
    return state;
}

/*
 * tasks_command_menu_item — a menu item for a command.
 * Inputs/output are documented in commands.h.
 */
NSMenuItem *
tasks_command_menu_item(TasksCommand command, NSNumber *param, id target)
{
    const tasks_command_spec_t *spec = tasks_command_spec(command);
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:spec->title
                                                  action:@selector(run_command:)
                                           keyEquivalent:spec->key];

    item.keyEquivalentModifierMask = NSEventModifierFlagCommand | spec->mods;
    item.tag               = command;
    item.representedObject = param;
    item.target            = target;
    return item;
}

/*
 * tasks_command_toolbar_item — a toolbar button for a command.
 * Inputs/output are documented in commands.h.
 */
NSToolbarItem *
tasks_command_toolbar_item(TasksCommand command, id target)
{
    const tasks_command_spec_t *spec = tasks_command_spec(command);
    NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:spec->ident];

    item.label    = spec->title;
    item.tag      = command;
    item.target   = target;
    item.action   = @selector(run_command:);
    item.bordered = YES;
    tasks_command_apply(tasks_command_default_state(command), item);
    return item;
}

/*
 * tasks_command_apply — show a state on a menu or toolbar item.
 * Inputs are documented in commands.h.
 */
void
tasks_command_apply(TasksCommandState *state, id item)
{
    if ([item isKindOfClass:NSMenuItem.class]) {
        NSMenuItem *menu_item = item;
        menu_item.title   = state.title;
        menu_item.enabled = state.enabled;
        menu_item.state   = state.checked ? NSControlStateValueOn : NSControlStateValueOff;
    } else if ([item isKindOfClass:NSToolbarItem.class]) {
        NSToolbarItem *tool_item = item;
        tool_item.image   = state.icon ? tasks_icon(state.icon, tasks_look_toolbar_icon) : nil;
        tool_item.toolTip = state.tooltip;
        tool_item.enabled = state.enabled;
    }
}
