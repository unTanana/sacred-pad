#!/bin/sh
# Fresh launch straight into the world: start.sh new (Gladiator; HERO_X/HERO_Y pick another) | load (save slot 1)
# | daemon (imported level-29 Daemon). PROTON=1 / REAL=1 are passed through to run.sh.
T=$(dirname "$0")
$T/kill.sh 2>/dev/null; sleep 1; : > "$T/../logs/ctl.txt"
$T/run.sh; sleep 14
click() { $T/pad.sh "CX=$1 CY=$2 A" 0.2 "CX=$1 CY=$2"; sleep "$3"; }
case "$1" in
  load)   click 512 415 2; click 453 322 1; click 867 303 1; click 420 438 12 ;; # Load Game, slot 1, Load, OK
  daemon) click 512 288 3; click 488 663 2; click 920 600 2; click 607 713 12; click 609 511 2 ;; # Ancaria, Import, Daemon, Start, Skip
  *)      click 512 288 3; click "${HERO_X:-100}" "${HERO_Y:-440}" 2; click 607 713 8; click 609 612 2 ;; # Ancaria, hero, Start, Skip
esac
: > "$T/../logs/ctl.txt"
