#!/bin/sh
# winshot.sh — screenshot one window of a running Tasks by window title.
#
#   tools/winshot.sh OUT.png [TITLE]
#
# Finds the process by its executable name (`tasks`, the development build),
# lists its windows with tools/winlist.swift, picks the one whose title
# matches TITLE (default "Tasks", the library window; an editor is
# "Tasks - <task title>"), and captures it by window id with screencapture
# — which works whichever Space the window is on.  macOS only.
set -e
out=${1:?usage: winshot.sh OUT.png [TITLE]}
title=${2:-Tasks}
pid=$(pgrep -x tasks | head -1)
[ -n "$pid" ] || { echo "winshot: no running 'tasks' process" >&2; exit 1; }
id=$(swift "$(dirname "$0")/winlist.swift" "$pid" 2>/dev/null \
     | awk -F' \\| ' -v t="$title" '$2 == t { print $1; exit }')
[ -n "$id" ] || { echo "winshot: no window titled '$title' in pid $pid" >&2; exit 1; }
screencapture -x -l "$id" "$out"
echo "$out"
