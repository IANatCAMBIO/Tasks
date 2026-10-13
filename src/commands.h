#import <Cocoa/Cocoa.h>

/*
 * commands — the ONE table of the app's commands.  Menu items, toolbar
 * items, context-menu items and their validation are all made from it, so
 * a command's title, shortcut, icon and tooltip exist once.
 *
 * HOW A COMMAND TRAVELS
 *   - Every control made here sends ONE action, run_command:, and carries
 *     its command in `tag` (and a parameter, when the command takes one,
 *     in a menu item's representedObject as an NSNumber).
 *   - The main menu's items target the app delegate, so they work
 *     whichever window is key; it forwards to the library window.  Toolbar
 *     items, the floating buttons, context-menu items and the items of the
 *     main menu's "Move to" submenus (rebuilt each time one opens) target
 *     the library window directly.
 *   - TasksLibraryWindow perform_command:param: dispatches by the table's
 *     `owner`: its own commands, or the sidebar's, or the task list's.
 *
 * HOW A COMMAND LOOKS
 *   - TasksLibraryWindow state_of_command: is the single answer to "is it
 *     enabled, and what does it say right now".  validateMenuItem:,
 *     validateToolbarItem: and the floating buttons' update all ask it and
 *     apply the answer with tasks_command_apply, so a menu item and its
 *     button twins cannot disagree.
 *   - A toggling command's title, icon and tooltip name what a click WILL
 *     DO ("Hide Completed" while they are shown), never the state in
 *     force.  There are no check marks except on the column-visibility
 *     items.
 *   - A menu item that carries a PARAMETER keeps the title it was built
 *     with ("Mark All Incomplete", a destination list's name): validation
 *     changes only whether it is enabled.
 *   - Rename Group... and Remove Group act on a group.  A context-menu item
 *     names it in its parameter; the main menu's items carry none and mean
 *     the group selected in the sidebar.
 *
 * The Edit menu's Undo/Redo/Cut/Copy/Paste/Select All are NOT commands:
 * they use the standard selectors so that text fields get them.
 */

typedef NS_ENUM(NSInteger, TasksCommand) {
    /* Application (the app delegate) */
    TasksCommandAbout = 1,
    TasksCommandSettings,
    TasksCommandHelp,               /* open the bundled help page             */

    /* Library window (the shell) */
    TasksCommandNewTask,            /* in the selected list, else the default */
    TasksCommandQuickTask,          /* in the default list, editor in the
                                     * screen's corner; also a global hotkey  */
    TasksCommandDeleteTask,         /* the selected task(s), after a confirm  */
    TasksCommandTaskInfo,           /* open the editor; one task selected     */
    TasksCommandClearCompleted,     /* the selected list's Done tasks         */
    TasksCommandOpenDatabase,       /* pick a database file and switch to it  */
    TasksCommandFind,               /* put the cursor in the search field     */
    TasksCommandToggleCompleted,    /* Hide / Show Completed                  */
    TasksCommandToggleSidebar,      /* Hide / Show Sidebar                    */
    TasksCommandToggleCompact,      /* Compact / Full Controls                */
    TasksCommandTogglePane,         /* Kanban / List View                     */
    TasksCommandToggleDayPlanner,   /* Hide / Show Day Planner                */
    TasksCommandMarkDone,           /* param: BOOL — complete or reopen       */
    TasksCommandSetPinned,          /* param: BOOL — favourite or not         */
    TasksCommandSetPriority,        /* param: BOOL — high priority or not     */
    TasksCommandMoveToList,         /* param: destination list id             */

    /* Sidebar */
    TasksCommandNewList,
    TasksCommandNewGroup,
    TasksCommandEditList,           /* the selected list                      */
    TasksCommandDeleteList,         /* the selected list, after a confirm     */
    TasksCommandMoveToGroup,        /* param: destination group id            */
    TasksCommandRemoveFromGroup,    /* the selected list                      */
    TasksCommandRenameGroup,        /* param: group id (none: the selected)   */
    TasksCommandDeleteGroup,        /* param: group id (none: the selected),
                                     * after a confirm                        */

    /* Task list: column visibility (check-marked items) */
    TasksCommandColumnDone,
    TasksCommandColumnStatus,
    TasksCommandColumnDue,
    TasksCommandColumnCompleted,
};

/* Which part of the app carries a command out. */
typedef NS_ENUM(NSInteger, TasksCommandOwner) {
    TasksCommandOwnerApp = 0,       /* TasksAppDelegate                       */
    TasksCommandOwnerShell,         /* TasksLibraryWindow                     */
    TasksCommandOwnerSidebar,       /* TasksListsBar                          */
    TasksCommandOwnerTaskList       /* TasksTaskList                          */
};

