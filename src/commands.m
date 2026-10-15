#import "commands.h"
#import "look.h"
#import "ui_util.h"

/* The table.  Shortcuts follow Mac conventions.  A toggling command's
 * title, icon and tooltip here are its FIRST face: what it offers while
 * completed tasks are shown, sorting is automatic, the sidebar and the
 * Favorites bar are visible and the list is up.  The library window supplies only the other face
 * (update_shell_state:for_command:). */
static const tasks_command_spec_t command_specs[] = {
    { TasksCommandAbout,           TasksCommandOwnerApp,      @"tasks.about",
      @"About " @TASKS_APP_NAME,   @"",  0, nil, nil },
    { TasksCommandSettings,        TasksCommandOwnerApp,      @"tasks.settings",
      @"Settings…",                @",", 0, nil, nil },
    { TasksCommandHelp,            TasksCommandOwnerApp,      @"tasks.help",
      @TASKS_APP_NAME @" Help",    @"?", 0, nil, nil },

    { TasksCommandNewTask,         TasksCommandOwnerShell,    @"tasks.new-task",
      @"New Task",                 @"n", 0, @"plus",
      @"Create a task with the selected tag, or else with the default tag" },
    /* The same shortcut is registered system-wide by hotkey.m. */
    { TasksCommandQuickTask,       TasksCommandOwnerShell,    @"tasks.quick-task",
      @"Quick Task",               @"t", NSEventModifierFlagControl | NSEventModifierFlagOption,
      nil, nil },
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
    { TasksCommandToggleSidebar,   TasksCommandOwnerShell,    @"tasks.toggle-sidebar",
      @"Hide Sidebar",             @"s", NSEventModifierFlagControl,
      @"sidebar.left",             @"Hide the sidebar" },
    { TasksCommandToggleFavoritesBar, TasksCommandOwnerShell, @"tasks.toggle-favorites-bar",
      @"Hide Favorites Bar",       @"",  0, nil, nil },
    { TasksCommandToggleCompact,   TasksCommandOwnerShell,    @"tasks.toggle-compact",
      @"Compact Controls",         @"",  0, nil, nil },
    { TasksCommandTogglePane,      TasksCommandOwnerShell,    @"tasks.toggle-pane",
      @"Kanban View",              @"k", NSEventModifierFlagShift,
      @"rectangle.split.3x1",      @"Show the tasks as a Kanban board" },
    { TasksCommandToggleDayPlanner, TasksCommandOwnerShell,  @"tasks.toggle-day-planner",
      @"Hide Day Planner",          @"d", NSEventModifierFlagShift,
      @"calendar.badge.clock",      @"Hide the day planner" },
    { TasksCommandMarkDone,        TasksCommandOwnerShell,    @"tasks.mark-done",
      @"Mark Complete",            @"",  0, nil, nil },
    { TasksCommandSetPriority,     TasksCommandOwnerShell,    @"tasks.set-priority",
      @"Set High Priority",        @"",  0, nil, nil },
    { TasksCommandAddTag,          TasksCommandOwnerShell,    @"tasks.add-tag",
      @"Add Tag",                  @"",  0, nil, nil },

    { TasksCommandEditTag,         TasksCommandOwnerSidebar,  @"tasks.edit-tag",
      @"Edit Tag…",                @"",  0, nil, nil },

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

/* The other three titles of a flag command.  Its first — setting the flag
 * on one task — is the command table's title. */
typedef struct {
    TasksCommand                 command;
    NSString * __unsafe_unretained off;         /* clear it on one task      */
    NSString * __unsafe_unretained on_all;      /* set it on several         */
    NSString * __unsafe_unretained off_all;     /* clear it on several       */
} flag_titles_t;

static const flag_titles_t flag_titles[] = {
    { TasksCommandMarkDone,    @"Mark Incomplete",
      @"Mark All Complete",    @"Mark All Incomplete" },
    { TasksCommandSetPriority, @"Clear High Priority",
      @"Set All High Priority", @"Clear All High Priority" },
};
#define FLAG_COUNT (sizeof(flag_titles) / sizeof(flag_titles[0]))

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
 * tasks_command_flag_item — a menu item for one direction of a flag.
 * Inputs/output are documented in commands.h.
 */
NSMenuItem *
tasks_command_flag_item(TasksCommand command, BOOL on, BOOL several, id target)
{
    NSMenuItem *item = tasks_command_menu_item(command, @(on), target);

    for (size_t i = 0; i < FLAG_COUNT; i++) {
        const flag_titles_t *titles = &flag_titles[i];
        if (titles->command != command) continue;
        if (several) {
            item.title = on ? titles->on_all : titles->off_all;
        } else if (!on) {
            item.title = titles->off;
        }
    }
    return item;
}

/*
 * tasks_command_button — a borderless icon button for a command.
 * Inputs/output are documented in commands.h.
 */
NSButton *
tasks_command_button(TasksCommand command, id target)
{
    const tasks_command_spec_t *spec = tasks_command_spec(command);
    NSButton *button = [[NSButton alloc]
        initWithFrame:NSMakeRect(0, 0, tasks_look_float_button, tasks_look_float_button)];

    button.bordered      = NO;
    button.imagePosition = NSImageOnly;
    button.tag           = command;
    button.target        = target;
    button.action        = @selector(run_command:);
    /* A click must not take the keyboard away from the task pane. */
    button.refusesFirstResponder = YES;
    button.accessibilityLabel    = spec->title;
    button.translatesAutoresizingMaskIntoConstraints = NO;
    [button.widthAnchor constraintEqualToConstant:tasks_look_float_button].active  = YES;
    [button.heightAnchor constraintEqualToConstant:tasks_look_float_button].active = YES;
    tasks_command_apply(tasks_command_default_state(command), button);
    return button;
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
        return;
    }

    /* A toolbar item or a button: the two answer the same three getters
     * and setters.  Only a value that differs is set: the toolbar asks
     * after every event, and a toolbar item given a new image (even an
     * equal one) is redrawn.  The images are cached, so an unchanged icon
     * is the same object. */
    NSImage *icon = nil;
    if (state.icon) {
        icon = [item isKindOfClass:NSToolbarItem.class]
            ? tasks_symbol(state.icon)
            : tasks_icon(state.icon, tasks_look_toolbar_icon);
    }
    if ([item image] != icon) [item setImage:icon];
    NSString *tooltip = [item toolTip];
    if (tooltip != state.tooltip && ![tooltip isEqualToString:state.tooltip]) {
        [item setToolTip:state.tooltip];
    }
    if ([item isEnabled] != state.enabled) [item setEnabled:state.enabled];
}
