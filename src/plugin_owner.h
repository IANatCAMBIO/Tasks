/* ===========================================================================
 * plugin_owner.h — who registered what.
 *
 * Every registry in the app (views, workers, op hooks, row decorations,
 * window chrome, settings sections) accepts registrations from the app
 * ITSELF and from plugins, and cannot otherwise tell them apart.  That is
 * fine until a plugin is switched off while running, at which point its
 * registrations — and ONLY its — have to come back out.
 *
 * Rather than give every registry's entry struct an owner field (six
 * different shapes, two of which store the caller's own const struct with
 * no wrapper at all), ownership is kept HERE, keyed by the pointer the
 * registry stored.  A registry stamps on the way in and asks on the way
 * out; nothing else about it changes.
 *
 * The CURRENT owner is set by the plugin loader around a plugin's init()
 * and cleared afterwards, so a registration made by the app's own code
 * is owned by nobody and can never be swept.  A plugin that registers
 * something LATER — outside init — is likewise unowned and will not be
 * removed when it is disabled; both in-tree plugins register only from
 * init(), which is what the API asks for.
 *
 * Main thread only, like the registries themselves.
 * =========================================================================== */

#ifndef TASK_PLUGIN_OWNER_H
#define TASK_PLUGIN_OWNER_H

#include <glib.h>

/* task_plugin_owner_set() — the plugin whose registrations follow, or
 * NULL for the app's own.  The string is COPIED on each stamp, so the
 * caller may free `id` afterwards.                                       */
void task_plugin_owner_set(const gchar *id);

/* task_plugin_owner_get() — the current owner, or NULL.                  */
const gchar *task_plugin_owner_get(void);

/* task_plugin_owner_stamp() — record `registration` as belonging to the
 * current owner.  A no-op when there is none, so the app's own
 * registrations cost nothing and occupy no memory.                       */
void task_plugin_owner_stamp(gpointer registration);

/* task_plugin_owner_is() — does `registration` belong to plugin `id`?
 * FALSE for anything unstamped, which is what keeps the app's own
 * registrations out of every sweep.                                      */
gboolean task_plugin_owner_is(gpointer registration, const gchar *id);

/* task_plugin_owner_forget() — drop the record for `registration`.  Call
 * when removing it, so the table cannot outgrow the registries.          */
void task_plugin_owner_forget(gpointer registration);

#endif /* TASK_PLUGIN_OWNER_H */
