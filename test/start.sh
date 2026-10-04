#!/bin/sh
# Fresh launch straight into the world, driven like a player: D-pad and A only (focus snapping).
#   start.sh new (Gladiator) | load (first save slot) | daemon (imported Daemon). PROTON=1 / REAL=1 pass through.
T=$(dirname "$0")
$T/kill.sh 2>/dev/null; sleep 1; : > "$T/../logs/ctl.txt"
$T/run.sh; sleep 14
nav() { for d in "$@"; do $T/pad.sh "$d" 0.1; sleep 0.35; done; }
nav LT=0.2                                     # wake the pad (the first press only shows the focus)
case "$1" in
  load)   nav DOWN DOWN DOWN DOWN A; sleep 2   # Load Game
          nav A; sleep 0.5                     # the first save (focus moves on to Load)
          nav A; sleep 1                       # Load
          nav LEFT A; sleep 12 ;;              # OK (the pop-up starts on Cancel)
  daemon) nav A; sleep 2.5                     # Ancaria Campaign
          nav LEFT A; sleep 1                  # a hero, so that Import shows (focus moves on to Start Game)
          nav UP A; sleep 1                    # Import
          nav RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT A; sleep 1  # the rightmost pedestal: the Daemon
          nav A; sleep 12                      # Start Game
          nav RIGHT A; sleep 2 ;;              # Skip Tutorial
  *)      nav A; sleep 2.5                     # Ancaria Campaign
          nav LEFT LEFT LEFT LEFT LEFT LEFT A; sleep 1  # the leftmost pedestal: the Gladiator
          nav A; sleep 8                       # Start Game
          nav RIGHT A; sleep 2 ;;              # Skip Tutorial
esac
: > "$T/../logs/ctl.txt"
