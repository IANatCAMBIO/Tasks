# Tasks — Makefile.  See BUILD.md for the full reference.
#
#   make          build the app bundle
#   make run      launch it against the sandbox data in dev/data
#   make seed     (re)create the sandbox test database through the core API
#   make test     the release checks: test-build test-unit test-features test-ui
#                 (test-unit: the core tests, no AppKit, no display)
#   make ui-check build the windows in-process: snapshots, ownership, memory
#   make install  copy the bundle to ~/Applications
#   make clean    remove build/ and the bundle

# ── Identity ─────────────────────────────────────────────────────────────
# The ONE place the app's names are defined.  They reach the code as the
# TASKS_* macros below and reach Info.plist through plutil; nothing else
# spells them.
APP_NAME      = Tasks
APP           = Tasks
BUNDLE_ID     = com.cambio.tasks
# Folder under ~/Library/Application Support holding the database, the ini
# and the Google client file; also names the keychain item of the Google
# sign-in.  Renaming it moves none of them: move the folder by hand (and
# sign in to Google again) when it changes.
DATA_DIR_NAME = Tasks

BUNDLE   = $(APP).app
CONTENTS = $(BUNDLE)/Contents
MACOS    = $(CONTENTS)/MacOS
RSRC     = $(CONTENTS)/Resources

CC       = clang

# The version shown in the About panel; edit VERSION to change it.
VERSION := $(strip $(shell cat VERSION))

# ── Sandbox data ─────────────────────────────────────────────────────────
# `make run`, `make seed`, `make test` and `make ui-check` ALWAYS point the
# app at these directories through TASKS_DATA_DIR (src/task_paths.h) — the
# ini, attachments and the Google client file — and at a shared database
# file inside them through CAMBIO_SHARED_DB (shared_db.h), so no
# development run ever uses the per-user data directory or the per-user
# shared database.  dev/ is gitignored.
# `override`: these feed `rm -rf` below, so a value given on the command
# line or in the environment must not be able to replace them.
override DEV_DATA      = $(CURDIR)/dev/data
override TEST_DATA     = $(CURDIR)/dev/test-data
override DEV_SHARED_DB = $(DEV_DATA)/shared_data.db
# The environment of every development run: both variables, always.
SANDBOX_ENV = TASKS_DATA_DIR="$(DEV_DATA)" CAMBIO_SHARED_DB="$(DEV_SHARED_DB)"

# ── The shared database module ───────────────────────────────────────────
# Notes and Tasks share one SQLite file; the module that owns its schema,
# its location and its backups lives in the shared_data repository beside
# this one and is compiled into the core from source.
SHARED_DATA_DIR ?= $(CURDIR)/../shared_data
SHARED_SRC       = $(SHARED_DATA_DIR)/src/shared_db.c
ifeq ($(wildcard $(SHARED_SRC)),)
$(error $(SHARED_SRC) not found: check out shared_data beside this repository or set SHARED_DATA_DIR)
endif

