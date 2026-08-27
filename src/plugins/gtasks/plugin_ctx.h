/* ===========================================================================
 * plugin_ctx.h — the host table and this plugin's identity, shared across
 * the plugin's translation units.
 *
 * gtasks.c owns the definitions (set once in task_plugin_entry); oauth.c
 * and anything else in the module read them through here.  A plugin split
 * across files still has exactly one host table and one identity.
 * =========================================================================== */

#ifndef GTASKS_PLUGIN_CTX_H
#define GTASKS_PLUGIN_CTX_H

#include "plugin.h"

extern const TaskHostApi *host;
extern const TaskPlugin  *self;

#endif /* GTASKS_PLUGIN_CTX_H */
