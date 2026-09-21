# =============================================================================
# Tasks — Makefile
#
# Builds the Tasks application (a GTK4 + SQLite task-list app written
# in plain C — the companion app to Notes).  Requires GTK 4.10+ and
# SQLite3, discovered via pkg-config.
#
# On macOS with MacPorts:
#     sudo port install pkgconf gtk4 +quartz
#
# Targets:
#     make          — build the `tasks` binary
#     make clean    — remove build artifacts (including dist/)
#     make run      — build and launch the app
#     make app      — macOS .app bundle → dist/Tasks.app
#                     (needs the macOS sips/iconutil tools; the bundle
#                     still depends on the MacPorts GTK libraries)
# =============================================================================

# Semantic version — read from VERSION file (the single source of truth).
# Baked into the binary as TASK_VERSION (shown in the About dialog) and into
# the .app bundle's Info.plist.  To release: edit VERSION, then `make`.
# Stated explicitly rather than left to "the first rule in the file":
# a rule defined above `all:` silently becomes the default goal and
# `make` stops building the binary.
.DEFAULT_GOAL := all

VERSION  := $(strip $(shell cat VERSION))

# The compiler to use.  clang is the system compiler on macOS.
CC       := cc

# pkg-config binary.  MacPorts installs into /opt/local/bin, which may not
# be on PATH in every shell, so fall back to the absolute path if needed.
PKGCONF  := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)

# Every pkg-config module the build needs, resolved in a single query.
# No network library: nothing in the app talks to one (checkable with
# `otool -L tasks` / `ldd tasks`).  No gtk-mac-integration either: the
# native macOS menu bar is GTK's own (gtk_application_set_menubar).
#
# NO deprecated GTK4 API is used, and no deprecation warning is
# suppressed: -Wdeprecated-declarations is part of -Wall and the build
# must stay clean under it (GTK4_MIGRATION.md).
PKGS     := gtk4 sqlite3

# Compiler flags: C11, broad warnings, debug symbols, plus the include
# paths for the modules above.
CFLAGS   := -std=c11 -Wall -Wextra -g -Isrc \
            -DTASK_VERSION='"$(VERSION)"' \
            $(shell $(PKGCONF) --cflags $(PKGS))

# Linker flags: those same libraries, plus libm.
LDFLAGS  := $(shell $(PKGCONF) --libs $(PKGS)) -lm

# All C source files that make up the application.
SRCS     := src/main.c \
            src/app.c \
            src/task_ops.c \
            src/task_worker.c \
            src/task_view.c \
            src/list_rows.c \
            src/task_rows.c \
            src/search.c \
            src/core_views.c \
            src/db.c \
            src/backup.c \
            src/recur.c \
            src/library_window.c \
            src/sidebar.c \
            src/task_list.c \
            src/kanban.c \
            src/editor_window.c \
            src/settings_window.c

# Object files derived from the source list (build/ mirrors src/).
OBJS     := $(SRCS:src/%.c=build/%.o)

# The final executable name.
BIN      := tasks

# Default target: build the application binary (and keep the clangd
# compilation database fresh — it only regenerates on Makefile changes).
all: $(BIN) compile_commands.json

# Link all object files into the final binary.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Compile each .c file into a .o in build/.  Every object depends on all
# headers for simplicity (the project is small enough that full rebuilds
# on header change are cheap), and on the Makefile so a VERSION bump
# recompiles the baked-in TASK_VERSION.
build/%.o: src/%.c $(wildcard src/*.h) Makefile VERSION
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

compile_commands.json: Makefile VERSION
	@{ echo '['; \
	first=1; \
	for f in $(SRCS); do \
	  [ $$first -eq 1 ] || echo ','; first=0; \
	  printf '  {"directory": "%s", "file": "%s", "command": "%s -c %s"}' \
	    "$(CURDIR)" "$$f" "$(CC) $(JSONFLAGS)" "$$f"; \
	done; \
	echo; echo ']'; } > $@
	@echo "wrote $@"

# Build and launch the application.
run: $(BIN)
	./$(BIN)

# --- Development sandbox -----------------------------------------------------
# `make run-dev` runs the freshly built binary against a throwaway data
# directory under dev/, never the real ~/.local/share/tasks.  It needs no
# code: task_db_default_dir() goes through g_get_user_data_dir(), which
# honours XDG_DATA_HOME on every platform, so the database, the ini and the
# backups all land in $(DEV_DATA)/tasks.
#
# A ZERO-BYTE tasks.db is seeded so the launch skips the first-run dialog
# and exercises the fresh-schema path (CLAUDE.md gotcha 21).  Delete it to
# test the first-run dialog itself; delete dev/ to start over.
DEV_DIR  := dev
DEV_DATA := $(DEV_DIR)/data

$(DEV_DATA)/tasks/tasks.db:
	mkdir -p $(dir $@)
	: > $@

run-dev: $(BIN) $(DEV_DATA)/tasks/tasks.db
	XDG_DATA_HOME=$(CURDIR)/$(DEV_DATA) ./$(BIN)

# Remove all build artifacts.
clean:
	rm -rf build $(BIN) $(DIST)

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
# The live tasks.ini is NEVER copied (it is per-machine state).

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
	# The executable is named "Tasks": for NIB-less apps (GTK builds
	# the menubar programmatically) macOS titles the app menu with the
	# PROCESS name, not CFBundleName — the binary's filename is the
	# only lever.  argv[0]-relative lookups (icons,
	# ini) resolve by directory, so the rename is harmless.
	cp $(BIN) "$(APP_DIR)/Contents/MacOS/Tasks"
	cp -R icons "$(APP_DIR)/Contents/MacOS/icons"
	cp tasks.ini.defaults "$(APP_DIR)/Contents/MacOS/"
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

.PHONY: all run run-dev clean app
