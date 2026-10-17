#import "commands.h"
#import "ui_util.h"

/* Shorthand for the table's modifier column, as in Notes' table. */
enum {
    cmd       = NSEventModifierFlagCommand,
    cmd_shift = NSEventModifierFlagCommand | NSEventModifierFlagShift,
    cmd_ctrl  = NSEventModifierFlagCommand | NSEventModifierFlagControl,
    ctrl_opt  = NSEventModifierFlagControl | NSEventModifierFlagOption,
    app       = SharedCommandScopeApp,
};

/* Backspace as a key equivalent: ⌘⌫ shows as the delete key in the menu. */
#define KEY_BACKSPACE @"\x08"

/* The table.  Shortcuts follow Mac conventions; the modifier column is the
 * full mask, as in Notes' table.  A toggling command's title, icon and
 * tooltip here are its FIRST face: what it offers while completed tasks
 * are shown, the sidebar, the Favorites bar and the day planner are
 * visible and the list is up. */
static const shared_command_t commands[] = {
    /* ── The app delegate's ── */
    { @"about",               @"About " @TASKS_APP_NAME,  @"",  0,         app, nil, nil },
    { @"settings",            @"Settings…",               @",", cmd,       app, nil, nil },
    { @"help",                @TASKS_APP_NAME @" Help",   @"?", cmd,       app, nil, nil },

    /* ── The library window's ── */
    { @"new-task",            @"New Task",                @"n", cmd,       app, @"plus",
      @"Create a task with the selected tag" },
    /* The same shortcut is registered system-wide by hotkey.m. */
    { @"quick-task",          @"Quick Task",              @"t", cmd | ctrl_opt, app, nil, nil },
    { @"delete-task",         @"Delete Task",             KEY_BACKSPACE, cmd, app, @"trash",
      @"Delete the selected task" },
    { @"task-info",           @"Info…",                   @"i", cmd,       app, nil, nil },
    { @"clear-completed",     @"Clear Completed Tasks",   @"",  0,         app, nil, nil },
    { @"open-database",       @"Open Database File…",     @"o", cmd,       app, nil, nil },
    { @"find",                @"Find…",                   @"f", cmd,       app, nil, nil },
    { @"toggle-completed",    @"Hide Completed",          @"",  0,         app, @"eye.slash",
      @"Hide completed tasks" },
    { @"toggle-sidebar",      @"Hide Sidebar",            @"s", cmd_ctrl,  app, @"sidebar.left",
      @"Hide the sidebar" },
    { @"toggle-favorites-bar", @"Hide Favorites Bar",     @"",  0,         app, nil, nil },
    { @"toggle-compact",      @"Compact Controls",        @"",  0,         app, nil, nil },
    { @"toggle-pane",         @"Kanban View",             @"k", cmd_shift, app, @"rectangle.split.3x1",
      @"Show the tasks as a Kanban board" },
    { @"toggle-day-planner",  @"Hide Day Planner",        @"d", cmd_shift, app, @"calendar.badge.clock",
      @"Hide the day planner" },
    { @"mark-done",           @"Mark Complete",           @"",  0,         app, nil, nil },
    { @"set-priority",        @"Set High Priority",       @"",  0,         app, nil, nil },
    { @"add-tag",             @"Add Tag",                 @"",  0,         app, nil, nil },

    /* ── The sidebar's ── */
    { @"edit-tag",            @"Edit Tag…",               @"",  0,         app, nil, nil },
    { @"add-favorite",        @"Add to Favorites",        @"",  0,         app, nil, nil },
    { @"remove-favorite",     @"Remove from Favorites",   @"",  0,         app, nil, nil },

    /* ── The task list's: column visibility (check-marked items) ── */
    { @"column-done",         @"Done",                    @"",  0,         app, nil, nil },
    { @"column-status",       @"Status",                  @"",  0,         app, nil, nil },
    { @"column-due",          @"Due Date",                @"",  0,         app, nil, nil },
    { @"column-completed",    @"Completion Date",         @"",  0,         app, nil, nil },
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

/* The other three titles of a flag command.  Its first — setting the flag
 * on one task — is the command table's title. */
typedef struct {
    NSString * __unsafe_unretained ident;
    NSString * __unsafe_unretained off;         /* clear it on one task      */
    NSString * __unsafe_unretained on_all;      /* set it on several         */
    NSString * __unsafe_unretained off_all;     /* clear it on several       */
} flag_titles_t;

static const flag_titles_t flag_titles[] = {
    { @"mark-done",    @"Mark Incomplete",
      @"Mark All Complete",    @"Mark All Incomplete" },
    { @"set-priority", @"Clear High Priority",
      @"Set All High Priority", @"Clear All High Priority" },
};
#define FLAG_COUNT (sizeof(flag_titles) / sizeof(flag_titles[0]))

/*
 * tasks_commands_install — see commands.h.
 */
void
tasks_commands_install(void)
{
    shared_commands_install(commands, COMMAND_COUNT);
}

/*
 * tasks_command_flag_item — a menu item for one direction of a flag.
 * Inputs/output are documented in commands.h.
 */
NSMenuItem *
tasks_command_flag_item(NSString *ident, BOOL on, BOOL several)
{
    NSMenuItem *item = shared_command_menu_item(ident, @(on));

    for (size_t i = 0; i < FLAG_COUNT; i++) {
        const flag_titles_t *titles = &flag_titles[i];
        if (![titles->ident isEqualToString:ident]) continue;
        if (several) {
            item.title = on ? titles->on_all : titles->off_all;
        } else if (!on) {
            item.title = titles->off;
        }
    }
    return item;
}
