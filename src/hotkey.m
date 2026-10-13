#import "hotkey.h"

#import <Carbon/Carbon.h>

#include "task_config.h"

/* The shortcut, ⌃⌥⌘T.  The menu item's copy is in commands.m. */
static const UInt32 hotkey_code = kVK_ANSI_T;
static const UInt32 hotkey_mods = controlKey | optionKey | cmdKey;
static const OSType hotkey_sig  = 'TskQ';   /* names this app's hotkey */

static EventHandlerRef handler_ref;          /* NULL until started     */
static EventHotKeyRef  hotkey_ref;           /* NULL while unregistered */
static void (^fire_block)(void);

/*
 * on_hotkey — Carbon's callback for a hotkey press: run the block if the
 * press is ours.
 *
 * Inputs:
 *   call  — the handler chain (unused)
 *   event — the kEventHotKeyPressed event
 *   data  — unused
 *
 * Output:
 *   noErr when handled, eventNotHandledErr for another hotkey.
 */
static OSStatus
on_hotkey(EventHandlerCallRef call, EventRef event, void *data)
{
    EventHotKeyID id = { 0, 0 };
    GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, NULL,
                      sizeof id, NULL, &id);
    if (id.signature != hotkey_sig || fire_block == nil) return eventNotHandledErr;
    fire_block();
    return noErr;
}

/*
 * tasks_hotkey_start — install the handler, then match the setting.
 * Inputs/output are documented in hotkey.h.
 */
BOOL
tasks_hotkey_start(void (^fire)(void))
{
    if (handler_ref == NULL) {
        EventTypeSpec spec = { kEventClassKeyboard, kEventHotKeyPressed };
        InstallApplicationEventHandler(on_hotkey, 1, &spec, NULL, &handler_ref);
    }
    fire_block = fire;
    return tasks_hotkey_apply();
}

/*
 * tasks_hotkey_apply — register or unregister to match the setting.
 * Output is documented in hotkey.h.
 */
BOOL
tasks_hotkey_apply(void)
{
    if (handler_ref == NULL) return YES;

    BOOL wanted = task_config_get_bool(TASK_CFG_QUICK_TASK_HOTKEY);
    if (!wanted && hotkey_ref != NULL) {
        UnregisterEventHotKey(hotkey_ref);
        hotkey_ref = NULL;
    } else if (wanted && hotkey_ref == NULL) {
        EventHotKeyID id = { hotkey_sig, 1 };
        if (RegisterEventHotKey(hotkey_code, hotkey_mods, id, GetApplicationEventTarget(),
                                0, &hotkey_ref) != noErr) {
            hotkey_ref = NULL;
            return NO;
        }
    }
    return YES;
}

/*
 * tasks_hotkey_stop — unregister, remove the handler, drop the block.
 */
void
tasks_hotkey_stop(void)
{
    if (hotkey_ref != NULL) UnregisterEventHotKey(hotkey_ref);
    if (handler_ref != NULL) RemoveEventHandler(handler_ref);
    hotkey_ref  = NULL;
    handler_ref = NULL;
    fire_block  = nil;
}