/*
 * tasks_command_spec_t — one row of the command table.
 *
 *   command — the command
 *   owner   — who performs it
 *   ident   — stable identifier ("tasks.new-task"): the toolbar item
 *             identifier
 *   title   — menu title; for a toggling command, its default face
 *   key     — key equivalent, "" for none (Command is always added)
 *   mods    — modifiers in addition to Command
 *   icon    — SF Symbol name (tasks_icon, ui_util.h) for toolbar/floating
 *             buttons; nil when the command is never a button
 *   tooltip — hover text for buttons; nil when never a button
 */
typedef struct {
    TasksCommand                 command;
    TasksCommandOwner            owner;
    NSString * __unsafe_unretained ident;
    NSString * __unsafe_unretained title;
    NSString * __unsafe_unretained key;
    NSEventModifierFlags         mods;
    NSString * __unsafe_unretained icon;
    NSString * __unsafe_unretained tooltip;
} tasks_command_spec_t;

/*
 * TasksCommandState — how a command looks right now.  Made with the table's
 * values by tasks_command_default_state; the command's owner changes only
 * what differs (a toggle's current face, a disabled command, a check mark).
 */
@interface TasksCommandState : NSObject
@property (nonatomic, assign) BOOL enabled;
@property (nonatomic, assign) BOOL checked;         /* menu check mark        */
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy) NSString *icon;         /* SF Symbol name, or nil */
@property (nonatomic, copy) NSString *tooltip;      /* or nil                 */
@end

/*
 * tasks_command_spec — a command's row of the table.
 *
 * Inputs:
 *   command — the command
 *
 * Output:
 *   the row (static storage), or NULL for a value that is not a command.
 */
const tasks_command_spec_t *tasks_command_spec(TasksCommand command);

/*
 * tasks_command_with_ident — the command a toolbar item identifier names.
 *
 * Inputs:
 *   ident — a tasks_command_spec_t ident
 *
 * Output:
 *   the row, or NULL when no command has that identifier.
 */
const tasks_command_spec_t *tasks_command_with_ident(NSString *ident);

/*
 * tasks_command_default_state — the state the table gives a command:
 * enabled, unchecked, with the table's title, icon and tooltip.
 *
 * Inputs:
 *   command — the command
 *
 * Output:
 *   new state for the owner to adjust.
 */
TasksCommandState *tasks_command_default_state(TasksCommand command);

/*
 * tasks_command_menu_item — a menu item for a command: its title and
 * shortcut from the table, action run_command:, the command in `tag`.
 *
 * Inputs:
 *   command — the command
 *   param   — the command's parameter, or nil; rides in representedObject
 *   target  — who receives run_command: (the app delegate for the main
 *             menu, the library window for context menus)
 *
 * Output:
 *   new NSMenuItem.
 */
NSMenuItem *tasks_command_menu_item(TasksCommand command, NSNumber *param, id target);

/*
 * tasks_command_toolbar_item — a toolbar button for a command: its icon and
 * tooltip from the table, action run_command:, the command in `tag`.  The
 * toolbar re-validates it through its target's validateToolbarItem:.
 *
 * Inputs:
 *   command — a command whose table row has an icon
 *   target  — who receives run_command: (the library window)
 *
 * Output:
 *   new NSToolbarItem with the command's ident as its identifier.
 */
NSToolbarItem *tasks_command_toolbar_item(TasksCommand command, id target);

/*
 * tasks_command_flag_item — a menu item for ONE DIRECTION of a flag command
 * (TasksCommandMarkDone, TasksCommandSetPinned, TasksCommandSetPriority),
 * titled for that direction: "Mark Incomplete", "Add All to Favorites".
 * The direction rides in the item as the command's BOOL parameter.  The
 * context menu and the main menu both make their flag items here, so the
 * wording exists once.
 *
 * Inputs:
 *   command — one of the three flag commands
 *   on      — the direction: YES sets the flag, NO clears it
 *   several — NO for the wording that fits one task ("Mark Complete"), YES
 *             for the wording used when several are selected and their
 *             states may differ ("Mark All Complete")
 *   target  — who receives run_command:
 *
 * Output:
 *   new NSMenuItem.
 */
NSMenuItem *tasks_command_flag_item(TasksCommand command, BOOL on, BOOL several, id target);

/*
 * tasks_command_button — an icon-only, borderless button for a command, for
 * controls that sit over a pane (Compact mode's floating pair): its icon
 * and tooltip from the table, action run_command:, the command in `tag`.
 * Nothing validates such a button automatically: its owner applies
 * state_of_command: to it with tasks_command_apply.
 *
 * Inputs:
 *   command — a command whose table row has an icon
 *   target  — who receives run_command: (the library window)
 *
 * Output:
 *   new NSButton, tasks_look_float_button points square.
 */
NSButton *tasks_command_button(TasksCommand command, id target);

/*
 * tasks_command_apply — make a control show a command's current state.
 *
 * Inputs:
 *   state — the state to show
 *   item  — an NSMenuItem (title, enabled, check mark), or an NSToolbarItem
 *           or NSButton (icon, tooltip, enabled)
 */
void tasks_command_apply(TasksCommandState *state, id item);
