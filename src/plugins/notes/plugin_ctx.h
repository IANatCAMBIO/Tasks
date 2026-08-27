/* ===========================================================================
 * plugin_ctx.h — the host table and this plugin's identity, shared across
 * the plugin's translation units.
 *
 * notes.c owns the definitions (set once in task_plugin_entry); bnotes.c
 * reads them through here.  A plugin split across files still has exactly
 * one host table and one identity.
 * =========================================================================== */

#ifndef NOTES_PLUGIN_CTX_H
#define NOTES_PLUGIN_CTX_H

#include "plugin.h"

extern const TaskHostApi *host;
extern const TaskPlugin  *self;

#endif /* NOTES_PLUGIN_CTX_H */
