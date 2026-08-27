# The Google Tasks plugin's OWN dependencies.
#
# libcurl is needed by this plugin and by nothing else in Tasks, which is
# the whole point of it being a plugin: the application does not link a
# network library for a feature the user may not use.  The plugin asks
# for it here, and the core's PKGS list no longer mentions it.
#
# GTK, GLib and SQLite are deliberately NOT here.  They are the HOST's,
# shared across the dlopen boundary, and a second copy of any of them in
# one process is a crash or silent corruption (see plugin.h).
PLUGIN_CFLAGS_gtasks := $(shell $(PKGCONF) --cflags libcurl)
PLUGIN_LIBS_gtasks   := $(shell $(PKGCONF) --libs libcurl)
