#import "shared_commands.h"

/*
 * commands — THE table of the app's commands (commands.m), installed into
 * shared_commands.h at launch, which is the one way they are dispatched
 * and validated: menu items, toolbar items, context-menu items and the
 * floating buttons are all made FROM the table, so a command's title,
 * shortcut, icon and tooltip exist once.
 *
 * Every command is app scope: its items target the app delegate
 * (run_app_command:), so they work whichever window is key.  The delegate
 * carries out its own three (About, Settings, Help) and hands every other
 * to the library window, whose run_command: and validateUserInterfaceItem:
 * walk the handler tables of the window, the sidebar and the task list.
 *
 * A toggling command's title, icon and tooltip name what a click WILL DO
 * ("Hide Completed" while they are shown), never the state in force; the
 * table holds each one's first face and the handler's validate supplies
 * the other.  There are no check marks except on the column-visibility
 * items.  Edit Tag… acts on a tag: a context-menu item names it in its
 * parameter; the main menu's item carries none and means the tag selected
 * in the sidebar.
 */

/*
 * tasks_commands_install — install the table.  Once, at launch, before any
 * item is made from it.
 */
void tasks_commands_install(void);

/*
 * tasks_command_flag_item — a menu item for ONE DIRECTION of a flag
 * command ("mark-done", "set-priority"), titled for that direction: "Mark
 * Incomplete", "Set All High Priority".  The direction rides in the item
 * as the command's BOOL parameter.  The context menu and the main menu
 * both make their flag items here, so the wording exists once.
 *
 * Inputs:
 *   ident   — the flag command's identifier
 *   on      — the direction: YES sets the flag, NO clears it
 *   several — NO for the wording that fits one task ("Mark Complete"), YES
 *             for the wording used when several are selected and their
 *             states may differ ("Mark All Complete")
 *
 * Output:
 *   new NSMenuItem.
 */
NSMenuItem *tasks_command_flag_item(NSString *ident, BOOL on, BOOL several);
