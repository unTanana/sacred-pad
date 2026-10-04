#!/bin/sh
# One launch of the installed game through Steam/Proton: workspace 5, never focused. Audio is left alone: never touch PipeWire streams.
hyprctl eval 'hl.window_rule({ match = { class = "^steam_app_12320$" }, workspace = "5 silent", float = true, size = { "window_w", "window_h" },
  no_initial_focus = true, focus_on_activate = false,
  suppress_event = "fullscreen maximize activate activatefocus" })' >/dev/null
steam steam://rungameid/12320 >/dev/null 2>&1 &
