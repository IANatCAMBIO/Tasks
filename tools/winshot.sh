#!/bin/sh
# winshot.sh — screenshot one window of a running Tasks AppKit by window
# title.
#
#   tools/winshot.sh OUT.png [TITLE]
#
# Finds the process by its executable name (TasksAppKit), lists its windows
# with tools/winlist.swift, picks the one whose title matches TITLE (default
# "Tasks AppKit", the library window; an editor's title is its task's
# title), and captures it by window id with screencapture — which works
# whichever Space the window is on.  Needs the Screen Recording permission;
# where that is not available, `make ui-check` writes snapshots without it.
set -e
out=${1:?usage: winshot.sh OUT.png [TITLE]}
title=${2:-Tasks AppKit}
pid=$(pgrep -x TasksAppKit | head -1)
[ -n "$pid" ] || { echo "winshot: no running TasksAppKit process" >&2; exit 1; }
id=$(swift "$(dirname "$0")/winlist.swift" "$pid" 2>/dev/null \
     | awk -F' \\| ' -v t="$title" '$2 == t { print $1; exit }')
[ -n "$id" ] || { echo "winshot: no window titled '$title' in pid $pid" >&2; exit 1; }
screencapture -x -l "$id" "$out"
echo "$out"
