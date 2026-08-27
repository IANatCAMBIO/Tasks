# =============================================================================
# Tasks — Makefile
#
# Builds the Tasks application (a GTK3 + SQLite task-list app written
# in plain C — the companion app to Notes).  Requires GTK3, SQLite3
# and libcurl, discovered via pkg-config.
#
# On macOS with MacPorts:
#     sudo port install pkgconf gtk3 +quartz curl
#
# Targets:
#     make          — build the `tasks` binary and its plugins
#     make plugins  — build only plugins/ from src/plugins/
#     make clean    — remove build artifacts (including dist/)
#     make run      — build and launch the app
#     make app      — macOS .app bundle → dist/Tasks.app
#                     (needs the macOS sips/iconutil tools; the bundle
#                     still depends on the MacPorts GTK libraries)
# =============================================================================

# Semantic version — read from VERSION file (the single source of truth).
# Baked into the binary as TASK_VERSION (shown in the About dialog) and into
# the .app bundle's Info.plist.  To release: edit VERSION, then `make`.
# Stated explicitly rather than left to "the first rule in the file".
# Twice now a rule defined above `all:` has silently become the default
# goal and `make` stopped building the binary — once for the `plugins`
# convenience target, once for a per-plugin rule created by $(eval).
.DEFAULT_GOAL := all

VERSION  := $(strip $(shell cat VERSION))

# The compiler to use.  clang is the system compiler on macOS.
CC       := cc

# pkg-config binary.  MacPorts installs into /opt/local/bin, which may not
# be on PATH in every shell, so fall back to the absolute path if needed.
PKGCONF  := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)

# Optional macOS menu-bar integration (MacPorts: gtk-osx-application-gtk3;
# the pkg-config module is gtk-mac-integration-gtk3).  When present, the
# Settings window offers moving the menu into the native macOS menu bar;
# without it the option shows as unavailable.  After toggling the
# dependency, run `make clean && make` so every object sees the new flags.
#
# Detected BEFORE the flags below because it joins the pkg-config module
# list rather than appending a second --libs run: gtk-mac-integration
# depends on GTK itself, so two separate runs hand the linker every GTK
# library twice ("ld: warning: ignoring duplicate libraries").  One run
# over all modules lets pkg-config collapse them.
HAVE_GTKOSX := $(shell $(PKGCONF) --exists gtk-mac-integration-gtk3 && echo 1)

# Every pkg-config module the build needs, resolved in a single query.
# NO libcurl.  The only thing that ever needed it was the Google Tasks
# sync, which is a plugin and brings its own (src/plugins/gtasks/deps.mk).
# An installation without that plugin links no network library at all —
# which is the point of the whole exercise, and is checkable with
# `otool -L tasks` / `ldd tasks`.
PKGS     := gtk+-3.0 sqlite3
ifeq ($(HAVE_GTKOSX),1)
PKGS    += gtk-mac-integration-gtk3
endif

# Compiler flags: C11, broad warnings, debug symbols, plus the include
# paths for the modules above.
CFLAGS   := -std=c11 -Wall -Wextra -g -Isrc \
            -DTASK_VERSION='"$(VERSION)"' \
            $(shell $(PKGCONF) --cflags $(PKGS))
ifeq ($(HAVE_GTKOSX),1)
CFLAGS  += -DHAVE_GTKOSX
endif

# Linker flags: those same libraries, plus libm.
LDFLAGS  := $(shell $(PKGCONF) --libs $(PKGS)) -lm

# The app's own Google OAuth client, baked into the binary so users just
# click Sync and sign in — no configuration.  One-time developer setup:
# create client_credentials.mk (gitignored) with two lines —
#   GOOGLE_CLIENT_ID     = <id>.apps.googleusercontent.com
#   GOOGLE_CLIENT_SECRET = <secret>
# from a "Desktop app" OAuth client in the Google Cloud console (Google
# Tasks API enabled).  Installed-app client secrets are not confidential
# (Google's own docs) — baking them in is the standard desktop-app
# pattern.
-include client_credentials.mk
ifneq ($(GOOGLE_CLIENT_ID),)
CFLAGS  += -DTASK_GOOGLE_CLIENT_ID='"$(GOOGLE_CLIENT_ID)"'
endif
ifneq ($(GOOGLE_CLIENT_SECRET),)
CFLAGS  += -DTASK_GOOGLE_CLIENT_SECRET='"$(GOOGLE_CLIENT_SECRET)"'
endif

