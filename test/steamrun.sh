#!/bin/sh
# One launch of the installed game through Steam/Proton: workspace 5, never focused, audio muted.
hyprctl eval 'hl.window_rule({ match = { class = "^steam_app_12320$" }, workspace = "5 silent", float = true, size = { "window_w", "window_h" },
  no_initial_focus = true, focus_on_activate = false,
  suppress_event = "fullscreen maximize activate activatefocus" })' >/dev/null
steam steam://rungameid/12320 >/dev/null 2>&1 &
# Mute every audio stream the game opens (its intro movie and music) for the next 3 minutes.
( for i in $(seq 360); do
    for id in $(pactl list short sink-inputs | cut -f1); do
      pid=$(pactl list sink-inputs | awk -v id="Sink Input #$id" '$0==id{f=1} f&&/application.process.id/{gsub(/"/,"",$3); print $3; exit}')
      [ -n "$pid" ] && grep -qa "Sacred" /proc/$pid/cmdline 2>/dev/null && pactl set-sink-input-mute "$id" 1
    done; sleep 0.5
  done ) >/dev/null 2>&1 &