# The interface elements Notes and Tasks share (the tool panel, the
# sidebar's split view, the table, the status labels, ...) live in the
# shared_ui repository beside this one and are compiled in from source.
SHARED_UI_DIR ?= $(CURDIR)/../shared_ui
SHARED_UI_SRCS = $(wildcard $(SHARED_UI_DIR)/src/*.m)
ifeq ($(SHARED_UI_SRCS),)
$(error $(SHARED_UI_DIR)/src has no sources: check out shared_ui beside this repository or set SHARED_UI_DIR)
endif

# ── Dependencies ─────────────────────────────────────────────────────────
# GLib (core utilities, kept for this phase) and SQLite, both from MacPorts
# via pkg-config.  MacPorts installs pkg-config in /opt/local/bin, which is
# not on every shell's PATH.
PKGCONF    := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)
PKGS        = glib-2.0 sqlite3
PKG_CFLAGS := $(shell $(PKGCONF) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKGCONF) --libs $(PKGS))

# ── Code signing ─────────────────────────────────────────────────────────
# macOS ties privacy grants to the app's signature: for an ad-hoc ("-")
# signature that is the exact build, so every rebuild loses the grant; for
# a certificate it is the bundle ID plus certificate, which rebuilds keep.
# Defaults to the first "Apple Development" identity in the keychain, else
# ad hoc.  Override with
#   make CODESIGN_IDENTITY="…"   (list: security find-identity -v -p codesigning)
#
# Only goals that build the bundle look the identity up: clean, test, seed
# and ui-check sign nothing, so they skip the keychain query.
SIGNING_GOALS := $(if $(MAKECMDGOALS),$(filter-out clean test test-unit test-features test-ui seed ui-check,$(MAKECMDGOALS)),all)
ifneq ($(SIGNING_GOALS),)
ifeq ($(origin CODESIGN_IDENTITY),undefined)
CODESIGN_IDENTITY := $(or $(shell security find-identity -v -p codesigning 2>/dev/null \
    | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -n 1),-)
endif

# build/codesign.id records the signing identity.  When the identity
# changes it is rewritten (while the Makefile is read) and the bundle is
# backdated, which forces a re-sign; an unchanged identity triggers nothing.
$(shell mkdir -p build && (echo '$(CODESIGN_IDENTITY)' | cmp -s - build/codesign.id \
    || { echo '$(CODESIGN_IDENTITY)' > build/codesign.id; \
         [ -d $(BUNDLE) ] && touch -t 200001010000 $(BUNDLE); true; }))
endif

# ── Flags ────────────────────────────────────────────────────────────────
# DEPLOY_TARGET is the macOS version the CODE is written for: every source
# file is compiled against it, so an API newer than it is a build error.
# LINK_TARGET is the version the BINARY is linked for.  While the MacPorts
# GLib and SQLite are linked (phase one) that has to be the build machine's
# own macOS, because MacPorts builds its libraries for the system it runs
# on and the linker rejects (with a warning per library) a program that
# claims to run on anything older than its libraries.  Once GLib is gone
# the two become the same number.
DEPLOY_TARGET = 14.0
LINK_TARGET  := $(shell sw_vers -productVersion | cut -d. -f1).0

# -Werror=unguarded-availability: calling an API newer than the deployment
# target without an @available check is a build error.  (Clang cannot check
# messages to __kindof receivers such as NSApp.)
# -MMD -MP: emit build/*.d header-dependency files, so editing a header
# rebuilds every object that includes it.
DEFS     = -DTASKS_APP_NAME='"$(APP_NAME)"' \
           -DTASKS_BUNDLE_ID='"$(BUNDLE_ID)"' \
           -DTASKS_DATA_DIR_NAME='"$(DATA_DIR_NAME)"' \
           -DTASKS_APP_ICON='"$(notdir $(APP_ICON))"'
COMMON   = -O2 -Wall -Wextra -Werror=unguarded-availability \
           -mmacosx-version-min=$(DEPLOY_TARGET) -I src -I $(SHARED_DATA_DIR)/src -I $(SHARED_UI_DIR)/src \
           -MMD -MP $(DEFS) $(PKG_CFLAGS)
# The core is plain C11 and builds with every -Wextra warning on.
CFLAGS   = -std=c11 $(COMMON)
# Objective-C: ARC.  -Wno-unused-parameter because the parameters left
# unused are ones AppKit's callback signatures require (senders,
# notifications, delegate arguments), which would otherwise each need a
# (void) cast.
OBJCFLAGS = -fobjc-arc $(COMMON) -Wno-unused-parameter
# The core's own system libraries: libcurl (the SDK's, which trusts the
# keychain's certificates, so it works behind a TLS-inspecting proxy) for
# the Google Calendar sync, Security for the keychain item holding its
# sign-in, CoreFoundation under both.  The tests and the seed tool link
# the core, so they link these too.
SYS_LIBS = -lcurl -framework Security -framework CoreFoundation
LDLIBS   = -mmacosx-version-min=$(LINK_TARGET) $(PKG_LIBS) $(SYS_LIBS)
LDFLAGS  = -framework Cocoa -framework Carbon -framework UniformTypeIdentifiers -framework UserNotifications $(LDLIBS)

# ── Google OAuth client ──────────────────────────────────────────────────
# The app's OAuth client for the Google Calendar sync, baked into
# src/oauth.c so that signing in needs no setup.  client_credentials.mk
# is gitignored and optional; it holds two lines from a "Desktop app"
# OAuth client in the Google Cloud console (Google Calendar API enabled):
#   GOOGLE_CLIENT_ID     = <id>.apps.googleusercontent.com
#   GOOGLE_CLIENT_SECRET = <secret>
# Without it the app looks for the console's downloaded client file in
# the data folder instead (src/oauth.h).  Only oauth.o is built with the
# values, and it is rebuilt when the file changes.
-include client_credentials.mk
CREDENTIALS = $(wildcard client_credentials.mk)
build/oauth.o: $(CREDENTIALS)
build/oauth.o: CFLAGS += $(if $(GOOGLE_CLIENT_ID),-DTASK_GOOGLE_CLIENT_ID='"$(GOOGLE_CLIENT_ID)"') \
                         $(if $(GOOGLE_CLIENT_SECRET),-DTASK_GOOGLE_CLIENT_SECRET='"$(GOOGLE_CLIENT_SECRET)"')

# Every src/*.c is the toolkit-free core; every src/*.m is the AppKit
# layer.  A .c and a .m must not share a base name (both would build to
# the same object).  The shared database module joins the core, and so
# does shared_ui's saved-order helper (plain C in a .m: task_order.c and
# its test use it).
CORE_SRCS = $(wildcard src/*.c)
UI_SRCS   = $(wildcard src/*.m)
CORE_OBJS = $(patsubst src/%.c,build/%.o,$(CORE_SRCS)) build/shared_db.o build/shared_order.o
UI_OBJS   = $(patsubst src/%.m,build/%.o,$(UI_SRCS)) \
            $(filter-out build/shared_order.o,$(patsubst $(SHARED_UI_DIR)/src/%.m,build/%.o,$(SHARED_UI_SRCS)))
OBJS      = $(CORE_OBJS) $(UI_OBJS)

.PHONY: all run seed install clean test test-build test-unit test-features test-ui ui-check
.DEFAULT_GOAL := all

all: $(BUNDLE)

# ── Bundle ───────────────────────────────────────────────────────────────
# Info.plist gets the app's identity from the variables above, the version
# from VERSION, the minimum system version from LINK_TARGET, and the time
# the bundle was assembled as TasksBuildDate (UTC), shown in the About
# panel.
# The app's own icon is the one picture bundled: CFBundleIconFile names it
# to Finder and the Dock, TASKS_APP_ICON (above) names it to the code
# (src/ui_util.m).  The command icons are SF Symbols, which come with the
# system.  HELP_PAGE is what Help > Tasks Help opens.  CHIME is the sound a
# reminder plays (src/task_notify.m).
APP_ICON   = icons/Tasks.icns
HELP_PAGE  = resources/help.html
CHIME      = resources/tunetank.com_data-select-safely.wav
PLIST      = $(CONTENTS)/Info.plist

$(BUNDLE): $(MACOS)/$(APP) resources/Info.plist VERSION $(APP_ICON) $(HELP_PAGE) $(CHIME)
	@rm -rf $(RSRC) && mkdir -p $(RSRC)  # fresh, so removed resources do not linger
	@cp resources/Info.plist $(PLIST)
	@plutil -replace CFBundleName -string "$(APP_NAME)" $(PLIST)
	@plutil -replace CFBundleDisplayName -string "$(APP_NAME)" $(PLIST)
	@plutil -replace CFBundleIdentifier -string "$(BUNDLE_ID)" $(PLIST)
	@plutil -replace CFBundleExecutable -string "$(APP)" $(PLIST)
	@plutil -replace CFBundleIconFile -string "$(basename $(notdir $(APP_ICON)))" $(PLIST)
	@plutil -replace CFBundleShortVersionString -string "$(VERSION)" $(PLIST)
	@plutil -replace CFBundleVersion -string "$(VERSION)" $(PLIST)
	@plutil -replace LSMinimumSystemVersion -string "$(LINK_TARGET)" $(PLIST)
	@plutil -replace TasksBuildDate -string "$$(date -u +%Y-%m-%dT%H:%M:%SZ)" $(PLIST)
	@cp $(APP_ICON) $(HELP_PAGE) $(CHIME) $(RSRC)/
	@codesign --force --sign "$(CODESIGN_IDENTITY)" $(BUNDLE)
	@touch $(BUNDLE)  # codesign rewrites the binary; keep the bundle newer than it

$(MACOS)/$(APP): $(OBJS)
	@mkdir -p $(MACOS)
	$(CC) -o $@ $^ $(LDFLAGS)

# Objects also depend on the Makefile, so a flag or identity change
# rebuilds everything.
build/%.o: src/%.c Makefile | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.m Makefile | build
	$(CC) $(OBJCFLAGS) -c -o $@ $<

# The shared interface module, compiled with the app's Objective-C flags.
build/%.o: $(SHARED_UI_DIR)/src/%.m Makefile | build
	$(CC) $(OBJCFLAGS) -c -o $@ $<

build/shared_db.o: $(SHARED_SRC) Makefile | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

build:
	@mkdir -p build

# ── Sandbox run ──────────────────────────────────────────────────────────
# The binary is run directly (not through `open`) so that the sandbox
# variables reach it and its log output stays in this terminal.  The
# sandbox is seeded first if it has no database yet.
run: all
	@[ -f "$(DEV_SHARED_DB)" ] || $(MAKE) seed
	$(SANDBOX_ENV) $(MACOS)/$(APP)

# The seed tool writes a realistic test database through the core API.
# `make seed` always starts from an empty sandbox, so the result is the
# same every time.
build/seed: tools/seed.c $(CORE_OBJS) Makefile | build
	$(CC) $(CFLAGS) -c -o build/seed.o $<
	$(CC) -o $@ build/seed.o $(CORE_OBJS) $(LDLIBS)

seed: build/seed
	@rm -rf "$(DEV_DATA)" && mkdir -p "$(DEV_DATA)"
	$(SANDBOX_ENV) build/seed

# ── Tests ────────────────────────────────────────────────────────────────
# tests/*_test.c, each linked against the core objects only — no AppKit —
# and run with TASKS_DATA_DIR set to a scratch directory that is emptied
# first and CAMBIO_SHARED_DB to a file inside it; every file a test
# creates is inside it (each test then points both at a scratch folder of
# its own, tests/check.h).  CoreFoundation (in SYS_LIBS) also serves the
# run loop the main-loop tests spin.
TEST_SRCS = $(wildcard tests/*_test.c)
TEST_BINS = $(patsubst tests/%.c,build/%,$(TEST_SRCS))

build/%_test: tests/%_test.c tests/check.h tests/fixture.h $(CORE_OBJS) Makefile | build
	$(CC) $(CFLAGS) -I tests -c -o $@.o $<
	$(CC) -o $@ $@.o $(CORE_OBJS) $(LDLIBS)

test-unit: $(TEST_BINS)
	@rm -rf "$(TEST_DATA)" && mkdir -p "$(TEST_DATA)"
	@for t in $(TEST_BINS); do \
	    echo "== $$t"; TASKS_DATA_DIR="$(TEST_DATA)" \
	        CAMBIO_SHARED_DB="$(TEST_DATA)/shared_data.db" ./$$t || exit 1; \
	done
	@echo "test-unit: ok"

# ── UI check ─────────────────────────────────────────────────────────────
# tools/ui_check.m linked against every app object but main.o: it builds
# the real windows against the sandbox, writes PNG snapshots of them to
# dev/snapshots, and checks window ownership and memory from the inside
# (needs no Screen Recording, Accessibility or Developer Tools permission).
# It runs from a throwaway bundle under build/ so that the app icon is
# found the way the app finds it.  Not part of the app.
UI_CHECK_APP  = build/ui_check.app/Contents
UI_CHECK_BIN  = $(UI_CHECK_APP)/MacOS/ui_check
UI_CHECK_OBJS = $(filter-out build/main.o,$(OBJS))

$(UI_CHECK_BIN): tools/ui_check.m tests/check.h $(UI_CHECK_OBJS) $(APP_ICON) Makefile | build
	@rm -rf build/ui_check.app && mkdir -p $(UI_CHECK_APP)/MacOS $(UI_CHECK_APP)/Resources
	@cp $(APP_ICON) $(UI_CHECK_APP)/Resources/
	$(CC) $(OBJCFLAGS) -I tests -c -o build/ui_check.o $<
	$(CC) -o $@ build/ui_check.o $(UI_CHECK_OBJS) $(LDFLAGS)

ui-check: $(UI_CHECK_BIN)
	@[ -f "$(DEV_SHARED_DB)" ] || $(MAKE) seed
	$(SANDBOX_ENV) $(UI_CHECK_BIN) "$(CURDIR)/dev/snapshots"

# ── Install ──────────────────────────────────────────────────────────────
# Per-user install: no sudo needed.  Whatever Tasks.app is already in
# ~/Applications is replaced, whichever app it is.  The old bundle is
# removed first
# because `cp -R` into an existing bundle directory would nest the new one
# inside it.  A running installed copy is asked to quit (SIGTERM, which the
# app handles as a normal quit: pending edits are saved and the database is
# closed) and reopened afterwards.  It is killed (SIGKILL) if it has not
# exited after 5 s.  The installed app uses the per-user data directory, never
# the sandbox.
INSTALL_DIR   = $(HOME)/Applications
INSTALLED_BIN = $(INSTALL_DIR)/$(BUNDLE)/Contents/MacOS/$(APP)

install: all
	@mkdir -p "$(INSTALL_DIR)"
	@running=0; \
	if pgrep -qxf "$(INSTALLED_BIN)"; then \
	    running=1; \
	    pkill -TERM -xf "$(INSTALLED_BIN)"; \
	    for i in 1 2 3 4 5 6 7 8 9 10; do \
	        pgrep -qxf "$(INSTALLED_BIN)" || break; sleep 0.5; \
	    done; \
	    pkill -KILL -xf "$(INSTALLED_BIN)" 2>/dev/null; \
	    echo "Stopped the running $(APP_NAME)"; \
	fi; \
	rm -rf "$(INSTALL_DIR)/$(BUNDLE)" && \
	cp -R $(BUNDLE) "$(INSTALL_DIR)/$(BUNDLE)" && \
	echo "Installed to $(INSTALL_DIR)/$(BUNDLE)" && \
	if [ $$running = 1 ]; then open "$(INSTALL_DIR)/$(BUNDLE)" && echo "Reopened $(APP_NAME)"; fi

# ── Release checks ───────────────────────────────────────────────────────
# Not dependencies of all/run/install: they run only when asked for (a
# release). `make test` runs all four; each is also a target of its own.
test: test-build test-unit test-features test-ui
	@echo "all release checks passed"

# test-build: a from-scratch build has zero warnings, and the bundle is
# sound (valid Info.plist, executable present, signature verifies).
BUILD_LOG = build/build.log

test-build:
	@$(MAKE) clean
	@mkdir -p build
	@$(MAKE) all > $(BUILD_LOG) 2>&1 || { cat $(BUILD_LOG); exit 1; }
	@! grep -q 'warning:' $(BUILD_LOG) || { grep 'warning:' $(BUILD_LOG); echo "test-build: warnings"; exit 1; }
	@plutil -lint $(CONTENTS)/Info.plist >/dev/null
	@test -x $(MACOS)/$(APP)
	@codesign --verify --strict $(BUNDLE)
	@echo "test-build: ok"

# test-features: each user-visible feature driven through the app's own
# code on seeded scratch data (tests/features/). None written yet.
test-features:
	@echo "test-features: no feature tests yet (tests/features/)"

# test-ui: the interface check, ui-check above.
test-ui: ui-check

clean:
	@rm -rf build $(BUNDLE)