# All C source files that make up the application.
SRCS     := src/main.c \
            src/app.c \
            src/task_ops.c \
            src/task_worker.c \
            src/plugin_loader.c \
            src/task_view.c \
            src/task_rows.c \
            src/task_ui.c \
            src/core_views.c \
            src/db.c \
            src/backup.c \
            src/library_window.c \
            src/editor_window.c \
            src/settings_window.c

# Object files derived from the source list (build/ mirrors src/).
OBJS     := $(SRCS:src/%.c=build/%.o)

# The final executable name.
BIN      := tasks

# --- Plugins -----------------------------------------------------------------
# Each src/plugins/<id>.c builds into plugins/<id>.<ext>, loaded at startup
# by src/plugin_loader.c.  The FILENAME is the plugin id (the loader reads
# the enabled setting from it before dlopen, so a disabled plugin is never
# mapped) and must match the id in its TaskPlugin struct.
#
# A plugin imports NOTHING from the host — everything arrives in the
# TaskHostApi table (see plugin.h) — so these link against no host object
# and need no symbol-resolution flags.  That is also why the host is NOT
# built with -rdynamic: exporting the app's symbols would cost the app a
# larger dynamic symbol table for a mechanism it does not use.
#
# -fvisibility=hidden keeps everything but task_plugin_entry (marked
# TASK_PLUGIN_EXPORT) out of the module's dynamic symbol table: a smaller
# table resolves faster under RTLD_NOW, and nothing else is callable.
# A plugin is EITHER a single src/plugins/<id>.c, or a directory
# src/plugins/<id>/ of several .c files that link into one module.  The
# second exists because a real integration is not one file: the Google
# Tasks plugin is its sync engine, its OAuth flow, an HTTP wrapper and a
# JSON parser, and splitting a plugin across files must not mean
# splitting it across modules.
PLUGIN_SRCS := $(wildcard src/plugins/*.c)
# Every directory under src/plugins/ is a plugin.
PLUGIN_PKGS := $(notdir $(patsubst %/,%,\
                 $(sort $(dir $(wildcard src/plugins/*/*.c)))))
PLUGIN_DIR  := plugins

# .so everywhere, including macOS: the extension is a build convention,
# not a format, and one name keeps the docs and the loader honest.  Only
# the LINK flag genuinely differs between the two platforms.
PLUGIN_EXT  := so
UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
PLUGIN_LDFLAGS := -dynamiclib -undefined dynamic_lookup
else
PLUGIN_LDFLAGS := -shared
endif

PLUGINS := $(patsubst src/plugins/%.c,$(PLUGIN_DIR)/%.$(PLUGIN_EXT),$(PLUGIN_SRCS)) \
           $(foreach d,$(PLUGIN_PKGS),$(PLUGIN_DIR)/$(d).$(PLUGIN_EXT))

