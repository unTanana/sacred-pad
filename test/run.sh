#!/bin/sh
# Launch the sandboxed game on Hyprland workspace 5 without ever taking focus.
# The window rule is runtime-only (hyprctl eval); it is not written to the user's config.
P=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$REAL" ] && exec "$P/test/protonrun.sh" # the real Steam install, DRM exe
[ -n "$PROTON" ] && exec env GAME="$P/sandbox" "$P/test/protonrun.sh" # same sandbox, Proton 11 instead of system wine
hyprctl eval 'hl.window_rule({ match = { class = "(?i)^sacred\\.exe$" }, workspace = "5 silent", float = true, size = { "window_w", "window_h" },
  no_initial_focus = true, focus_on_activate = false,
  suppress_event = "fullscreen maximize activate activatefocus" })' >/dev/null
export WINEPREFIX=$P/prefix WINEDEBUG=${WINEDEBUG:--all} SACREDPAD_CTL=$P/logs/ctl.txt
cd "$P/sandbox" && setsid wine Sacred.exe > "$P/logs/wine.log" 2>&1 < /dev/null &
