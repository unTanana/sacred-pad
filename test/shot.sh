#!/bin/sh
# Grab the game window via XWayland (works while it sits on a hidden workspace).
W=$(xdotool search --name '^Sacred$' | head -1)
[ -n "$W" ] && import -window "$W" "${1:-$(dirname "$0")/../logs/shot.png}"
