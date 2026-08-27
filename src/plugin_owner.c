/* ===========================================================================
 * plugin_owner.c — who registered what (see header)
 * =========================================================================== */

#include "plugin_owner.h"

static gchar      *current = NULL;   /* the plugin now registering, or NULL */
static GHashTable *owners  = NULL;   /* registration pointer -> owner id     */

void
task_plugin_owner_set(const gchar *id)
{
    g_free(current);
    current = g_strdup(id);
}

const gchar *
task_plugin_owner_get(void)
{
    return current;
}

void
task_plugin_owner_stamp(gpointer registration)
{
    if (current == NULL || registration == NULL)
        return;                      /* the app's own — owned by nobody     */
    if (owners == NULL)
        owners = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                       NULL, g_free);
    g_hash_table_insert(owners, registration, g_strdup(current));
}

gboolean
task_plugin_owner_is(gpointer registration, const gchar *id)
{
    if (owners == NULL || registration == NULL || id == NULL)
        return FALSE;
    const gchar *o = g_hash_table_lookup(owners, registration);
    return o != NULL && g_strcmp0(o, id) == 0;
}

void
task_plugin_owner_forget(gpointer registration)
{
    if (owners != NULL && registration != NULL)
        g_hash_table_remove(owners, registration);
}