# A plugin's README travels WITH it: the Settings list finds one by the
# convention "<id>.README.md" beside the module, which is the only way it
# can be offered for a plugin the user has switched OFF (a disabled
# plugin is never opened, so nothing inside it can be read).
PLUGIN_DOCS := $(patsubst src/plugins/%,$(PLUGIN_DIR)/%,\
                 $(wildcard src/plugins/*.README.md)) \
               $(foreach d,$(PLUGIN_PKGS),\
                 $(if $(wildcard src/plugins/$(d)/README.md),\
                   $(PLUGIN_DIR)/$(d).README.md))

$(PLUGIN_DIR)/%.README.md: src/plugins/%/README.md
	@mkdir -p $(PLUGIN_DIR)
	cp $< $@

$(PLUGIN_DIR)/%.README.md: src/plugins/%.README.md
	@mkdir -p $(PLUGIN_DIR)
	cp $< $@

# A plugin may bring its OWN dependencies: src/plugins/<id>/deps.mk, when
# present, is included and may add to that plugin's CFLAGS/LDFLAGS.  This
# is how the Google Tasks plugin asks for libcurl WITHOUT the application
# linking it — see the PKGS list above, which no longer mentions it.
define PLUGIN_PKG_RULE
-include src/plugins/$(1)/deps.mk
$$(PLUGIN_DIR)/$(1).$$(PLUGIN_EXT): $$(wildcard src/plugins/$(1)/*.c) \
                                   $$(wildcard src/plugins/$(1)/*.h) \
                                   $$(wildcard src/*.h) Makefile
	@mkdir -p $$(PLUGIN_DIR)
	$$(CC) $$(CFLAGS) $$(PLUGIN_CFLAGS_$(1)) -fPIC -fvisibility=hidden \
	      -Isrc -Isrc/plugins/$(1) $$(PLUGIN_LDFLAGS) -o $$@ \
	      $$(wildcard src/plugins/$(1)/*.c) $$(PLUGIN_LIBS_$(1))
endef
$(foreach d,$(PLUGIN_PKGS),$(eval $(call PLUGIN_PKG_RULE,$(d))))

$(PLUGIN_DIR)/%.$(PLUGIN_EXT): src/plugins/%.c $(wildcard src/*.h) Makefile
	@mkdir -p $(PLUGIN_DIR)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -Isrc \
	      $(PLUGIN_LDFLAGS) -o $@ $<

# NOTE: the `plugins` convenience target is declared BELOW `all`.  make
# builds the first target in the file, so declaring it here would make
# `make` stop building the binary.

# Default target: build the application binary (and keep the clangd
# compilation database fresh — it only regenerates on Makefile changes).
all: $(BIN) $(PLUGINS) $(PLUGIN_DOCS) compile_commands.json

# Link all object files into the final binary.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Compile each .c file into a .o in build/.  Every object depends on all
# headers for simplicity (the project is small enough that full rebuilds
# on header change are cheap), and on the Makefile so a VERSION bump
# recompiles the baked-in TASK_VERSION.
build/%.o: src/%.c $(wildcard src/*.h) Makefile VERSION $(wildcard client_credentials.mk)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- clangd / IDE support ----------------------------------------------------
# compile_commands.json gives clangd the same include paths as the real
# build (without it the IDE reports "gtk/gtk.h not found").  Regenerated
# whenever the Makefile changes; machine-specific, so it stays gitignored.
# JSONFLAGS escapes the double quotes in CFLAGS (e.g. -DTASK_VERSION='"…"')
# for embedding in a JSON string: make turns each " into \\\", the shell's
# double-quoting collapses that to \", which is what JSON needs.
JSONFLAGS := $(subst ",\\\",$(CFLAGS))

compile_commands.json: Makefile VERSION $(wildcard client_credentials.mk)
	@{ echo '['; \
	first=1; \
	for f in $(SRCS); do \
	  [ $$first -eq 1 ] || echo ','; first=0; \
	  printf '  {"directory": "%s", "file": "%s", "command": "%s -c %s"}' \
	    "$(CURDIR)" "$$f" "$(CC) $(JSONFLAGS)" "$$f"; \
	done; \
	echo; echo ']'; } > $@
	@echo "wrote $@"

# Build only the plugins.
plugins: $(PLUGINS) $(PLUGIN_DOCS)

# Build and launch the application.
run: $(BIN)
	./$(BIN)

# Remove all build artifacts.
# Removes only what this Makefile BUILT.  Note plugins/ itself is left
# alone, and only the modules and docs we produced are deleted from it:
# Settings tells the user to copy third-party plugins into that folder,
# so `rm -rf` on it would make `make clean` quietly delete something the
# app had just invited them to put there.
clean:
	rm -rf build $(BIN) $(DIST)
	rm -rf $(PLUGINS) $(PLUGIN_DOCS) $(addsuffix .dSYM,$(PLUGINS))
	-rmdir $(PLUGIN_DIR) 2>/dev/null || true

# =============================================================================
# Optional packaging targets — everything lands in dist/.
# =============================================================================

DIST     := dist

# --- macOS .app bundle -------------------------------------------------------
# A minimal bundle around the binary: icons/ and the defaults ini sit next
# to the executable inside Contents/MacOS (the app resolves both relative
# to argv[0]).  document.png becomes the bundle icon via sips + iconutil.
# The binary still links against the MacPorts GTK dylibs (absolute install
# names), so the bundle runs on this machine but is NOT self-contained.
# The live tasks.ini is NEVER copied (it holds the refresh token);
# the OAuth client json IS copied when present so Sync sign-in works from
# the bundle (installed-app client secrets are not confidential — the
# same rationale as the baked client_credentials.mk defaults).

# The bundle name carries no version: the path stays stable across
# releases, so a Dock/Launchpad entry or an alias pointing at it keeps
# working after a VERSION bump.  The version still ships INSIDE, in
# CFBundleShortVersionString/CFBundleVersion below and in the binary's
# baked-in TASK_VERSION (the About dialog).
APP_DIR  := $(DIST)/Tasks.app
ICONSET  := $(DIST)/document.iconset

app: $(BIN)
	@command -v iconutil >/dev/null || \
	  { echo "error: iconutil/sips not found — 'make app' is macOS-only"; \
	    exit 1; }
	rm -rf "$(APP_DIR)" "$(ICONSET)"
	mkdir -p "$(APP_DIR)/Contents/MacOS" "$(APP_DIR)/Contents/Resources" \
	         "$(ICONSET)"
	# The executable is named "Tasks": for NIB-less apps (the
	# gtkosx menubar is built programmatically) macOS titles the app
	# menu with the PROCESS name, not CFBundleName — the binary's
	# filename is the only lever.  argv[0]-relative lookups (icons,
	# ini, client json) resolve by directory, so the rename is harmless.
	cp $(BIN) "$(APP_DIR)/Contents/MacOS/Tasks"
	cp -R icons "$(APP_DIR)/Contents/MacOS/icons"
	cp tasks.ini.defaults "$(APP_DIR)/Contents/MacOS/"
	@if [ -f client_secret.apps.googleusercontent.com.json ]; then \
	  cp client_secret.apps.googleusercontent.com.json \
	     "$(APP_DIR)/Contents/MacOS/"; \
	fi
	find "$(APP_DIR)" -name .DS_Store -delete
	for sz in 16 32 128 256 512; do \
	  sips -z $$sz $$sz icons/document.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}.png" >/dev/null; \
	  dbl=$$((sz * 2)); \
	  sips -z $$dbl $$dbl icons/document.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}@2x.png" >/dev/null; \
	done
	iconutil -c icns -o "$(APP_DIR)/Contents/Resources/document.icns" \
	         "$(ICONSET)"
	rm -rf "$(ICONSET)"
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '  <key>CFBundleName</key><string>Tasks</string>' \
	  '  <key>CFBundleDisplayName</key><string>Tasks</string>' \
	  '  <key>CFBundleIdentifier</key><string>org.example.tasks</string>' \
	  '  <key>CFBundleExecutable</key><string>Tasks</string>' \
	  '  <key>CFBundleIconFile</key><string>document</string>' \
	  '  <key>CFBundlePackageType</key><string>APPL</string>' \
	  '  <key>CFBundleShortVersionString</key><string>$(VERSION)</string>' \
	  '  <key>CFBundleVersion</key><string>$(VERSION)</string>' \
	  '  <key>NSHighResolutionCapable</key><true/>' \
	  '</dict>' \
	  '</plist>' \
	  > "$(APP_DIR)/Contents/Info.plist"
	@echo "built $(APP_DIR)"

.PHONY: all run clean app plugins
