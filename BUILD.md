# Tasks — Build Guide

## Toolchain

| Tool | Version |
|------|---------|
| C compiler | C11 (GCC 10+ or Clang 12+) |
| GTK | 4.10 or later |
| SQLite | 3.35 or later (DROP COLUMN required for migrations) |
| pkg-config | any |

`-Wall -Wextra` are always enabled and the build must stay clean.

## Getting the dependencies

### macOS (MacPorts)

```sh
sudo port install pkgconf gtk4 +quartz
```

Optionally rebuild after each port install with `make clean && make` so
every object file sees the updated pkg-config flags.

### Debian / Ubuntu

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libsqlite3-dev
```

### Fedora / RHEL

```sh
sudo dnf install gcc pkg-config gtk4-devel sqlite-devel
```

## Build targets

| Target | Description |
|--------|-------------|
| `make` | Build the `tasks` binary |
| `make run` | Build and launch against the real user data directory |
| `make run-dev` | Build and launch against the sandbox `dev/data/` directory (safe — never touches the real database) |
| `make install` | Install to `$(PREFIX)/share/tasks/` and symlink from `$(PREFIX)/bin/` |
| `make test` | Build and run the logic test suite |
| `make clean` | Remove `build/` and the binary |
| `make app` | macOS only: wrap into `dist/Tasks.app` |

## Installing

```sh
make install                    # default: PREFIX=~/.local
make install PREFIX=/usr/local  # system-wide
```

The install puts the binary, `icons/`, and `tasks.ini.defaults` under
`$(PREFIX)/share/tasks/`, then symlinks `$(PREFIX)/bin/tasks` to the
binary.  On first launch the app creates `~/.local/share/tasks/tasks.db`
and `~/.local/share/tasks/tasks.ini`.

`$(PREFIX)/bin` must be on your `PATH`.  The symlink is load-bearing: the
binary resolves `icons/` and `tasks.ini.defaults` relative to its own
location, and the symlink resolution gives the correct directory on both
Linux and macOS.

## Running the test suite

```sh
make test
```

Builds `build/test_tasks` and runs it.  The suite covers search-query
parsing and matching, recurrence arithmetic (daily/weekly/monthly
advance, lead defaults, month-end clamps), and the database schema
(fresh open, task CRUD, updated_at stamping).  No display required.

## Platform notes

### Linux / Debian / XFCE (primary)

The reference platform.  GTK4 on X11 and Wayland.  Emoji rendering uses
the system font stack; Noto Color Emoji ships with Debian and is sufficient.
For Apple-style emoji:

```sh
curl -L "https://github.com/samuelngs/apple-emoji-ttf/releases/download/macos-26-20260722-484daf4e/fonts-apple-color-emoji.deb" \
     -o fonts-apple-color-emoji.deb
sudo dpkg -i fonts-apple-color-emoji.deb
```

### macOS (Quartz, secondary)

Built against MacPorts GTK4 (`+quartz` variant).  The resulting binary and
`.app` bundle depend on the MacPorts GTK libraries — it runs on the build
machine but is not self-contained.

`make app` wraps the binary into `dist/Tasks.app`.  The bundle icon is
converted from `icons/document.png` with `sips` and `iconutil`
(macOS-only tools), so `make app` is macOS-only.

Known macOS behavior differences (not bugs):
- Editor windows resize upward — AppKit anchors `NSWindow` at
  bottom-left; `gtk_window_move` was removed in GTK4, so this cannot
  be corrected at the application level.

### SQLite version

SQLite 3.35 or later is required for `DROP COLUMN` (used in schema
migrations).  The build does not check for this; the app will fail at
first run with a migration error on an older SQLite.  Check with:

```sh
sqlite3 --version
```

## File layout

```
tasks/
├── src/               C sources and headers
├── icons/             PNG icons loaded at runtime (relative to the binary)
├── tasks.ini.defaults Default configuration (relative to the binary)
├── tests/             Logic test sources
├── build/             Object files and test binary (generated, gitignored)
├── dev/               Sandbox data directory for make run-dev (gitignored)
├── dist/              Packaged output: Tasks.app (generated, gitignored)
├── Makefile
├── VERSION
├── README.md
└── BUILD.md           (this file)
```
