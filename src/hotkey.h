/*
 * hotkey — Quick Task's system-wide shortcut, ⌃⌥⌘T: pressed in any app,
 * it runs Quick Task in this one (a task with the default tag, its editor
 * in the corner of the screen).  Only while Tasks is running.
 *
 * Registered through Carbon's RegisterEventHotKey, the one system-wide
 * shortcut API that needs no Accessibility permission.  The menu item
 * shows the same shortcut (the Quick Task row of commands.m); while the
 * hotkey is registered it takes the key first, so the two never both run.
 * The setting TASK_CFG_QUICK_TASK_HOTKEY turns it on and off.
 *
 * Main thread only; the handler runs on the main thread.
 */

#import <Cocoa/Cocoa.h>

/*
 * tasks_hotkey_start — install the handler and register the shortcut if
 * the setting is on.  Call once at launch.
 *
 * Inputs:
 *   fire — what a press does; kept until tasks_hotkey_stop
 *
 * Output:
 *   NO when the setting is on but the shortcut could not be registered
 *   (another app holds it); YES otherwise.
 */
BOOL tasks_hotkey_start(void (^fire)(void));

/*
 * tasks_hotkey_apply — register or unregister the shortcut to match the
 * setting (Settings calls this when the checkbox changes).  Does nothing
 * before tasks_hotkey_start.
 *
 * Output:
 *   NO when the setting is on but the shortcut could not be registered;
 *   YES otherwise.
 */
BOOL tasks_hotkey_apply(void);

/*
 * tasks_hotkey_stop — unregister the shortcut, remove the handler and let
 * go of the block.  Called at quit; idempotent.
 */
void tasks_hotkey_stop(void);
